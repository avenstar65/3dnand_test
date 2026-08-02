#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

qemu_dir=$(selected_qemu_dir)
overlay_dir="$repo_root/qemu"

[ -d "$qemu_dir/hw/block" ] || die "QEMU 源码缺少 hw/block: $qemu_dir"
[ -d "$qemu_dir/include/hw/mtd" ] || mkdir -p "$qemu_dir/include/hw/mtd"

info "应用 q3n-nand overlay 到: $qemu_dir"

cp "$overlay_dir/hw/mtd/q3n-nand.c" "$qemu_dir/hw/block/q3n-nand.c"
cp "$overlay_dir/hw/mtd/q3n-pci.c" "$qemu_dir/hw/block/q3n-pci.c"
cp "$overlay_dir/hw/mtd/q3n-media.c" "$qemu_dir/hw/block/q3n-media.c"
cp "$overlay_dir/hw/mtd/q3n-multiplane.c" \
   "$qemu_dir/hw/block/q3n-multiplane.c"
cp "$overlay_dir/hw/mtd/q3n-multiplane.h" \
   "$qemu_dir/hw/block/q3n-multiplane.h"
cp "$overlay_dir/hw/mtd/q3n-media-overlay.h" \
   "$qemu_dir/hw/block/q3n-media-overlay.h"
cp "$overlay_dir/include/hw/mtd/q3n-nand.h" "$qemu_dir/include/hw/mtd/q3n-nand.h"
cp "$overlay_dir/include/hw/mtd/q3n-media.h" "$qemu_dir/include/hw/mtd/q3n-media.h"

meson_file="$qemu_dir/hw/block/meson.build"
meson_line="system_ss.add(when: 'CONFIG_Q3N_NAND', if_true: files('q3n-media.c', 'q3n-multiplane.c', 'q3n-nand.c', 'q3n-pci.c'))"
if ! grep -Fq "$meson_line" "$meson_file"; then
  sed -i.bak "/CONFIG_Q3N_NAND.*q3n-nand.c/d" "$meson_file"
  rm -f "$meson_file.bak"
  printf '\n%s\n' "$meson_line" >> "$meson_file"
fi

kconfig_file="$qemu_dir/hw/block/Kconfig"
if [ -f "$kconfig_file" ]; then
  if ! grep -Eq '^config Q3N_NAND$' "$kconfig_file"; then
    cat >> "$kconfig_file" <<'EOF'

config Q3N_NAND
    bool
    depends on PCI
    default y
EOF
  fi
fi

info "完成 q3n-nand overlay"
