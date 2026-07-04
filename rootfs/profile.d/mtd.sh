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
  nandsim) mtd_load_simulators; cat /proc/mtd ;;
  ubifs) mtd_ubifs ;;
  clean) mtd_clean ;;
esac
