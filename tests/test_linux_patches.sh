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

helper_source="$tmp_dir/exact-geometry-helpers.inc"
awk '
	/^\+static inline bool nand_has_non_power_of_2_geometry/ { copying = 1 }
	/^ \/\* MLC pairing schemes \*\// { copying = 0 }
	copying && /^\+/ && !/^\+\+\+/ { print substr($0, 2) }
' "$helper_patch" >"$helper_source"
bbt_size_source="$tmp_dir/exact-bbt-size.inc"
awk '
	/^\+[[:space:]]*len = DIV_ROUND_UP_ULL\(mtd->size \/ mtd->erasesize,/ {
		copying = 1
	}
	copying && /^\+/ && !/^\+\+\+/ { print substr($0, 2) }
	copying && /^\+[[:space:]]*len = 1;/ { copying = 0 }
' "$bbt_patch" >"$bbt_size_source"

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
#define DIV_ROUND_UP_ULL(value, divisor) \
	(((value) + (divisor) - 1) / (divisor))

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

static u64 nand_bbt_allocation_bytes(struct mtd_info *mtd)
{
	u64 len;

#include "exact-bbt-size.inc"
	return len;
}

struct profile {
	const char *name;
	u32 writesize;
	u32 pages_per_block;
	u32 erasesize;
	u64 logical_blocks;
	u64 bbt_bytes;
	u64 capacity;
	u64 total_pages;
	u64 first_block_last_page;
	u64 first_block_last_offset;
	u64 second_block_first_page;
	u64 second_block_offset;
	u64 last_block_first_page;
	u64 last_block_first_offset;
	u64 final_page;
	u64 final_page_offset;
	u64 last_byte_offset;
	u64 misaligned_second_block_offset;
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
	u64 last_block;
	struct mtd_info mtd = {
		.writesize = profile->writesize,
		.erasesize = profile->erasesize,
		.size = profile->capacity,
	};
	struct nand_chip chip = {
		.options = NAND_NON_POWER_OF_2_GEOMETRY,
		.base.target_size = profile->capacity,
		.mtd = &mtd,
	};

	if (!profile->logical_blocks) {
		fprintf(stderr, "FAIL: %s logical blocks: got 0, want non-zero\n",
			profile->name);
		exit(1);
	}
	last_block = profile->logical_blocks - 1;

	expect_u64(profile->name, "pages per block",
		   nand_pages_per_eraseblock(&chip), profile->pages_per_block);
	expect_u64(profile->name, "first page offset",
		   nand_page_to_offs(&chip, 0), 0);
	expect_u64(profile->name, "last page in first block offset",
		   nand_page_to_offs(&chip, profile->first_block_last_page),
		   profile->first_block_last_offset);
	expect_u64(profile->name, "cross-block page offset",
		   nand_page_to_offs(&chip, profile->second_block_first_page),
		   profile->second_block_offset);
	expect_u64(profile->name, "cross-block page number",
		   nand_offs_to_page(&chip, profile->second_block_offset),
		   profile->second_block_first_page);
	expect_u64(profile->name, "cross-block eraseblock",
		   nand_page_to_eraseblock(&chip,
		       profile->second_block_first_page), 1);
	expect_u64(profile->name, "cross-block offset eraseblock",
		   nand_offs_to_eraseblock(&chip,
		       profile->second_block_offset), 1);
	expect_u64(profile->name, "single target",
		   nand_offs_to_target(&chip, profile->last_byte_offset), 0);
	expect_u64(profile->name, "final page in target",
		   nand_page_in_target(&chip, profile->final_page),
		   profile->final_page);
	expect_u64(profile->name, "last block first page",
		   nand_eraseblock_to_page(&chip, last_block),
		   profile->last_block_first_page);
	expect_u64(profile->name, "last block offset",
		   nand_page_to_offs(&chip, profile->last_block_first_page),
		   profile->last_block_first_offset);
	expect_u64(profile->name, "final page offset",
		   nand_page_to_offs(&chip, profile->final_page),
		   profile->final_page_offset);
	expect_u64(profile->name, "final page eraseblock",
		   nand_page_to_eraseblock(&chip, profile->final_page),
		   last_block);
	expect_u64(profile->name, "literal total pages",
		   nand_offs_to_page(&chip, profile->capacity),
		   profile->total_pages);
	expect_u64(profile->name, "erase alignment",
		   nand_offs_in_eraseblock(&chip,
		       profile->second_block_offset), 0);
	expect_u64(profile->name, "misaligned erase offset",
		   nand_offs_in_eraseblock(&chip,
		       profile->misaligned_second_block_offset), 1);
	expect_u64(profile->name, "RAM BBT bytes",
		   nand_bbt_allocation_bytes(&mtd), profile->bbt_bytes);
}

int main(void)
{
	static const struct profile profiles[] = {
		{
			"2:1", 32768, 533, 17465344, 1664, 416,
			29062332416ULL, 886912,
			532, 17432576, 533, 17465344,
			886379, 29044867072ULL,
			886911, 29062299648ULL,
			29062332415ULL, 17465345,
		},
		{
			"4:1", 65536, 320, 20971520, 1664, 416,
			34896609280ULL, 532480,
			319, 20905984, 320, 20971520,
			532160, 34875637760ULL,
			532479, 34896543744ULL,
			34896609279ULL, 20971521,
		},
		{
			"8:1", 131072, 177, 23199744, 1664, 416,
			38604374016ULL, 294528,
			176, 23068672, 177, 23199744,
			294351, 38581174272ULL,
			294527, 38604242944ULL,
			38604374015ULL, 23199745,
		},
		{
			"multiplane", 65536, 1600, 104857600, 416, 104,
			43620761600ULL, 665600,
			1599, 104792064, 1600, 104857600,
			664000, 43515904000ULL,
			665599, 43620696064ULL,
			43620761599ULL, 104857601,
		},
	};
	struct mtd_info empty = {
		.erasesize = 17465344,
	};
	unsigned int i;

	for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
		check_profile(&profiles[i]);

	expect_u64("empty", "minimum RAM BBT bytes",
		   nand_bbt_allocation_bytes(&empty), 1);
	printf("ok: exact-geometry helpers verified for 2:1, 4:1, 8:1 and multi-plane\n");
	return 0;
}
EOF

cc -std=c11 -Wall -Wextra -Werror -O2 \
	"$tmp_dir/exact-geometry.c" -o "$tmp_dir/exact-geometry"
"$tmp_dir/exact-geometry"

printf 'ok: Linux patch application is strict, idempotent and exact-geometry safe\n'
