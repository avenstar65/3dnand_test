#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd gdb
mkdirs

linux_dir=$(selected_linux_dir)
version=$(kernel_version_from_dir "$linux_dir")
out_dir="$build_dir/linux-$version"
vmlinux="$out_dir/vmlinux"

[ -f "$vmlinux" ] || die "缺少 vmlinux，请先运行 ./scripts/build-kernel.sh"

gdbinit="$work_dir/gdb-kernel.gdb"
cat > "$gdbinit" <<EOF
set pagination off
file $vmlinux
target remote localhost:1234
lx-symbols
EOF

info "连接 QEMU GDB stub: localhost:1234"
exec gdb -x "$gdbinit"

