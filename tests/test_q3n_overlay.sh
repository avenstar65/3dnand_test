#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_bin=$(mktemp "${TMPDIR:-/tmp}/q3n-overlay-test.XXXXXX")
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/q3n-overlay.XXXXXX")
trap 'rm -f "$test_bin"; rm -rf "$test_tmp"' EXIT HUP INT TERM

sed -n '/Q3N_MEDIA_LAYOUT_HELPERS_BEGIN/,/Q3N_MEDIA_LAYOUT_HELPERS_END/p' \
  "$repo_root/qemu/hw/mtd/q3n-media.c" > \
  "$test_tmp/q3n-media-layout-helpers.inc"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/qemu/hw/mtd" \
  -I"$test_tmp" \
  "$repo_root/tests/test_q3n_overlay.c" -o "$test_bin"
"$test_bin"
