#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

usage() {
  printf '%s\n' "用法: $0 --image PATH --logical-block N --die N --block-in-plane N --page N --expected-erased-digest SHA256" >&2
  exit 2
}

image=
logical_block=
die=
block_in_plane=
page=
expected_erased_digest=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --image) shift; [ "$#" -gt 0 ] || usage; image=$1 ;;
    --logical-block) shift; [ "$#" -gt 0 ] || usage; logical_block=$1 ;;
    --die) shift; [ "$#" -gt 0 ] || usage; die=$1 ;;
    --block-in-plane) shift; [ "$#" -gt 0 ] || usage; block_in_plane=$1 ;;
    --page) shift; [ "$#" -gt 0 ] || usage; page=$1 ;;
    --expected-erased-digest)
      shift; [ "$#" -gt 0 ] || usage; expected_erased_digest=$1
      ;;
    *) usage ;;
  esac
  shift
done

[ -f "$image" ] || die "缺少 multi-plane NAND 镜像: $image"
case "$logical_block" in ''|*[!0-9]*) usage ;; esac
case "$die" in ''|*[!0-9]*) usage ;; esac
case "$block_in_plane" in ''|*[!0-9]*) usage ;; esac
case "$page" in ''|*[!0-9]*) usage ;; esac
case "$expected_erased_digest" in *[!0123456789abcdef]*|'') usage ;; esac
[ "${#expected_erased_digest}" -eq 64 ] || usage

image_dir=$(CDPATH= cd -- "$(dirname -- "$image")" && pwd -P)
image="$image_dir/$(basename -- "$image")"
case "$image" in
  "$repo_root"/*) image_container=/workspace/${image#"$repo_root"/} ;;
  *) die "NAND 镜像不在工作区内，无法映射到验证容器: $image" ;;
esac

qemu_dir=$(selected_qemu_dir)
qemu_version=$(qemu_version_from_dir "$qemu_dir")
qemu_bin="$build_dir/qemu-$qemu_version/qemu-system-x86_64-unsigned"
[ -x "$qemu_bin" ] || qemu_bin="$build_dir/qemu-$qemu_version/qemu-system-x86_64"
[ -x "$qemu_bin" ] || die "缺少 QEMU 验证二进制: $qemu_bin"
qemu_dir=$(CDPATH= cd -- "$(dirname -- "$qemu_bin")" && pwd -P)
qemu_bin="$qemu_dir/$(basename -- "$qemu_bin")"
case "$qemu_bin" in
  "$repo_root"/*) qemu_container=/workspace/${qemu_bin#"$repo_root"/} ;;
  *) die "QEMU 验证二进制不在工作区内: $qemu_bin" ;;
esac

exec "$repo_root/scripts/shell.sh" python3 \
  /workspace/tests/q3n_multiplane_media_verify.py \
  --qemu "$qemu_container" --image "$image_container" \
  --logical-block "$logical_block" --die "$die" \
  --block-in-plane "$block_in_plane" --page "$page" \
  --expected-erased-digest "$expected_erased_digest"
