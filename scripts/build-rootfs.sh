#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd cpio
need_cmd find
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
out_dir="$build_dir/linux-$version"
stage="$rootfs_build_dir/stage"
initramfs="$rootfs_build_dir/initramfs.cpio.gz"
modules_src="$out_dir/modules/lib/modules"

[ -d "$out_dir" ] || die "缺少内核构建目录: $out_dir"

rm -rf "$stage"
mkdir -p \
  "$stage/bin" \
  "$stage/sbin" \
  "$stage/etc/profile.d" \
  "$stage/proc" \
  "$stage/sys" \
  "$stage/dev" \
  "$stage/tmp" \
  "$stage/run" \
  "$stage/mnt" \
  "$stage/lib/modules"

if command -v busybox >/dev/null 2>&1; then
  busybox_path=$(command -v busybox)
  cp "$busybox_path" "$stage/bin/busybox"
  for applet in sh mount modprobe cat echo grep ls mkdir dmesg insmod rmmod; do
    ln -sf busybox "$stage/bin/$applet"
  done
else
  die "容器内缺少 busybox，请在 Dockerfile 中安装或提供 busybox"
fi

cp "$repo_root/rootfs/init" "$stage/init"
cp "$repo_root/rootfs/profile.d/mtd.sh" "$stage/etc/profile.d/mtd.sh"
chmod +x "$stage/init" "$stage/etc/profile.d/mtd.sh"

if [ -d "$modules_src" ]; then
  mkdir -p "$stage/lib/modules"
  cp -R "$modules_src"/. "$stage/lib/modules/"
fi

info "生成 initramfs: $initramfs"
(cd "$stage" && find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$initramfs")

info "完成: $initramfs"
