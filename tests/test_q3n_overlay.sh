#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_bin=$(mktemp "${TMPDIR:-/tmp}/q3n-overlay-test.XXXXXX")
trap 'rm -f "$test_bin"' EXIT HUP INT TERM

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/qemu/hw/mtd" \
  "$repo_root/tests/test_q3n_overlay.c" -o "$test_bin"
"$test_bin"
