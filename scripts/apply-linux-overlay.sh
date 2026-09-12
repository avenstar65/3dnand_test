#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

linux_dir=$(selected_linux_dir)
overlay_dir="$repo_root/linux/drivers/mtd/nand/raw"
raw_dir="$linux_dir/drivers/mtd/nand/raw"

[ -d "$raw_dir" ] || die "Linux 源码缺少 raw NAND 目录: $raw_dir"

"$repo_root/scripts/apply-linux-core-patches.sh" "$linux_dir"

info "应用 qemu_3dnand Linux overlay 到: $linux_dir"

copy_overlay() {
  source_file=$1
  target_file=$2
  temporary_file="$target_file.q3n-new"

  cp "$source_file" "$temporary_file"
  sync -f "$temporary_file" 2>/dev/null || sync
  cmp -s "$source_file" "$temporary_file" ||
    die "overlay 复制校验失败: $source_file"
  mv -f "$temporary_file" "$target_file"
  sync -f "$target_file" 2>/dev/null || sync
  cmp -s "$source_file" "$target_file" ||
    die "overlay 原子替换校验失败: $source_file"
}

rm -f "$raw_dir/qemu_3dnand.c"
for file in qemu_3dnand_main.c qemu_3dnand.h qemu_3dnand_priv.h \
            qemu_3dnand_map.c qemu_3dnand_raid.c qemu_3dnand_sched.c \
            qemu_3dnand_kunit.c qemu_3dnand_mp.c Kconfig.qemu_3dnand \
            Makefile.qemu_3dnand; do
  copy_overlay "$overlay_dir/$file" "$raw_dir/$file"
done

kconfig_line='source "drivers/mtd/nand/raw/Kconfig.qemu_3dnand"'
if ! grep -Fq "$kconfig_line" "$raw_dir/Kconfig"; then
  printf '\n%s\n' "$kconfig_line" >> "$raw_dir/Kconfig"
fi

makefile_line='include $(src)/Makefile.qemu_3dnand'
if ! grep -Fq "$makefile_line" "$raw_dir/Makefile"; then
  printf '\n%s\n' "$makefile_line" >> "$raw_dir/Makefile"
fi

info "完成 qemu_3dnand Linux overlay"
