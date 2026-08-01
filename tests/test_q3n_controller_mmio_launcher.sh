#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture_dir=$(mktemp -d "$repo_root/work/q3n-qtest-launcher.XXXXXX")
outside_dir=$(mktemp -d "$repo_root/../q3n-qtest-launcher-outside.XXXXXX")
trap 'rm -rf "$fixture_dir" "$outside_dir"' EXIT HUP INT TERM
version=42.7.13
source_dir="$fixture_dir/qemu-$version"
build_dir="$fixture_dir/build"
mkdir -p "$source_dir" "$build_dir/qemu-$version" "$outside_dir/qemu-$version"
regular="$build_dir/qemu-$version/qemu-system-x86_64"
unsigned="$build_dir/qemu-$version/qemu-system-x86_64-unsigned"
printf '#!/usr/bin/env sh\nprintf regular-fixture\n' >"$regular"
printf '#!/usr/bin/env sh\nprintf unsigned-fixture\n' >"$unsigned"
printf '#!/usr/bin/env sh\nprintf escaped-fixture\n' >"$outside_dir/qemu-$version/qemu-system-x86_64-unsigned"
chmod +x "$regular" "$unsigned" "$outside_dir/qemu-$version/qemu-system-x86_64-unsigned"

expected=/workspace/work/${fixture_dir#"$repo_root/work/"}/build/qemu-$version/qemu-system-x86_64-unsigned

actual=$(Q3N_QTEST_PRINT_PATH=1 \
    QEMU_DIR="$source_dir" \
    BUILD_DIR="$build_dir" \
    sh "$repo_root/tests/test_q3n_controller_mmio.sh")

[ "$actual" = "$expected" ] || {
    printf 'FAIL: selected QEMU path: got %s, want %s\n' "$actual" "$expected" >&2
    exit 1
}
[ "$("$regular")" = regular-fixture ]
[ "$("$repo_root${actual#/workspace}")" = unsigned-fixture ]

reject_escape() {
    if Q3N_QTEST_PRINT_PATH=1 QEMU_DIR="$source_dir" BUILD_DIR="$1" \
        sh "$repo_root/tests/test_q3n_controller_mmio.sh" >/dev/null 2>&1; then
        printf 'FAIL: launcher accepted escaped build path: %s\n' "$1" >&2
        exit 1
    fi
}

traversal_build="$repo_root/work/../../${outside_dir##*/}"
reject_escape "$traversal_build"
ln -s "$outside_dir" "$fixture_dir/escaped-build"
reject_escape "$fixture_dir/escaped-build"

printf 'ok: Q3N controller qtest launcher selects the configured build\n'
