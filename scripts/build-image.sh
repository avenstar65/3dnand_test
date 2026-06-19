#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd docker

base_image=${BASE_IMAGE:-ubuntu:24.04}

info "构建 Docker 镜像: $image_name"
info "基础镜像: $base_image"
docker build --build-arg "BASE_IMAGE=$base_image" -t "$image_name" "$repo_root"
