#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-qemu-multiplane-test.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

set -- "$repo_root/tests/test_q3n_qemu_multiplane.c"
if [ -f "$repo_root/qemu/hw/mtd/q3n-multiplane.c" ]; then
    set -- "$@" "$repo_root/qemu/hw/mtd/q3n-multiplane.c"
fi

${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$repo_root/qemu/hw/mtd" \
    "$@" -o "$test_dir/test_q3n_qemu_multiplane"

"$test_dir/test_q3n_qemu_multiplane"
