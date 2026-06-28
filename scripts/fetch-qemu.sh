#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

version=${QEMU_VERSION:-11.0.2}
force=0
redownload=0

usage() {
  cat <<'USAGE'
用法: scripts/fetch-qemu.sh [--version X.Y.Z] [--force] [--redownload]

默认下载 QEMU 11.0.2 源码到 work/qemu/qemu-11.0.2。
可通过 QEMU_VERSION 环境变量或 --version 覆盖版本。
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --version)
      shift
      [ "$#" -gt 0 ] || die "--version 需要参数"
      version=$1
      ;;
    --force) force=1 ;;
    --redownload) redownload=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "未知参数: $1" ;;
  esac
  shift
done

need_cmd curl
need_cmd rsync
need_cmd tar
mkdirs

tar_name="qemu-$version.tar.xz"
url="${QEMU_BASE_URL:-https://download.qemu.org}/$tar_name"
tar_path="$downloads_dir/$tar_name"
dest="$qemu_work_dir/qemu-$version"

if [ -d "$dest" ] && [ "$force" -ne 1 ]; then
  info "复用已有 QEMU 源码: $dest"
  exit 0
fi

if [ "$force" -eq 1 ]; then
  rm -rf "$dest"
fi

if [ -f "$tar_path" ] && [ "$redownload" -ne 1 ] && tar -tf "$tar_path" >/dev/null 2>&1; then
  info "复用已有下载包: $tar_path"
else
  tmp_tar="$tar_path.part"
  if [ -f "$tar_path" ] && [ ! -f "$tmp_tar" ]; then
    mv "$tar_path" "$tmp_tar"
  fi
  info "下载 QEMU $version: $url"
  curl --continue-at - -fL "$url" -o "$tmp_tar"
  mv "$tmp_tar" "$tar_path"
fi

info "解压到 $dest"
mkdir -p "$qemu_work_dir"
extract_tmp=${TMPDIR:-/tmp}/qemu-extract-$$
rm -rf "$extract_tmp"
mkdir -p "$extract_tmp"
if tar --help 2>&1 | grep -q -- '--delay-directory-restore'; then
  tar -C "$extract_tmp" --delay-directory-restore -xf "$tar_path"
else
  tar -C "$extract_tmp" -xf "$tar_path"
fi
rm -rf "$dest"
rsync -a --delete "$extract_tmp/qemu-$version/" "$dest/"
rm -rf "$extract_tmp"

info "完成: $dest"
