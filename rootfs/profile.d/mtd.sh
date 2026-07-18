#!/bin/sh

PATH=/usr/sbin:/sbin:/usr/bin:/bin
UBIFS_MOUNT=/mnt/ubifs
UBIFS_VOL_NAME=rootfs

mtd_load_simulators() {
  modprobe qemu_3dnand 2>/dev/null || true
  modprobe mtdram total_size=32768 erase_size=128 2>/dev/null || true
  modprobe nandsim first_id_byte=0x20 second_id_byte=0xaa third_id_byte=0x00 fourth_id_byte=0x15 2>/dev/null || true
}

mtd_load_q3n() {
  modprobe qemu_3dnand || return 1
  cat /proc/mtd
  grep -q '"qemu-3dnand"' /proc/mtd
}

mtd_q3n_stats() {
  modprobe qemu_3dnand 2>/dev/null || true
  for f in raid_recovered raid_failed parity_stale parity_written generation_updates faults_injected; do
    if [ -r "/sys/kernel/debug/qemu_3dnand/$f" ]; then
      printf '%s=' "$f"
      cat "/sys/kernel/debug/qemu_3dnand/$f"
    fi
  done
}

mtd_q3n_inject_loss() {
  addr=${1:-}
  [ -n "$addr" ] || {
    echo "用法: mtd.sh q3n-inject-loss <logical-byte-address>"
    return 1
  }
  echo "$addr" > /sys/kernel/debug/qemu_3dnand/inject_data_loss
}

mtd_find_q3n() {
  sed -n 's/^mtd\([0-9][0-9]*\):.*"qemu-3dnand"$/\1/p' /proc/mtd | head -n 1
}

mtd_q3n_serial_smoke() {
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  stats=/sys/kernel/debug/qemu_3dnand
  parity_written_before=$(cat "$stats/parity_written") || return 1
  raid_recovered_before=$(cat "$stats/raid_recovered") || return 1

  flash_erase -q "$mtd_dev" 0 1 || return 1
  # Cross the first parity barrier without waiting: D0..D6,P,D0.
  dd if=/dev/zero of=/tmp/q3n-data.bin bs=16384 count=8 2>/dev/null || return 1
  dd if=/tmp/q3n-data.bin of="$mtd_dev" bs=16384 count=8 2>/dev/null || return 1
  tries=0
  parity_written_after=$(cat "$stats/parity_written") || return 1
  while [ "$parity_written_after" -le "$parity_written_before" ] && [ "$tries" -lt 20 ]; do
    sleep 1
    tries=$((tries + 1))
    parity_written_after=$(cat "$stats/parity_written") || return 1
  done
  [ "$parity_written_after" -gt "$parity_written_before" ] || return 1

  echo 0 > "$stats/inject_data_loss" || return 1
  dd if="$mtd_dev" of=/tmp/q3n-recovered.bin bs=16384 count=1 2>/dev/null || return 1
  cmp /tmp/q3n-data.bin /tmp/q3n-recovered.bin -n 16384 || return 1
  raid_recovered_after=$(cat "$stats/raid_recovered") || return 1
  [ "$raid_recovered_after" -gt "$raid_recovered_before" ] || return 1
  echo "q3n serial smoke passed: parity=$parity_written_after recovered=$raid_recovered_after"
}

mtd_q3n_markbad_smoke() {
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  erasesize=$(cat "/sys/class/mtd/mtd${mtd_num}/erasesize") || return 1
  offset=$erasesize
  page_seek=$((erasesize / 16384))

  mtd_badblock set "$mtd_dev" "$offset" >/dev/null || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$offset")" = "1" ] || return 1
  dd if=/dev/zero of=/tmp/q3n-bad-page.bin bs=16384 count=1 \
    2>/dev/null || return 1
  if dd if=/tmp/q3n-bad-page.bin of="$mtd_dev" bs=16384 count=1 \
       seek="$page_seek" 2>/tmp/q3n-bad-write.err; then
    echo "q3n markbad smoke: write to bad block unexpectedly succeeded"
    return 1
  fi
  flash_erase -q -N "$mtd_dev" "$offset" 1 \
    >/tmp/q3n-bad-erase.err 2>&1 || true
  grep -q 'MTD Erase failure' /tmp/q3n-bad-erase.err || {
    echo "q3n markbad smoke: erase of bad block unexpectedly succeeded"
    return 1
  }
  mtd_badblock set "$mtd_dev" "$offset" >/dev/null || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$offset")" = "1" ] || return 1
  echo "q3n markbad smoke passed"
}

mtd_q3n_generation_smoke() {
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  stats=/sys/kernel/debug/qemu_3dnand
  parity_stale_before=$(cat "$stats/parity_stale") || return 1
  raid_failed_before=$(cat "$stats/raid_failed") || return 1

  flash_erase -q "$mtd_dev" 0 1 || return 1
  dd if=/dev/zero of=/tmp/q3n-generation.bin bs=16384 count=7 2>/dev/null || return 1
  dd if=/tmp/q3n-generation.bin of="$mtd_dev" bs=16384 count=7 2>/dev/null || return 1
  flash_erase -q "$mtd_dev" 0 1 || return 1

  tries=0
  parity_stale_after=$(cat "$stats/parity_stale") || return 1
  while [ "$parity_stale_after" -le "$parity_stale_before" ] && [ "$tries" -lt 20 ]; do
    sleep 1
    tries=$((tries + 1))
    parity_stale_after=$(cat "$stats/parity_stale") || return 1
  done
  raid_failed_after=$(cat "$stats/raid_failed") || return 1
  [ "$parity_stale_after" -gt "$parity_stale_before" ] || return 1
  [ "$raid_failed_after" -eq "$raid_failed_before" ] || return 1
  echo "q3n generation smoke passed: stale=$parity_stale_after failed=$raid_failed_after"
}

mtd_q3n_cancel_cleanup() {
  echo 0 > "$stats/parity_pause_enable" 2>/dev/null || true
  if [ "${cancel_writer_owned:-0}" -eq 1 ] &&
     [ -n "${cancel_writer_pid:-}" ]; then
    wait "$cancel_writer_pid" 2>/dev/null || true
  fi
  cancel_writer_owned=0 cancel_writer_pid=
}

mtd_q3n_cancel_barrier_smoke() {
  mtd_load_q3n || {
    echo "q3n cancel barrier smoke: failed to load qemu_3dnand"
    return 1
  }
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || {
    echo "q3n cancel barrier smoke: qemu-3dnand MTD not found"
    return 1
  }
  mtd_dev="/dev/mtd${mtd_num}"
  stats=/sys/kernel/debug/qemu_3dnand
  for counter in parity_pause_block parity_pause_enable parity_paused \
                 parity_written raid_failed pending_parity reserved_parity; do
    [ -e "$stats/$counter" ] || {
      echo "q3n cancel barrier smoke: missing $stats/$counter"
      return 1
    }
  done

  # The EXIT trap is deliberately installed before enabling the pause.  Every
  # subsequent error path therefore wakes a worker that may be stopped at the
  # deterministic pre-claim test point.
  cancel_writer_pid= cancel_writer_owned=0
  trap 'mtd_q3n_cancel_cleanup' 0
  trap 'exit 1' HUP INT TERM

  echo 0 > "$stats/parity_pause_block" || {
    echo "q3n cancel barrier smoke: failed to select block 0"
    return 1
  }
  echo 1 > "$stats/parity_pause_enable" || {
    echo "q3n cancel barrier smoke: failed to enable parity pause"
    return 1
  }
  flash_erase -q "$mtd_dev" 0 1 || {
    echo "q3n cancel barrier smoke: initial block erase failed"
    return 1
  }
  dd if=/dev/zero of=/tmp/q3n-cancel.bin bs=16384 count=7 2>/dev/null || {
    echo "q3n cancel barrier smoke: failed to create input"
    return 1
  }
  # Closing an MTD character-device fd invokes _sync(), which intentionally
  # flushes the parity workqueue.  Keep dd in the background so the parent can
  # observe the paused pre-claim worker and issue the erase that cancels it.
  dd if=/tmp/q3n-cancel.bin of="$mtd_dev" bs=16384 count=7 2>/tmp/q3n-cancel-dd.err &
  cancel_writer_pid=$! cancel_writer_owned=1

  tries=0
  parity_paused=$(cat "$stats/parity_paused") || return 1
  while [ "$parity_paused" -le 0 ] && [ "$tries" -lt 20 ]; do
    sleep 1
    tries=$((tries + 1))
    parity_paused=$(cat "$stats/parity_paused") || return 1
  done
  [ "$parity_paused" -gt 0 ] || {
    echo "q3n cancel barrier smoke: parity worker did not pause within ${tries}s"
    return 1
  }

  written_before=$(cat "$stats/parity_written") || return 1
  failed_before=$(cat "$stats/raid_failed") || return 1
  flash_erase -q "$mtd_dev" 0 1 || {
    echo "q3n cancel barrier smoke: cancel-barrier erase failed"
    return 1
  }
  if wait "$cancel_writer_pid"; then
    cancel_writer_owned=0 cancel_writer_pid=
  else
    cancel_writer_owned=0 cancel_writer_pid=
    echo "q3n cancel barrier smoke: data writer failed after cancellation"
    cat /tmp/q3n-cancel-dd.err 2>/dev/null || true
    return 1
  fi
  written_after=$(cat "$stats/parity_written") || return 1
  failed_after=$(cat "$stats/raid_failed") || return 1
  pending_after=$(cat "$stats/pending_parity") || return 1
  reserved_after=$(cat "$stats/reserved_parity") || return 1

  [ "$written_after" -eq "$written_before" ] || {
    echo "q3n cancel barrier smoke: parity_written changed ($written_before -> $written_after)"
    return 1
  }
  [ "$failed_after" -eq "$failed_before" ] || {
    echo "q3n cancel barrier smoke: raid_failed changed ($failed_before -> $failed_after)"
    return 1
  }
  [ "$pending_after" -eq 0 ] || {
    echo "q3n cancel barrier smoke: pending_parity=$pending_after, expected 0"
    return 1
  }
  [ "$reserved_after" -eq 0 ] || {
    echo "q3n cancel barrier smoke: reserved_parity=$reserved_after, expected 0"
    return 1
  }

  echo 0 > "$stats/parity_pause_enable" || {
    echo "q3n cancel barrier smoke: failed to disable parity pause"
    return 1
  }
  trap - 0 HUP INT TERM

  mtd_q3n_serial_smoke || {
    echo "q3n cancel barrier smoke: new-generation serial recovery failed"
    return 1
  }
  echo "q3n cancel barrier smoke passed: paused=$parity_paused parity=$written_after failed=$failed_after pending=$pending_after reserved=$reserved_after"
}

mtd_find_nandsim() {
  while IFS= read -r line; do
    case "$line" in
      mtd*\"NAND\ simulator*)
        dev=${line%%:*}
        echo "${dev#mtd}"
        return 0
        ;;
    esac
  done < /proc/mtd

  return 1
}

mtd_smoke() {
  echo "加载 MTD 模拟模块"
  mtd_load_simulators

  echo "当前 MTD 设备:"
  cat /proc/mtd 2>/dev/null || {
    echo "未找到 /proc/mtd"
    return 1
  }

  if command -v ubiattach >/dev/null 2>&1 && grep -q '^mtd[0-9]' /proc/mtd; then
    echo "检测到 ubiattach，可手动执行 UBI/UBIFS 测试"
  fi
}

mtd_ubifs() {
  echo "加载 nandsim/ubi/ubifs"
  mtd_load_simulators
  modprobe deflate 2>/dev/null || true
  modprobe zlib_deflate 2>/dev/null || true
  modprobe zstd 2>/dev/null || true
  modprobe zstd_compress 2>/dev/null || true
  modprobe ubi 2>/dev/null || true
  modprobe ubifs || return 1

  cat /proc/mtd

  mtd_num=$(mtd_find_nandsim) || {
    echo "未找到 NAND simulator MTD 设备"
    return 1
  }
  mtd_dev="/dev/mtd${mtd_num}"

  [ -e "$mtd_dev" ] || {
    echo "缺少 $mtd_dev"
    return 1
  }

  mkdir -p "$UBIFS_MOUNT"
  umount "$UBIFS_MOUNT" 2>/dev/null || true
  ubidetach -m "$mtd_num" 2>/dev/null || true

  echo "擦除并格式化 $mtd_dev"
  flash_erase -q "$mtd_dev" 0 0 || return 1
  ubiformat -q "$mtd_dev" -y || return 1

  echo "attach UBI: mtd$mtd_num"
  ubiattach /dev/ubi_ctrl -m "$mtd_num" || return 1

  echo "创建 UBI volume: $UBIFS_VOL_NAME"
  ubimkvol /dev/ubi0 -N "$UBIFS_VOL_NAME" -m || return 1

  echo "挂载 UBIFS: ubi0:$UBIFS_VOL_NAME -> $UBIFS_MOUNT"
  mount -t ubifs "ubi0:$UBIFS_VOL_NAME" "$UBIFS_MOUNT" || return 1
  echo "hello from ubifs" > "$UBIFS_MOUNT/hello.txt" || return 1
  cat "$UBIFS_MOUNT/hello.txt" || return 1

  echo "UBIFS 已挂载:"
  mount | grep "$UBIFS_MOUNT" || return 1
}

mtd_clean() {
  mtd_num=$(mtd_find_nandsim 2>/dev/null || true)

  umount "$UBIFS_MOUNT" 2>/dev/null || true
  if [ -n "$mtd_num" ]; then
    ubidetach -m "$mtd_num" 2>/dev/null || true
  fi

  rmmod ubifs 2>/dev/null || true
  rmmod ubi 2>/dev/null || true
  rmmod nandsim 2>/dev/null || true
  rmmod nand 2>/dev/null || true
  rmmod mtdram 2>/dev/null || true
}

case "${1:-}" in
  smoke) mtd_smoke ;;
  q3n) mtd_load_q3n ;;
  q3n-stats) mtd_q3n_stats ;;
  q3n-inject-loss) mtd_q3n_inject_loss "${2:-}" ;;
  q3n-serial-smoke) mtd_q3n_serial_smoke ;;
  q3n-generation-smoke) mtd_q3n_generation_smoke ;;
  q3n-cancel-barrier-smoke) mtd_q3n_cancel_barrier_smoke ;;
  q3n-markbad-smoke) mtd_q3n_markbad_smoke ;;
  nandsim) mtd_load_simulators; cat /proc/mtd ;;
  ubifs) mtd_ubifs ;;
  clean) mtd_clean ;;
esac
