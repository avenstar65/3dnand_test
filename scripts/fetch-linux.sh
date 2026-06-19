#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

channel=stable
version=
force=0
redownload=0

usage() {
  cat <<'USAGE'
用法: scripts/fetch-linux.sh [--stable|--mainline|--longterm] [--version X.Y.Z] [--force] [--redownload]

默认从 kernel.org releases.json 解析 stable 版本，并下载对应 tarball。
--force      重新解压源码目录，但复用已有 tarball。
--redownload 重新下载 tarball。
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --stable) channel=stable ;;
    --mainline) channel=mainline ;;
    --longterm) channel=longterm ;;
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
need_cmd python3
need_cmd rsync
need_cmd tar
mkdirs

if [ -z "$version" ]; then
  releases_json="$downloads_dir/kernel-releases.json"
  info "获取 kernel.org 版本元数据"
  curl -fsSL https://www.kernel.org/releases.json -o "$releases_json"
  version=$(python3 - "$channel" "$releases_json" <<'PY'
import json
import sys

channel = sys.argv[1]
path = sys.argv[2]
data = json.load(open(path, encoding="utf-8"))

for rel in data.get("releases", []):
    moniker = rel.get("moniker", "")
    if channel == "longterm" and moniker == "longterm":
        print(rel["version"])
        break
    if moniker == channel:
        print(rel["version"])
        break
else:
    raise SystemExit(f"cannot find kernel.org channel: {channel}")
PY
)
fi

major=${version%%.*}
tar_name="linux-$version.tar.xz"
kernel_base_url=${KERNEL_BASE_URL:-https://cdn.kernel.org/pub/linux/kernel}
url="$kernel_base_url/v$major.x/$tar_name"
tar_path="$downloads_dir/$tar_name"
dest="$linux_work_dir/linux-$version"

if [ -d "$dest" ] && [ "$force" -ne 1 ]; then
  info "复用已有内核源码: $dest"
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
  info "下载 Linux $version: $url"
  curl --continue-at - -fL "$url" -o "$tmp_tar"
  mv "$tmp_tar" "$tar_path"
fi

info "解压到 $dest"
mkdir -p "$linux_work_dir"
extract_tmp=${TMPDIR:-/tmp}/linux-extract-$$
rm -rf "$extract_tmp"
mkdir -p "$extract_tmp"
tar -C "$extract_tmp" --delay-directory-restore -xf "$tar_path"
rm -rf "$dest"
rsync -a --delete "$extract_tmp/linux-$version/" "$dest/"
rm -rf "$extract_tmp"

info "完成: $dest"
