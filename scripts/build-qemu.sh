#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

target_list=${QEMU_TARGET_LIST:-x86_64-softmmu}
jobs=${JOBS:-}
python_bin=${PYTHON:-}

usage() {
  cat <<'USAGE'
用法: scripts/build-qemu.sh [--target-list LIST]

默认构建 x86_64-softmmu，并在 configure 前应用 q3n-nand overlay。
可通过 QEMU_DIR 指定源码目录，通过 JOBS 指定并行度。
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --target-list)
      shift
      [ "$#" -gt 0 ] || die "--target-list 需要参数"
      target_list=$1
      ;;
    -h|--help) usage; exit 0 ;;
    *) die "未知参数: $1" ;;
  esac
  shift
done

need_cmd python3
need_cmd pkg-config
need_cmd ninja
mkdirs

if [ -z "$python_bin" ] && [ -x /opt/homebrew/bin/python3 ]; then
  python_bin=/opt/homebrew/bin/python3
fi
if [ -z "$python_bin" ]; then
  python_bin=$(command -v python3)
fi

qemu_dir=$(selected_qemu_dir)
version=$(qemu_version_from_dir "$qemu_dir")
out_dir="$build_dir/qemu-$version"

"$repo_root/scripts/apply-qemu-overlay.sh"

if [ ! -f "$out_dir/build.ninja" ]; then
  info "配置 QEMU $version: $out_dir"
  mkdir -p "$out_dir"
  (
    cd "$out_dir"
    "$qemu_dir/configure" \
      --python="$python_bin" \
      --target-list="$target_list" \
      --disable-werror \
      --disable-docs \
      --disable-gtk \
      --disable-sdl \
      --disable-vnc \
      --disable-curses \
      --disable-spice \
      --disable-opengl \
      --disable-virglrenderer \
      --disable-tools \
      --disable-guest-agent \
      --disable-strip \
      --prefix="$out_dir/install" \
      --cross-prefix= \
      --extra-cflags="-Wno-error"
  )
fi

info "编译 QEMU $version"
if [ -n "$jobs" ]; then
  ninja -C "$out_dir" -j "$jobs"
else
  ninja -C "$out_dir"
fi

qemu_bin="$out_dir/qemu-system-x86_64"
unsigned_bin="$out_dir/qemu-system-x86_64-unsigned"
if [ -x "$unsigned_bin" ] &&
   { [ ! -x "$qemu_bin" ] || [ "$unsigned_bin" -nt "$qemu_bin" ]; }; then
  cp "$unsigned_bin" "$qemu_bin"
  chmod +x "$qemu_bin"
fi

[ -x "$qemu_bin" ] || die "未生成 $qemu_bin"

info "完成: $qemu_bin"
