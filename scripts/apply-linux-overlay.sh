#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

linux_dir=$(selected_linux_dir)
overlay_dir="$repo_root/linux/drivers/mtd/nand/raw"
raw_dir="$linux_dir/drivers/mtd/nand/raw"

[ -d "$raw_dir" ] || die "Linux 源码缺少 raw NAND 目录: $raw_dir"

info "应用 qemu_3dnand Linux overlay 到: $linux_dir"

rm -f "$raw_dir/qemu_3dnand.c"
cp "$overlay_dir/qemu_3dnand_main.c" "$raw_dir/qemu_3dnand_main.c"
cp "$overlay_dir/qemu_3dnand.h" "$raw_dir/qemu_3dnand.h"
cp "$overlay_dir/qemu_3dnand_priv.h" "$raw_dir/qemu_3dnand_priv.h"
cp "$overlay_dir/qemu_3dnand_map.c" "$raw_dir/qemu_3dnand_map.c"
cp "$overlay_dir/qemu_3dnand_kunit.c" "$raw_dir/qemu_3dnand_kunit.c"
cp "$overlay_dir/Kconfig.qemu_3dnand" "$raw_dir/Kconfig.qemu_3dnand"
cp "$overlay_dir/Makefile.qemu_3dnand" "$raw_dir/Makefile.qemu_3dnand"

kconfig_line='source "drivers/mtd/nand/raw/Kconfig.qemu_3dnand"'
if ! grep -Fq "$kconfig_line" "$raw_dir/Kconfig"; then
  printf '\n%s\n' "$kconfig_line" >> "$raw_dir/Kconfig"
fi

makefile_line='include $(src)/Makefile.qemu_3dnand'
if ! grep -Fq "$makefile_line" "$raw_dir/Makefile"; then
  printf '\n%s\n' "$makefile_line" >> "$raw_dir/Makefile"
fi

info "完成 qemu_3dnand Linux overlay"
