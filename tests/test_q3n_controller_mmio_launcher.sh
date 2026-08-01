#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir=$(mktemp -d "$repo_root/work/q3n-qtest-launcher.XXXXXX")
trap 'rm -rf "$fixture_dir"' EXIT HUP INT TERM
mkdir -p "$fixture_dir/qemu-11.0.2"
unsigned="$fixture_dir/qemu-11.0.2/qemu-system-x86_64-unsigned"
: >"$unsigned"
chmod +x "$unsigned"

expected=/workspace/work/${fixture_dir#"$repo_root/work/"}/qemu-11.0.2/qemu-system-x86_64-unsigned

actual=$(Q3N_QTEST_PRINT_PATH=1 \
    QEMU_DIR="$repo_root/work/qemu/qemu-11.0.2" \
    BUILD_DIR="$fixture_dir" \
    sh "$repo_root/tests/test_q3n_controller_mmio.sh")

[ "$actual" = "$expected" ] || {
    printf 'FAIL: selected QEMU path: got %s, want %s\n' "$actual" "$expected" >&2
    exit 1
}

printf 'ok: Q3N controller qtest launcher selects the configured build\n'
