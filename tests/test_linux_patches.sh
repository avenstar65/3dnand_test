#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-linux-patches.XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

write_fixture()
{
	source_dir=$1
	patch_dir=$2

	mkdir -p "$source_dir" "$patch_dir"
	printf 'alpha\n' >"$source_dir/sample.txt"
	printf '%s\n' \
		'diff --git a/sample.txt b/sample.txt' \
		'index 4a58007..fbbee86 100644' \
		'--- a/sample.txt' \
		'+++ b/sample.txt' \
		'@@ -1 +1 @@' \
		'-alpha' \
		'+beta' >"$patch_dir/0001-alpha-to-beta.patch"
	printf '%s\n' \
		'diff --git a/sample.txt b/sample.txt' \
		'index fbbee86..f2e2e55 100644' \
		'--- a/sample.txt' \
		'+++ b/sample.txt' \
		'@@ -1 +1,2 @@' \
		' beta' \
		'+gamma' >"$patch_dir/0002-add-gamma.patch"
}

source_dir="$tmp_dir/linux"
patch_dir="$tmp_dir/patches"
write_fixture "$source_dir" "$patch_dir"

Q3N_LINUX_PATCH_DIR="$patch_dir" \
	sh "$repo_root/scripts/apply-linux-patches.sh" "$source_dir"

expected=$(printf 'beta\ngamma')
actual=$(cat "$source_dir/sample.txt")
[ "$actual" = "$expected" ] ||
	fail "first application produced unexpected content: $actual"

first_sum=$(cksum "$source_dir/sample.txt")
Q3N_LINUX_PATCH_DIR="$patch_dir" \
	sh "$repo_root/scripts/apply-linux-patches.sh" "$source_dir"
second_sum=$(cksum "$source_dir/sample.txt")
[ "$first_sum" = "$second_sum" ] ||
	fail "second application changed an already-patched tree"

if Q3N_LINUX_PATCH_DIR="$patch_dir" \
	sh "$repo_root/scripts/apply-linux-patches.sh" "$tmp_dir/missing" \
	>"$tmp_dir/missing.out" 2>&1; then
	fail "missing source directory was accepted"
fi

broken_source="$tmp_dir/broken-linux"
broken_patches="$tmp_dir/broken-patches"
mkdir -p "$broken_source" "$broken_patches"
printf 'unrelated\n' >"$broken_source/sample.txt"
cp "$patch_dir/0001-alpha-to-beta.patch" "$broken_patches/0001-broken.patch"

if Q3N_LINUX_PATCH_DIR="$broken_patches" \
	sh "$repo_root/scripts/apply-linux-patches.sh" "$broken_source" \
	>"$tmp_dir/broken.out" 2>&1; then
	fail "a patch that is neither applicable nor reversible was accepted"
fi
grep -q '无法应用' "$tmp_dir/broken.out" ||
	fail "broken-patch diagnostic did not identify the application failure"

printf 'ok: Linux patch application is strict and idempotent\n'
