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

helper_patch="$repo_root/linux/patches/0001-mtd-rawnand-add-exact-geometry-helpers.patch"
bbt_patch="$repo_root/linux/patches/0003-mtd-rawnand-use-exact-geometry-in-NAND-BBT.patch"
[ -f "$helper_patch" ] || fail "tracked exact-geometry helper patch is unavailable"
[ -f "$bbt_patch" ] || fail "tracked exact-geometry BBT patch is unavailable"

helper_source="$tmp_dir/exact-geometry-helpers.inc"
awk '
	/^\+static inline bool nand_has_non_power_of_2_geometry/ { copying = 1 }
	/^ \/\* MLC pairing schemes \*\// { copying = 0 }
	copying && /^\+/ && !/^\+\+\+/ { print substr($0, 2) }
' "$helper_patch" >"$helper_source"
grep -q '^static inline u64 nand_page_to_offs' "$helper_source" ||
	fail "patched exact-geometry page helper is unavailable"
grep -q '^static inline u64 nand_eraseblock_to_page' "$helper_source" ||
	fail "patched exact-geometry eraseblock helper is unavailable"
grep -q '^+.*DIV_ROUND_UP_ULL(mtd->size / mtd->erasesize, 4)' "$bbt_patch" ||
	fail "patched NAND BBT does not allocate two exact bits per eraseblock"

cat >"$tmp_dir/exact-geometry.c" <<'EOF'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t loff_t;

#define BIT(n) (1U << (n))
#define NAND_NON_POWER_OF_2_GEOMETRY BIT(15)

struct mtd_info {
	u32 writesize;
	u32 erasesize;
	u64 size;
};

struct nand_device {
	u64 target_size;
};

struct nand_chip {
	u32 options;
	u32 page_shift;
	u32 phys_erase_shift;
	u32 chip_shift;
	u64 pagemask;
	struct nand_device base;
	struct mtd_info *mtd;
};

#define nand_to_mtd(chip) ((chip)->mtd)

static inline u64 nanddev_target_size(const struct nand_device *nand)
{
	return nand->target_size;
}
EOF
cat "$helper_source" >>"$tmp_dir/exact-geometry.c"
cat >>"$tmp_dir/exact-geometry.c" <<'EOF'

struct profile {
	const char *name;
	u32 writesize;
	u32 pages_per_block;
	u32 erasesize;
};

static void expect_u64(const char *profile, const char *field,
		       u64 got, u64 want)
{
	if (got != want) {
		fprintf(stderr, "FAIL: %s %s: got %llu, want %llu\n",
			profile, field, (unsigned long long)got,
			(unsigned long long)want);
		exit(1);
	}
}

static void check_profile(const struct profile *profile)
{
	const u64 blocks = 1664;
	const u64 total_pages = blocks * profile->pages_per_block;
	const u64 size = blocks * profile->erasesize;
	struct mtd_info mtd = {
		.writesize = profile->writesize,
		.erasesize = profile->erasesize,
		.size = size,
	};
	struct nand_chip chip = {
		.options = NAND_NON_POWER_OF_2_GEOMETRY,
		.base.target_size = size,
		.mtd = &mtd,
	};

	expect_u64(profile->name, "pages per block",
		   nand_pages_per_eraseblock(&chip), profile->pages_per_block);
	expect_u64(profile->name, "first page offset",
		   nand_page_to_offs(&chip, 0), 0);
	expect_u64(profile->name, "last page in first block offset",
		   nand_page_to_offs(&chip, profile->pages_per_block - 1),
		   profile->erasesize - profile->writesize);
	expect_u64(profile->name, "cross-block page offset",
		   nand_page_to_offs(&chip, profile->pages_per_block),
		   profile->erasesize);
	expect_u64(profile->name, "cross-block page number",
		   nand_offs_to_page(&chip, profile->erasesize),
		   profile->pages_per_block);
	expect_u64(profile->name, "cross-block eraseblock",
		   nand_page_to_eraseblock(&chip, profile->pages_per_block), 1);
	expect_u64(profile->name, "cross-block offset eraseblock",
		   nand_offs_to_eraseblock(&chip, profile->erasesize), 1);
	expect_u64(profile->name, "single target",
		   nand_offs_to_target(&chip, size - 1), 0);
	expect_u64(profile->name, "final page in target",
		   nand_page_in_target(&chip, total_pages - 1),
		   total_pages - 1);
	expect_u64(profile->name, "last block first page",
		   nand_eraseblock_to_page(&chip, blocks - 1),
		   (blocks - 1) * profile->pages_per_block);
	expect_u64(profile->name, "last block offset",
		   nand_page_to_offs(&chip,
		       nand_eraseblock_to_page(&chip, blocks - 1)),
		   (blocks - 1) * profile->erasesize);
	expect_u64(profile->name, "final page offset",
		   nand_page_to_offs(&chip, total_pages - 1),
		   size - profile->writesize);
	expect_u64(profile->name, "final page eraseblock",
		   nand_page_to_eraseblock(&chip, total_pages - 1), blocks - 1);
	expect_u64(profile->name, "erase alignment",
		   nand_offs_in_eraseblock(&chip, profile->erasesize), 0);
	expect_u64(profile->name, "misaligned erase offset",
		   nand_offs_in_eraseblock(&chip, profile->erasesize + 1), 1);
	expect_u64(profile->name, "RAM BBT bytes",
		   ((mtd.size / mtd.erasesize) + 3) / 4, 416);
}

int main(void)
{
	static const struct profile profiles[] = {
		{ "2:1", 32768, 533, 17465344 },
		{ "4:1", 65536, 320, 20971520 },
		{ "8:1", 131072, 177, 23199744 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
		check_profile(&profiles[i]);

	printf("ok: exact-geometry helpers verified for 2:1, 4:1 and 8:1\n");
	return 0;
}
EOF

cc -std=c11 -Wall -Wextra -Werror -O2 \
	"$tmp_dir/exact-geometry.c" -o "$tmp_dir/exact-geometry"
"$tmp_dir/exact-geometry"

printf 'ok: Linux patch application is strict, idempotent and exact-geometry safe\n'
