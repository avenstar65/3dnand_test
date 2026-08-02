#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

mkdirs
log="$work_dir/q3n-multiplane-smoke.log"
nand_image="$work_dir/media/q3n-nand-multiplane.raw"
expected_pre_mark_digest=f790d342cca81bc826050f0b6ce23ce7b4c06c7f174ce97c499653e4202fd450
expected_erased_digest=71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063

line_count() {
  sed 's/\r$//' "$log" | grep -E -c "$1" || true
}

image_fingerprint() {
  stat -f '%i:%z:%m:%b' "$1" 2>/dev/null ||
    stat -c '%i:%s:%Y:%b' "$1"
}

if ! "$repo_root/scripts/run-qemu.sh" --nand-mode multiplane --fresh-nand \
     --append "MTD_SMOKE=q3n-multiplane-smoke" >"$log" 2>&1; then
  cat "$log"
  die "q3n multi-plane smoke QEMU failed"
fi

cat "$log"
guest_pattern='^q3n multi-plane guest complete logical_block=[0-9][0-9]* die=[0-9][0-9]* block_in_plane=[0-9][0-9]* page=[0-9][0-9]* main_digest=[0-9a-f][0-9a-f]*$'
guest_exact_count=$(line_count "$guest_pattern")
guest_prefix_count=$(line_count '^q3n multi-plane guest complete')
[ "$guest_exact_count" = 1 ] && [ "$guest_prefix_count" = 1 ] ||
  die "q3n multi-plane guest metadata must appear exactly once"
success_exact_count=$(line_count '^MTD smoke 测试通过，关闭虚拟机$')
success_prefix_count=$(line_count '^MTD smoke')
[ "$success_exact_count" = 1 ] && [ "$success_prefix_count" = 1 ] ||
  die "q3n multi-plane guest success stage must appear exactly once"
powerdown_count=$(line_count '^\[[[:space:]]*[0-9][0-9]*\.[0-9][0-9]*\] reboot: Power down$')
[ "$powerdown_count" = 1 ] || die "q3n multi-plane kernel powerdown marker missing"

guest_line=$(sed 's/\r$//' "$log" | grep -E "$guest_pattern")
logical_block=$(printf '%s\n' "$guest_line" | sed -n 's/.* logical_block=\([0-9][0-9]*\) .*/\1/p')
die=$(printf '%s\n' "$guest_line" | sed -n 's/.* die=\([0-9][0-9]*\) .*/\1/p')
block_in_plane=$(printf '%s\n' "$guest_line" | sed -n 's/.* block_in_plane=\([0-9][0-9]*\) .*/\1/p')
page=$(printf '%s\n' "$guest_line" | sed -n 's/.* page=\([0-9][0-9]*\) .*/\1/p')
main_digest=$(printf '%s\n' "$guest_line" | sed -n 's/.* main_digest=\([0-9a-f][0-9a-f]*\)$/\1/p')
[ "$die" = "$((logical_block % 2))" ] || die "q3n multi-plane guest die metadata is inconsistent"
[ "$block_in_plane" = "$((logical_block / 2))" ] ||
  die "q3n multi-plane guest block metadata is inconsistent"
[ "$page" = 0 ] || die "q3n multi-plane guest page metadata is inconsistent"
[ "$main_digest" = "$expected_pre_mark_digest" ] ||
  die "q3n multi-plane guest pre-mark digest is unexpected"

image_before=$(image_fingerprint "$nand_image") ||
  die "q3n multi-plane base image fingerprint failed"

"$repo_root/scripts/q3n-multiplane-media-verify.sh" \
  --image "$nand_image" --logical-block "$logical_block" --die "$die" \
  --block-in-plane "$block_in_plane" --page "$page" \
  --expected-erased-digest "$expected_erased_digest"
image_after=$(image_fingerprint "$nand_image") ||
  die "q3n multi-plane base image post-verifier fingerprint failed"
[ "$image_before" = "$image_after" ] ||
  die "q3n multi-plane verifier modified the base image"

echo "q3n multi-plane smoke passed"

info "q3n multi-plane host acceptance passed"
