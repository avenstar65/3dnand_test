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

docker run --rm $tty_args \
  --name linux-mtd-qemu-dev \
  -v "$repo_root:/workspace" \
  -w /workspace \
  -e WORK_DIR=/workspace/work \
  "$image_name" \
  "$@"
