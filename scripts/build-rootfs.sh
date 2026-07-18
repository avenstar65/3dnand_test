#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd cpio
need_cmd file
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
  "$stage/mnt/ubifs" \
  "$stage/lib64" \
  "$stage/usr/sbin" \
  "$stage/lib/modules"

busybox_path=${ROOTFS_BUSYBOX:-}
if [ -z "$busybox_path" ] && [ -x /opt/rootfs-amd64/usr/bin/busybox ]; then
  busybox_path=/opt/rootfs-amd64/usr/bin/busybox
fi
if [ -z "$busybox_path" ] && command -v busybox >/dev/null 2>&1; then
  busybox_path=$(command -v busybox)
fi

[ -n "$busybox_path" ] && [ -x "$busybox_path" ] || die "容器内缺少 busybox，请在 Dockerfile 中安装或设置 ROOTFS_BUSYBOX"
file "$busybox_path" | grep -Eq 'x86-64|x86_64' || die "busybox 不是 x86_64 ELF: $busybox_path"

cp "$busybox_path" "$stage/bin/busybox"
for applet in sh mount umount modprobe cat echo grep ls mkdir dmesg insmod rmmod sleep true false poweroff; do
  ln -sf busybox "$stage/bin/$applet"
done

if [ -d /opt/rootfs-amd64/lib ]; then
  cp -R /opt/rootfs-amd64/lib/. "$stage/lib/"
fi
if [ -d /opt/rootfs-amd64/lib64 ]; then
  cp -R /opt/rootfs-amd64/lib64/. "$stage/lib64/"
fi
if [ -d /opt/rootfs-amd64/usr/lib ]; then
  mkdir -p "$stage/usr/lib"
  cp -R /opt/rootfs-amd64/usr/lib/. "$stage/usr/lib/"
fi
if [ -e "$stage/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" ]; then
  ln -sf /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$stage/lib64/ld-linux-x86-64.so.2"
fi

for tool in flash_erase ubiformat ubiattach ubidetach ubimkvol ubinfo; do
  tool_path="/opt/rootfs-amd64/usr/sbin/$tool"
  [ -x "$tool_path" ] || die "缺少 amd64 mtd-utils 工具: $tool_path"
  cp "$tool_path" "$stage/usr/sbin/$tool"
done

rootfs_cc=${ROOTFS_CC:-x86_64-linux-gnu-gcc}
need_cmd "$rootfs_cc"
"$rootfs_cc" -O2 -Wall -Wextra -o "$stage/usr/sbin/mtd_badblock" \
  "$repo_root/rootfs/helpers/mtd_badblock.c"
file "$stage/usr/sbin/mtd_badblock" | grep -Eq 'x86-64|x86_64' || \
  die "mtd_badblock 不是 x86_64 ELF"

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
