#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd docker

info "构建 Docker 镜像: $image_name"
docker build -t "$image_name" "$repo_root"

