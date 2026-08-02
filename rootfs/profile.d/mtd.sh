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

mtd_q3n_parity_stats() {
  mtd_load_q3n || return 1
  stats=/sys/kernel/debug/qemu_3dnand
  for counter in foreground_ops parity_reads parity_writes \
                 protected_stripes unprotected_stripes failed_stripes \
                 pending_parity max_pending_parity; do
    [ -r "$stats/$counter" ] || {
      echo "q3n parity stats: missing $stats/$counter"
      return 1
    }
    printf '%s=' "$counter"
    cat "$stats/$counter"
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

mtd_smoke_command_for() {
  case "${1:-}" in
    q3n-serial-smoke) printf '%s\n' mtd_q3n_serial_smoke ;;
    q3n-multiplane-smoke) printf '%s\n' mtd_q3n_multiplane_smoke ;;
    q3n-multiplane-persist-prepare) printf '%s\n' mtd_q3n_multiplane_persist_prepare ;;
    q3n-multiplane-persist-verify) printf '%s\n' mtd_q3n_multiplane_persist_verify ;;
    q3n-persist-prepare) printf '%s\n' mtd_q3n_persist_prepare ;;
    q3n-persist-verify) printf '%s\n' mtd_q3n_persist_verify ;;
    ubifs) printf '%s\n' mtd_ubifs ;;
    1) printf '%s\n' mtd_smoke ;;
    *) return 1 ;;
  esac
}

mtd_q3n_multiplane_cleanup() {
  rm -f /tmp/q3n-multiplane-*
}

mtd_q3n_multiplane_expect() {
  mp_actual=$1 mp_expected=$2 mp_name=$3
  [ "$mp_actual" = "$mp_expected" ] || {
    echo "q3n multi-plane smoke: $mp_name=$mp_actual, expected $mp_expected"
    return 1
  }
}

mtd_q3n_multiplane_all_bbm_zero() {
  mp_offset=$1 mp_slot=0
  while [ "$mp_slot" -lt 4 ]; do
    mp_bbm=$(mtd_badblock oob-read raw "$mtd_dev" "$mp_offset" \
      $((mp_slot * 1024)) 1) || return 1
    [ "$mp_bbm" = 00 ] || {
      echo "q3n multi-plane smoke: BBM[$mp_slot]=$mp_bbm, expected 00"
      return 1
    }
    mp_slot=$((mp_slot + 1))
  done
}

mtd_q3n_multiplane_smoke() {
  trap 'mtd_q3n_multiplane_cleanup' 0 HUP INT TERM
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  mp_sys="/sys/class/mtd/mtd${mtd_num}"

  writesize=$(cat "$mp_sys/writesize") || return 1
  oobsize=$(cat "$mp_sys/oobsize") || return 1
  oobavail=$(cat "$mp_sys/oobavail") || return 1
  erasesize=$(cat "$mp_sys/erasesize") || return 1
  mtd_size=$(cat "$mp_sys/size") || return 1
  mtd_q3n_multiplane_expect "$writesize" 65536 writesize || return 1
  mtd_q3n_multiplane_expect "$oobsize" 4096 oobsize || return 1
  mtd_q3n_multiplane_expect "$oobavail" 4092 oobavail || return 1
  mtd_q3n_multiplane_expect "$erasesize" 104857600 erasesize || return 1
  mtd_q3n_multiplane_expect "$mtd_size" 43620761600 size || return 1
  mtd_q3n_multiplane_expect "$((mtd_size / erasesize))" 416 blocks || return 1
  echo "q3n multi-plane geometry writesize=$writesize oobsize=$oobsize oobavail=$oobavail erasesize=$erasesize size=$mtd_size blocks=416"

  page1599_offset=$((1599 * writesize))
  page1600_offset=$erasesize
  last_block_offset=$((415 * erasesize))
  last_page_offset=$((665599 * writesize))
  bbm_fold_offset=$((2 * erasesize))
  markbad_offset=$((3 * erasesize))

  echo "q3n multi-plane stage: first and boundary pages"
  flash_erase -q "$mtd_dev" 0 1 || return 1
  mtd_badblock page-write raw "$mtd_dev" 0 0x31 128 1 0xa1 || return 1
  mtd_badblock page-read raw "$mtd_dev" 0 0x31 128 1 0xa1 || return 1
  mtd_badblock page-pattern-write place "$mtd_dev" "$page1599_offset" \
    0x32 0xa2 || return 1
  mtd_badblock page-pattern-read place "$mtd_dev" "$page1599_offset" \
    0x32 0xa2 || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$page1599_offset")" = 0 ] || return 1

  flash_erase -q "$mtd_dev" "$page1600_offset" 1 || return 1
  mtd_badblock page-pattern-write raw "$mtd_dev" "$page1600_offset" \
    0x33 0xa3 || return 1
  mtd_badblock page-pattern-read raw "$mtd_dev" "$page1600_offset" \
    0x33 0xa3 || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$page1600_offset")" = 0 ] || return 1

  flash_erase -q "$mtd_dev" "$last_block_offset" 1 || return 1
  mtd_badblock page-write raw "$mtd_dev" "$last_page_offset" \
    0x34 128 1 0xa4 || return 1
  mtd_badblock page-read raw "$mtd_dev" "$last_page_offset" \
    0x34 128 1 0xa4 || return 1

  echo "q3n multi-plane stage: folded BBM"
  flash_erase -q "$mtd_dev" "$bbm_fold_offset" 1 || return 1
  echo "q3n multi-plane stage: raw BBM write"
  mtd_badblock oob-write raw "$mtd_dev" "$bbm_fold_offset" \
    1024 1 0x00 || return 1
  echo "q3n multi-plane stage: raw BBM readback"
  mtd_q3n_multiplane_all_bbm_zero "$bbm_fold_offset" || return 1

  echo "q3n multi-plane stage: NAND Core markbad erase"
  flash_erase -q "$mtd_dev" "$markbad_offset" 1 || return 1
  markbad_page_seek=$((markbad_offset / writesize))
  mtd_badblock page-pattern-write raw "$mtd_dev" "$markbad_offset" \
    0x35 0xa5 || return 1
  dd if="$mtd_dev" of=/tmp/q3n-multiplane-main-before.bin bs="$writesize" \
    count=1 skip="$markbad_page_seek" 2>/dev/null || return 1
  markbad_digest_before=$(sha256sum /tmp/q3n-multiplane-main-before.bin | \
    awk '{print $1}') || return 1
  [ "$markbad_digest_before" = \
    f790d342cca81bc826050f0b6ce23ce7b4c06c7f174ce97c499653e4202fd450 ] || {
    echo "q3n multi-plane smoke: pre-mark main digest mismatch"
    return 1
  }
  mtd_badblock set "$mtd_dev" "$markbad_offset" >/dev/null || return 1
  mtd_q3n_multiplane_all_bbm_zero "$markbad_offset" || return 1
  if mtd_badblock page-pattern-read place "$mtd_dev" "$markbad_offset" \
       0x35 0xa5 >/tmp/q3n-multiplane-post-mark-normal.err 2>&1; then
    echo "q3n multi-plane smoke: normal MEMREAD unexpectedly read a marked block"
    return 1
  fi
  if mtd_badblock page-pattern-read raw "$mtd_dev" "$markbad_offset" \
       0x35 0xa5 >/tmp/q3n-multiplane-post-mark-raw.err 2>&1; then
    echo "q3n multi-plane smoke: raw MEMREAD unexpectedly read a marked block"
    return 1
  fi
  [ "$(mtd_badblock get "$mtd_dev" "$markbad_offset")" = 1 ] || return 1

  echo "q3n multi-plane stage: reload NAND Core BBT"
  modprobe -r qemu_3dnand || return 1
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  [ "$(mtd_badblock get "$mtd_dev" "$bbm_fold_offset")" = 1 ] || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$markbad_offset")" = 1 ] || return 1

  echo "q3n multi-plane guest complete logical_block=3 die=1 block_in_plane=1 page=0 main_digest=$markbad_digest_before"
}

mtd_q3n_multiplane_persist_geometry() {
  mp_persist_num=$(mtd_find_q3n)
  [ -n "$mp_persist_num" ] || return 1
  mtd_dev="/dev/mtd${mp_persist_num}"
  mp_persist_sys="/sys/class/mtd/mtd${mp_persist_num}"
  writesize=$(cat "$mp_persist_sys/writesize") || return 1
  oobsize=$(cat "$mp_persist_sys/oobsize") || return 1
  oobavail=$(cat "$mp_persist_sys/oobavail") || return 1
  erasesize=$(cat "$mp_persist_sys/erasesize") || return 1
  mtd_size=$(cat "$mp_persist_sys/size") || return 1
  mtd_q3n_multiplane_expect "$writesize" 65536 writesize || return 1
  mtd_q3n_multiplane_expect "$oobsize" 4096 oobsize || return 1
  mtd_q3n_multiplane_expect "$oobavail" 4092 oobavail || return 1
  mtd_q3n_multiplane_expect "$erasesize" 104857600 erasesize || return 1
  mtd_q3n_multiplane_expect "$mtd_size" 43620761600 size || return 1
  mtd_q3n_multiplane_expect "$((mtd_size / erasesize))" 416 blocks || return 1
}

mtd_q3n_multiplane_persist_digests() {
  mtd_badblock page-pattern-read raw "$mtd_dev" 0 0x5a 0xb1 || return 1
  dd if="$mtd_dev" of=/tmp/q3n-multiplane-persist-main.bin \
    bs="$writesize" count=1 2>/dev/null || return 1
  mtd_badblock oob-dump raw "$mtd_dev" 0 0 4096 \
    >/tmp/q3n-multiplane-persist-oob.bin || return 1
  mp_persist_main_digest=$(sha256sum /tmp/q3n-multiplane-persist-main.bin | \
    awk '{print $1}') || return 1
  mp_persist_oob_digest=$(sha256sum /tmp/q3n-multiplane-persist-oob.bin | \
    awk '{print $1}') || return 1
  [ "$mp_persist_main_digest" = \
    944044fe482bc4e91085c15c5a923a1b9e02eac98d3bce04997d6dbecd2a5b8d ] || return 1
  [ "$mp_persist_oob_digest" = \
    f0f9ce8608610d597e3416195182a2d1f47d53cf00f1e72e3824a5bc3bfa7ce8 ] || return 1
}

mtd_q3n_multiplane_persist_reject_bad_reads() {
  mp_persist_bad_offset=$1
  if mtd_badblock page-pattern-read place "$mtd_dev" "$mp_persist_bad_offset" \
       0x69 0xb9 >/tmp/q3n-multiplane-persist-normal.err 2>&1; then
    echo "q3n multi-plane persistence: normal MEMREAD unexpectedly read marked block"
    return 1
  fi
  if mtd_badblock page-pattern-read raw "$mtd_dev" "$mp_persist_bad_offset" \
       0x69 0xb9 >/tmp/q3n-multiplane-persist-raw.err 2>&1; then
    echo "q3n multi-plane persistence: raw MEMREAD unexpectedly read marked block"
    return 1
  fi
}

mtd_q3n_multiplane_persist_prepare() {
  mtd_load_q3n || return 1
  mtd_q3n_multiplane_persist_geometry || return 1
  mp_persist_bad_offset=$erasesize

  flash_erase -q "$mtd_dev" 0 1 || return 1
  mtd_badblock page-pattern-write raw "$mtd_dev" 0 0x5a 0xb1 || return 1
  mtd_q3n_multiplane_persist_digests || return 1

  # NAND Core erases block 1 before programming its BBMs; block-1 payload
  # preservation is deliberately not part of this persistence contract.
  flash_erase -q "$mtd_dev" "$mp_persist_bad_offset" 1 || return 1
  mtd_badblock page-pattern-write raw "$mtd_dev" "$mp_persist_bad_offset" \
    0x69 0xb9 || return 1
  mtd_badblock page-pattern-read raw "$mtd_dev" "$mp_persist_bad_offset" \
    0x69 0xb9 || return 1
  mtd_badblock set "$mtd_dev" "$mp_persist_bad_offset" >/dev/null || return 1
  mtd_q3n_multiplane_all_bbm_zero "$mp_persist_bad_offset" || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$mp_persist_bad_offset")" = 1 ] || return 1
  mtd_q3n_multiplane_persist_reject_bad_reads "$mp_persist_bad_offset" || return 1

  sync
  echo "q3n multi-plane persistence expected main_digest=$mp_persist_main_digest oob_digest=$mp_persist_oob_digest bbm=00000000"
  echo "q3n multi-plane persistence prepare passed"
}

mtd_q3n_multiplane_persist_verify() {
  mtd_load_q3n || return 1
  mtd_q3n_multiplane_persist_geometry || return 1
  mp_persist_bad_offset=$erasesize
  mtd_q3n_multiplane_persist_digests || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$mp_persist_bad_offset")" = 1 ] || return 1
  mtd_q3n_multiplane_all_bbm_zero "$mp_persist_bad_offset" || return 1
  mtd_q3n_multiplane_persist_reject_bad_reads "$mp_persist_bad_offset" || return 1

  modprobe -r qemu_3dnand || return 1
  mtd_load_q3n || return 1
  mtd_q3n_multiplane_persist_geometry || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$mp_persist_bad_offset")" = 1 ] || return 1
  mtd_q3n_multiplane_all_bbm_zero "$mp_persist_bad_offset" || return 1

  echo "q3n multi-plane persistence verified main_digest=$mp_persist_main_digest oob_digest=$mp_persist_oob_digest bbm=00000000"
  echo "q3n multi-plane persistence verify passed"
}

mtd_q3n_serial_cleanup() {
  echo 0 > "$stats/parity_pause_enable" 2>/dev/null || true
  echo 0 > "$stats/parity_continuation_pause_enable" 2>/dev/null || true
  if [ -n "${serial_writer_pid:-}" ]; then
    wait "$serial_writer_pid" 2>/dev/null || true
  fi
  if [ -n "${serial_writer2_pid:-}" ]; then
    wait "$serial_writer2_pid" 2>/dev/null || true
  fi
  serial_writer_pid= serial_writer2_pid=
}

mtd_q3n_serial_make_pattern() {
  pattern_page=$1
  dd if=/dev/zero of=/tmp/q3n-serial-page.bin bs=16384 count=1 \
    2>/dev/null || return 1
  printf 'q3n-serial-page-%04d\n' "$pattern_page" | \
    dd of=/tmp/q3n-serial-page.bin conv=notrunc 2>/dev/null || return 1
}

mtd_q3n_serial_write_page() {
  write_page=$1
  mtd_q3n_serial_make_pattern "$write_page" || return 1
  dd if=/tmp/q3n-serial-page.bin of="$mtd_dev" bs=16384 count=1 \
    seek="$write_page" 2>/dev/null || return 1
}

mtd_q3n_serial_read_page() {
  read_page=$1
  mtd_q3n_serial_make_pattern "$read_page" || return 1
  dd if="$mtd_dev" of=/tmp/q3n-serial-read.bin bs=16384 count=1 \
    skip="$read_page" 2>/dev/null || return 1
  cmp /tmp/q3n-serial-page.bin /tmp/q3n-serial-read.bin || return 1
}

mtd_q3n_serial_write_read_page() {
  mtd_q3n_serial_write_page "$1" || return 1
  mtd_q3n_serial_read_page "$1"
}

mtd_q3n_wait_eq() {
  file=$1 expected=$2 label=$3 tries=0
  while [ "$(cat "$file")" -ne "$expected" ] && [ "$tries" -lt 20 ]; do
    sleep 1
    tries=$((tries + 1))
  done
  [ "$(cat "$file")" -eq "$expected" ] || {
    echo "q3n serial smoke: $label did not reach $expected"
    return 1
  }
}

mtd_q3n_wait_gt() {
  file=$1 baseline=$2 label=$3 tries=0
  while [ "$(cat "$file")" -le "$baseline" ] && [ "$tries" -lt 20 ]; do
    sleep 1
    tries=$((tries + 1))
  done
  [ "$(cat "$file")" -gt "$baseline" ] || {
    echo "q3n serial smoke: $label did not advance past $baseline"
    return 1
  }
}

mtd_q3n_write_stripe() {
  stripe_base=$1 stripe_slot=0
  while [ "$stripe_slot" -lt 7 ]; do
    mtd_q3n_serial_write_read_page $((stripe_base + stripe_slot)) || return 1
    stripe_slot=$((stripe_slot + 1))
  done
}

mtd_q3n_serial_smoke() {
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  stats=/sys/kernel/debug/qemu_3dnand
  for counter in foreground_ops parity_reads parity_writes \
                 protected_stripes unprotected_stripes failed_stripes \
                 pending_parity reserved_parity max_pending_parity parity_paused \
                 parity_pause_block parity_pause_enable \
                 parity_continuation_pause_block \
                 parity_continuation_pause_class \
                 parity_continuation_pause_enable \
                 parity_continuation_paused p1_over_p2 \
                 inject_parity_program_fail cancel_parity_program_fail \
                 inject_invalid_program_fail reset_controller \
                 inject_parity_queue_fail \
                 faults_injected; do
    [ -e "$stats/$counter" ] || {
      echo "q3n serial smoke: missing $stats/$counter"
      return 1
    }
  done

  foreground_before=$(cat "$stats/foreground_ops") || return 1
  parity_reads_before=$(cat "$stats/parity_reads") || return 1
  parity_writes_before=$(cat "$stats/parity_writes") || return 1
  protected_before=$(cat "$stats/protected_stripes") || return 1
  unprotected_before=$(cat "$stats/unprotected_stripes") || return 1
  failed_before=$(cat "$stats/failed_stripes") || return 1
  faults_before=$(cat "$stats/faults_injected") || return 1
  erasesize=$(cat "/sys/class/mtd/mtd${mtd_num}/erasesize") || return 1
  block_pages=$((erasesize / 16384))

  trap 'mtd_q3n_serial_cleanup' 0
  trap 'exit 1' HUP INT TERM
  serial_writer_pid= serial_writer2_pid=

  # Pause a real rebuild exactly after it requeues P1. A foreground read must
  # complete while that continuation is waiting, without another P1 command.
  echo "q3n serial stage: p0 continuation"
  flash_erase -q "$mtd_dev" 0 1 || return 1
  page=0
  while [ "$page" -lt 6 ]; do
    mtd_q3n_serial_write_read_page "$page" || return 1
    page=$((page + 1))
  done
  echo 0 > "$stats/parity_continuation_pause_block" || return 1
  echo 1 > "$stats/parity_continuation_pause_class" || return 1
  echo 1 > "$stats/parity_continuation_pause_enable" || return 1
  mtd_q3n_serial_make_pattern 6 || return 1
  cp /tmp/q3n-serial-page.bin /tmp/q3n-p0-writer.bin || return 1
  dd if=/tmp/q3n-p0-writer.bin of="$mtd_dev" bs=16384 count=1 \
    seek=6 2>/tmp/q3n-p0-writer.err &
  serial_writer_pid=$!
  mtd_q3n_wait_gt "$stats/parity_continuation_paused" 0 \
    "P1 continuation pause" || return 1
  [ "$(cat "$stats/pending_parity")" -gt 0 ] || return 1
  p0_foreground_before=$(cat "$stats/foreground_ops") || return 1
  p0_parity_before=$(cat "$stats/parity_reads") || return 1
  mtd_q3n_serial_read_page 0 || return 1
  p0_foreground_after=$(cat "$stats/foreground_ops") || return 1
  p0_parity_after=$(cat "$stats/parity_reads") || return 1
  [ "$p0_foreground_after" -eq $((p0_foreground_before + 1)) ] || return 1
  [ "$p0_parity_after" -eq "$p0_parity_before" ] || return 1
  echo "p0 continuation acceptance passed"
  echo 0 > "$stats/parity_continuation_pause_enable" || return 1
  if ! wait "$serial_writer_pid"; then
    serial_writer_pid=
    cat /tmp/q3n-p0-writer.err 2>/dev/null || true
    return 1
  fi
  serial_writer_pid=
  mtd_q3n_wait_eq "$stats/pending_parity" 0 "initial pending parity" || return 1
  mtd_q3n_serial_read_page 6 || return 1
  protected_after=$(cat "$stats/protected_stripes") || return 1
  [ "$protected_after" -eq $((protected_before + 1)) ] || return 1

  # A queue/setup failure happens after D6 is physically durable. The MTD
  # write and retlen must still succeed, while health records failed/unprotected.
  echo "q3n serial stage: queue failure"
  queue_base=$((7 * block_pages))
  flash_erase -q "$mtd_dev" $((7 * erasesize)) 1 || return 1
  page=0
  while [ "$page" -lt 6 ]; do
    mtd_q3n_serial_write_read_page $((queue_base + page)) || return 1
    page=$((page + 1))
  done
  queue_failed_before=$(cat "$stats/failed_stripes") || return 1
  queue_unprotected_before=$(cat "$stats/unprotected_stripes") || return 1
  queue_protected_before=$(cat "$stats/protected_stripes") || return 1
  queue_reserved_before=$(cat "$stats/reserved_parity") || return 1
  echo 1 > "$stats/inject_parity_queue_fail" || return 1
  mtd_q3n_serial_write_read_page $((queue_base + 6)) || return 1
  [ "$(cat "$stats/pending_parity")" -eq 0 ] || return 1
  [ "$(cat "$stats/failed_stripes")" -eq \
    $((queue_failed_before + 1)) ] || return 1
  [ "$(cat "$stats/unprotected_stripes")" -eq \
    $((queue_unprotected_before + 1)) ] || return 1
  [ "$(cat "$stats/protected_stripes")" -eq "$queue_protected_before" ] || \
    return 1
  [ "$(cat "$stats/reserved_parity")" -eq "$queue_reserved_before" ] || \
    return 1
  echo "queue failure acceptance passed"

  # A one-shot physical program fault targets P for logical stripe zero.
  # D0..D6 remain readable even though that stripe never becomes protected.
  echo "q3n serial stage: one-shot program fault"
  flash_erase -q "$mtd_dev" 0 1 || return 1
  page=0
  while [ "$page" -lt 6 ]; do
    mtd_q3n_serial_write_read_page "$page" || return 1
    page=$((page + 1))
  done
  faults_before=$(cat "$stats/faults_injected") || return 1
  failed_before=$(cat "$stats/failed_stripes") || return 1
  unprotected_before=$(cat "$stats/unprotected_stripes") || return 1
  protected_before_fail=$(cat "$stats/protected_stripes") || return 1
  parity_writes_before_fail=$(cat "$stats/parity_writes") || return 1
  echo 0 > "$stats/inject_parity_program_fail" || return 1
  mtd_q3n_serial_write_read_page 6 || return 1
  mtd_q3n_wait_eq "$stats/pending_parity" 0 "failed pending parity" || return 1
  parity_writes_after_fail=$(cat "$stats/parity_writes") || return 1
  faults_after=$(cat "$stats/faults_injected") || return 1
  failed_after=$(cat "$stats/failed_stripes") || return 1
  unprotected_after=$(cat "$stats/unprotected_stripes") || return 1
  protected_failed=$(cat "$stats/protected_stripes") || return 1
  [ "$faults_after" -eq $((faults_before + 1)) ] || return 1
  [ "$failed_after" -eq $((failed_before + 1)) ] || return 1
  [ "$unprotected_after" -eq $((unprotected_before + 1)) ] || return 1
  [ "$protected_failed" -eq "$protected_before_fail" ] || return 1
  [ "$parity_writes_after_fail" -eq $((parity_writes_before_fail + 1)) ] || \
    return 1
  echo "one-shot fault acceptance passed"

  # A failed stripe-0 parity PROGRAM leaves no persistent placeholder. Logical
  # page 7 maps to stripe-1 D0 at physical page 8 and must still make progress.
  mtd_q3n_serial_write_read_page 7 || return 1
  [ "$(cat "$stats/unprotected_stripes")" -eq "$unprotected_after" ] || \
    return 1
  [ "$(cat "$stats/protected_stripes")" -eq "$protected_failed" ] || return 1
  [ "$(cat "$stats/parity_writes")" -eq "$parity_writes_after_fail" ] || \
    return 1
  echo "q3n no-frontier stripe progress passed"

  page=0
  while [ "$page" -lt 8 ]; do
    mtd_q3n_serial_read_page "$page" || return 1
    page=$((page + 1))
  done

  # The one-shot was consumed: an independent later stripe protects normally.
  echo "q3n serial stage: later protected stripe"
  flash_erase -q "$mtd_dev" "$erasesize" 1 || return 1
  mtd_q3n_write_stripe "$block_pages" || return 1
  mtd_q3n_wait_eq "$stats/pending_parity" 0 "later pending parity" || return 1
  protected_later=$(cat "$stats/protected_stripes") || return 1
  [ "$protected_later" -eq $((protected_failed + 1)) ] || return 1
  [ "$(cat "$stats/faults_injected")" -eq "$faults_after" ] || return 1
  echo "later protected stripe acceptance passed"

  # Every QEMU disarm path must clear both the visible bit and target latch.
  echo "q3n serial stage: fault disarm paths"
  disarm_block=2
  while [ "$disarm_block" -le 4 ]; do
    disarm_base=$((disarm_block * block_pages))
    flash_erase -q "$mtd_dev" $((disarm_block * erasesize)) 1 || return 1
    page=0
    while [ "$page" -lt 6 ]; do
      mtd_q3n_serial_write_read_page $((disarm_base + page)) || return 1
      page=$((page + 1))
    done
    echo $((disarm_base * 16384)) > "$stats/inject_parity_program_fail" || \
      return 1
    case "$disarm_block" in
      2)
        disarm_name=cancel
        echo 1 > "$stats/cancel_parity_program_fail" || return 1
        ;;
      3)
        disarm_name=invalid
        if echo 1 > "$stats/inject_invalid_program_fail" 2>/dev/null; then
          echo "q3n serial smoke: invalid fault arm unexpectedly succeeded"
          return 1
        fi
        ;;
      4)
        disarm_name=reset
        echo 1 > "$stats/reset_controller" || return 1
        ;;
    esac
    protected_disarm_before=$(cat "$stats/protected_stripes") || return 1
    mtd_q3n_serial_write_read_page $((disarm_base + 6)) || return 1
    mtd_q3n_wait_eq "$stats/pending_parity" 0 "disarm pending parity" || return 1
    [ "$(cat "$stats/protected_stripes")" -eq \
      $((protected_disarm_before + 1)) ] || return 1
    [ "$(cat "$stats/faults_injected")" -eq "$faults_after" ] || return 1
    echo "fault disarm acceptance passed: $disarm_name"
    disarm_block=$((disarm_block + 1))
  done

  # Real-worker P1>P2: pause block 5 after its seventh P1 has queued P2,
  # then hold block 6 at entry with P1 queued. The actual P2 claim must yield.
  echo "q3n serial stage: p1 over p2"
  block5_base=$((5 * block_pages))
  block6_base=$((6 * block_pages))
  flash_erase -q "$mtd_dev" $((5 * erasesize)) 2 || return 1
  # Program D0..D5 of block 6 before pausing block 5: closing any writable
  # MTD fd performs a global sync, so these foreground writes must come first.
  stripe_slot=0
  while [ "$stripe_slot" -lt 6 ]; do
    mtd_q3n_serial_write_read_page $((block6_base + stripe_slot)) || return 1
    stripe_slot=$((stripe_slot + 1))
  done
  echo 5 > "$stats/parity_continuation_pause_block" || return 1
  echo 2 > "$stats/parity_continuation_pause_class" || return 1
  echo 1 > "$stats/parity_continuation_pause_enable" || return 1
  stripe_slot=0
  while [ "$stripe_slot" -lt 6 ]; do
    mtd_q3n_serial_write_read_page $((block5_base + stripe_slot)) || return 1
    stripe_slot=$((stripe_slot + 1))
  done
  mtd_q3n_serial_make_pattern $((block5_base + 6)) || return 1
  cp /tmp/q3n-serial-page.bin /tmp/q3n-p2-writer.bin || return 1
  dd if=/tmp/q3n-p2-writer.bin of="$mtd_dev" bs=16384 count=1 \
    seek=$((block5_base + 6)) 2>/tmp/q3n-p2-writer.err &
  serial_writer_pid=$!
  mtd_q3n_wait_gt "$stats/parity_continuation_paused" 0 \
    "P2 continuation pause" || return 1
  echo 6 > "$stats/parity_pause_block" || return 1
  echo 1 > "$stats/parity_pause_enable" || return 1
  mtd_q3n_serial_make_pattern $((block6_base + 6)) || return 1
  cp /tmp/q3n-serial-page.bin /tmp/q3n-p1-writer.bin || return 1
  dd if=/tmp/q3n-p1-writer.bin of="$mtd_dev" bs=16384 count=1 \
    seek=$((block6_base + 6)) 2>/tmp/q3n-p1-writer.err &
  serial_writer2_pid=$!
  mtd_q3n_wait_gt "$stats/parity_paused" 0 "P1 entry pause" || return 1
  p1_over_p2_before=$(cat "$stats/p1_over_p2") || return 1
  echo 0 > "$stats/parity_continuation_pause_enable" || return 1
  mtd_q3n_wait_gt "$stats/p1_over_p2" "$p1_over_p2_before" \
    "P1 over P2 arbitration" || return 1
  p1_over_p2_after=$(cat "$stats/p1_over_p2") || return 1
  [ "$p1_over_p2_after" -eq $((p1_over_p2_before + 1)) ] || {
    echo "P2 retried without a scheduler state change: before=$p1_over_p2_before after=$p1_over_p2_after" >&2
    return 1
  }
  echo "p1 over p2 acceptance passed: event wait"
  echo 0 > "$stats/parity_pause_enable" || return 1
  if ! wait "$serial_writer2_pid"; then
    serial_writer2_pid=
    cat /tmp/q3n-p1-writer.err 2>/dev/null || true
    return 1
  fi
  serial_writer2_pid=
  if ! wait "$serial_writer_pid"; then
    serial_writer_pid=
    cat /tmp/q3n-p2-writer.err 2>/dev/null || true
    return 1
  fi
  serial_writer_pid=
  mtd_q3n_wait_eq "$stats/pending_parity" 0 "arbitration pending parity" || return 1

  foreground_after=$(cat "$stats/foreground_ops") || return 1
  parity_reads_after=$(cat "$stats/parity_reads") || return 1
  parity_writes_after=$(cat "$stats/parity_writes") || return 1
  max_pending=$(cat "$stats/max_pending_parity") || return 1
  [ "$foreground_after" -gt "$foreground_before" ] || return 1
  [ "$parity_reads_after" -gt "$parity_reads_before" ] || return 1
  [ "$parity_writes_after" -gt "$parity_writes_before" ] || return 1
  [ "$max_pending" -gt 0 ] || return 1
  [ "$(cat "$stats/reserved_parity")" -eq 0 ] || return 1

  trap - 0 HUP INT TERM
  mtd_q3n_parity_stats || return 1
  mtd_q3n_oob_bbm_test || return 1
  echo "q3n serial smoke passed: protected=$protected_later unprotected=$unprotected_after failed=$failed_after faults=$faults_after max_pending=$max_pending"
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

mtd_q3n_oob_bbm_test() {
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  mtd_sysfs="/sys/class/mtd/mtd${mtd_num}"
  writesize=$(cat "$mtd_sysfs/writesize") || return 1
  oobsize=$(cat "$mtd_sysfs/oobsize") || return 1
  erasesize=$(cat "$mtd_sysfs/erasesize") || return 1
  stats=/sys/kernel/debug/qemu_3dnand

  [ "$writesize" -eq 16384 ] || return 1
  [ "$oobsize" -eq 128 ] || {
    echo "q3n OOB/BBM test: exposed OOB is $oobsize, expected 128"
    return 1
  }

  # Page-scoped commands must reject an OOB offset at the boundary and any
  # length that would wrap into the next page.
  bounds_offset=$erasesize
  flash_erase -q "$mtd_dev" "$bounds_offset" 1 || return 1
  if mtd_badblock oob-read place "$mtd_dev" "$bounds_offset" 128 1 \
       >/tmp/q3n-oob-offset.out 2>/tmp/q3n-oob-offset.err; then
    echo "q3n OOB/BBM test: OOB offset 128 unexpectedly accepted"
    return 1
  fi
  if mtd_badblock oob-read place "$mtd_dev" "$bounds_offset" 0 129 \
       >/tmp/q3n-oob-length.out 2>/tmp/q3n-oob-length.err; then
    echo "q3n OOB/BBM test: page OOB length 129 unexpectedly accepted"
    return 1
  fi
  echo "OOB bounds rejection passed"

  # Bypass the helper's logical-OOB range check and let MEMREADOOB64 reach
  # mtdchar.  The kernel must independently reject ooboffs == oobsize.
  if mtd_badblock oob-read-unchecked place "$mtd_dev" \
       "$bounds_offset" 128 1 >/tmp/q3n-oob-kernel.out \
       2>/tmp/q3n-oob-kernel.err; then
    echo "q3n OOB/BBM test: kernel accepted OOB offset 128"
    return 1
  fi
  grep -q 'ioctl' /tmp/q3n-oob-kernel.err || return 1
  echo "kernel OOB bounds rejection passed"

  # The explicit span command is the unambiguous cross-page interface.
  mtd_badblock span-write place "$mtd_dev" "$bounds_offset" 2 100 0x5a || \
    return 1
  mtd_badblock span-read place "$mtd_dev" "$bounds_offset" 2 100 0x5a || \
    return 1
  echo "cross-page OOB passed"

  # A combined request programs main and logical OOB through separate physical
  # operations while preserving both regions.
  place_offset=$((2 * erasesize))
  flash_erase -q "$mtd_dev" "$place_offset" 1 || return 1
  mtd_badblock page-write place "$mtd_dev" "$place_offset" \
    0x5a 100 1 0xa5 || return 1
  mtd_badblock page-read place "$mtd_dev" "$place_offset" \
    0x5a 100 1 0xa5 || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$place_offset")" = "0" ] || return 1
  echo "PLACE main+OOB passed"

  raw_offset=$((3 * erasesize))
  flash_erase -q "$mtd_dev" "$raw_offset" 1 || return 1
  mtd_badblock page-write raw "$mtd_dev" "$raw_offset" \
    0x3c 100 1 0xc3 || return 1
  mtd_badblock page-read raw "$mtd_dev" "$raw_offset" \
    0x3c 100 1 0xc3 || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$raw_offset")" = "0" ] || return 1
  echo "RAW main+OOB passed"

  # Keep the direct writable-BBM acceptance on a separate block so the
  # good-page rewrite case above is not masked by bad-block rejection.
  direct_bbm_offset=$((7 * erasesize))
  flash_erase -q "$mtd_dev" "$direct_bbm_offset" 1 || return 1
  [ "$(mtd_badblock oob-read place "$mtd_dev" \
       "$direct_bbm_offset" 0 1)" = "ff" ] || {
    echo "q3n OOB/BBM test: erased page BBM is not ff"
    return 1
  }

  mtd_badblock oob-write place "$mtd_dev" \
    "$direct_bbm_offset" 0 1 0x00 || return 1
  [ "$(mtd_badblock oob-read place "$mtd_dev" \
       "$direct_bbm_offset" 0 1)" = "00" ] || {
    echo "q3n OOB/BBM test: programmed BBM did not read back as 00"
    return 1
  }
  [ "$(mtd_badblock get "$mtd_dev" "$direct_bbm_offset")" = "1" ] || {
    echo "q3n OOB/BBM test: block status did not observe BBM"
    return 1
  }

  # MEMSETBADBLOCK must use the same OOB-only path without changing an
  # already-programmed first-page main area.
  markbad_offset=$((8 * erasesize))
  markbad_page_seek=$((markbad_offset / writesize))
  markbad_expected_digest=00ae035cc27f2bf984c1fee26bf8cdeecd7245b4c9e115384ffd429bf79c1b1b
  flash_erase -q "$mtd_dev" "$markbad_offset" 1 || return 1
  mtd_badblock page-write raw "$mtd_dev" "$markbad_offset" \
    0x69 100 1 0xa5 || return 1
  mtd_badblock page-read raw "$mtd_dev" "$markbad_offset" \
    0x69 100 1 0xa5 || return 1
  dd if="$mtd_dev" of=/tmp/q3n-markbad-main-before.bin bs="$writesize" \
    count=1 skip="$markbad_page_seek" 2>/dev/null || return 1
  markbad_main_digest_before=$(
    sha256sum /tmp/q3n-markbad-main-before.bin | awk '{print $1}'
  ) || return 1
  [ "$markbad_main_digest_before" = "$markbad_expected_digest" ] || {
    echo "q3n OOB/BBM test: pre-mark main digest mismatch"
    return 1
  }
  mtd_badblock set "$mtd_dev" "$markbad_offset" >/dev/null || return 1
  [ "$(mtd_badblock oob-read raw "$mtd_dev" "$markbad_offset" 0 1)" = "00" ] || {
    echo "q3n OOB/BBM test: MEMSETBADBLOCK did not program BBM 00"
    return 1
  }
  mtd_badblock page-read raw "$mtd_dev" "$markbad_offset" \
    0x69 100 1 0xa5 || return 1
  dd if="$mtd_dev" of=/tmp/q3n-markbad-main-after.bin bs="$writesize" \
    count=1 skip="$markbad_page_seek" 2>/dev/null || return 1
  markbad_main_digest_after=$(
    sha256sum /tmp/q3n-markbad-main-after.bin | awk '{print $1}'
  ) || return 1
  [ "$markbad_main_digest_after" = "$markbad_expected_digest" ] || {
    echo "q3n OOB/BBM test: post-mark main digest mismatch"
    return 1
  }
  cmp /tmp/q3n-markbad-main-before.bin /tmp/q3n-markbad-main-after.bin || {
    echo "q3n OOB/BBM test: markbad changed first-page main data"
    return 1
  }
  [ "$(mtd_badblock get "$mtd_dev" "$markbad_offset")" = "1" ] || {
    echo "q3n OOB/BBM test: MEMGETBADBLOCK did not observe BBM"
    return 1
  }
  mtd_badblock set "$mtd_dev" "$markbad_offset" >/dev/null || return 1
  echo "q3n OOB-only markbad main preservation passed: digest=$markbad_main_digest_after"
  echo "q3n OOB/BBM test passed: logical_oob=128 bbm=00"
}

mtd_q3n_persist_prepare() {
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  erasesize=$(cat "/sys/class/mtd/mtd${mtd_num}/erasesize") || return 1
  writesize=$(cat "/sys/class/mtd/mtd${mtd_num}/writesize") || return 1
  persist_offset=$erasesize
  persist_page_seek=$((persist_offset / writesize))

  echo "q3n persistence stage: erase block 0"
  flash_erase -q "$mtd_dev" 0 1 || return 1
  echo "q3n persistence stage: program block 0 page 0"
  mtd_badblock page-write raw "$mtd_dev" 0 0x5a 100 1 0xa5 || return 1
  echo "q3n persistence stage: read block 0 OOB"
  mtd_badblock oob-read raw "$mtd_dev" 0 100 1 || return 1

  echo "q3n persistence stage: erase block 1"
  flash_erase -q "$mtd_dev" "$persist_offset" 1 || return 1
  echo "q3n persistence stage: program block 1 page 0"
  mtd_badblock page-write raw "$mtd_dev" "$persist_offset" \
    0x69 100 1 0xa5 || return 1
  echo "q3n persistence stage: read block 1 page 0"
  dd if="$mtd_dev" of=/tmp/q3n-persist-main-before.bin bs="$writesize" \
    count=1 skip="$persist_page_seek" 2>/dev/null || return 1
  echo "q3n persistence stage: mark block 1 bad"
  mtd_badblock set "$mtd_dev" "$persist_offset" >/dev/null || return 1
  expected_bbm=$(mtd_badblock oob-read raw "$mtd_dev" \
    "$persist_offset" 0 1) || return 1
  echo "q3n persistence stage: BBM read returned $expected_bbm"
  [ "$expected_bbm" = "00" ] || return 1
  echo "q3n persistence stage: raw-read marked page"
  mtd_badblock page-read raw "$mtd_dev" "$persist_offset" \
    0xff 0 1 0x00 || return 1
  echo "q3n persistence stage: regular-read marked page"
  dd if="$mtd_dev" of=/tmp/q3n-persist-main-after.bin bs="$writesize" \
    count=1 skip="$persist_page_seek" 2>/dev/null || return 1
  expected_main_digest=$(sha256sum /tmp/q3n-persist-main-after.bin |
    awk '{print $1}') || return 1
  [ "$(mtd_badblock get "$mtd_dev" "$persist_offset")" = "1" ] || return 1
  sync
  echo "q3n persistence expected bbm=$expected_bbm main_digest=$expected_main_digest"
  echo "q3n persistence prepare passed"
}

mtd_q3n_persist_verify() {
  mtd_load_q3n || return 1
  mtd_num=$(mtd_find_q3n)
  [ -n "$mtd_num" ] || return 1
  mtd_dev="/dev/mtd${mtd_num}"
  erasesize=$(cat "/sys/class/mtd/mtd${mtd_num}/erasesize") || return 1
  writesize=$(cat "/sys/class/mtd/mtd${mtd_num}/writesize") || return 1
  bad_page_seek=$((erasesize / writesize))
  persist_offset=$erasesize

  mtd_badblock page-read raw "$mtd_dev" 0 0x5a 100 1 0xa5 || {
    echo "q3n persistence verify: baseline main/OOB read failed"
    return 1
  }

  verified_bbm=$(mtd_badblock oob-read raw "$mtd_dev" \
    "$persist_offset" 0 1) || {
    echo "q3n persistence verify: persisted BBM read failed"
    return 1
  }
  [ "$verified_bbm" = "00" ] || {
    echo "q3n persistence verify: BBM=$verified_bbm, expected 00"
    return 1
  }
  [ "$(mtd_badblock get "$mtd_dev" "$persist_offset")" = "1" ] || {
    echo "q3n persistence verify: block-isbad did not observe persisted BBM"
    return 1
  }
  mtd_badblock page-read raw "$mtd_dev" "$persist_offset" \
    0xff 0 1 0x00 || {
    echo "q3n persistence verify: bad-page raw main/OOB read failed"
    return 1
  }
  dd if="$mtd_dev" of=/tmp/q3n-persist-main-verified.bin bs="$writesize" \
    count=1 skip="$bad_page_seek" 2>/dev/null || {
    echo "q3n persistence verify: bad-page main read failed"
    return 1
  }
  verified_main_digest=$(sha256sum /tmp/q3n-persist-main-verified.bin |
    awk '{print $1}') || {
    echo "q3n persistence verify: main digest failed"
    return 1
  }
  if dd if=/dev/zero of="$mtd_dev" bs="$writesize" count=1 \
       seek="$bad_page_seek" 2>/tmp/q3n-persist-bad-write.err; then
    return 1
  fi
  flash_erase -q -N "$mtd_dev" "$erasesize" 1 \
    >/tmp/q3n-persist-bad-erase.err 2>&1 || true
  grep -q 'MTD Erase failure' /tmp/q3n-persist-bad-erase.err || return 1

  echo "q3n persistence verified bbm=$verified_bbm main_digest=$verified_main_digest"
  echo "q3n persistence verify passed"
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
  q3n-parity-stats) mtd_q3n_parity_stats ;;
  q3n-inject-loss) mtd_q3n_inject_loss "${2:-}" ;;
  q3n-serial-smoke) mtd_q3n_serial_smoke ;;
  q3n-generation-smoke) mtd_q3n_generation_smoke ;;
  q3n-cancel-barrier-smoke) mtd_q3n_cancel_barrier_smoke ;;
  q3n-markbad-smoke) mtd_q3n_markbad_smoke ;;
  q3n-oob-bbm-test) mtd_q3n_oob_bbm_test ;;
  q3n-persist-prepare) mtd_q3n_persist_prepare ;;
  q3n-persist-verify) mtd_q3n_persist_verify ;;
  nandsim) mtd_load_simulators; cat /proc/mtd ;;
  ubifs) mtd_ubifs ;;
  clean) mtd_clean ;;
esac
