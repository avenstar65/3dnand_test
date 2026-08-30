#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

need_cmd docker

mkdirs

if [ "$#" -eq 0 ]; then
  set -- /bin/bash
fi

tty_args="-i"
if [ -t 0 ] && [ -t 1 ]; then
  tty_args="-it"
fi

container_name=${CONTAINER_NAME:-linux-mtd-qemu-dev-$$}

docker run --rm $tty_args \
  --name "$container_name" \
  -v "$repo_root:/workspace" \
  -w /workspace \
  -e WORK_DIR=/workspace/work \
  -e KERNEL_BASE_URL \
  -e LINUX_DIR \
  -e JOBS \
  -e KERNEL_ARCH \
  -e CROSS_COMPILE \
  -e Q3N_ENABLE_MULTIPLANE_RAID \
  "$image_name" \
  "$@"
