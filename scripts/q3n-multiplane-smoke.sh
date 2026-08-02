#!/usr/bin/env sh
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/common.sh"

mkdirs
log="$work_dir/q3n-multiplane-smoke.log"
nand_image="$work_dir/media/q3n-nand-multiplane.raw"

if ! "$repo_root/scripts/run-qemu.sh" --nand-mode multiplane --fresh-nand \
     --append "MTD_SMOKE=q3n-multiplane-smoke" >"$log" 2>&1; then
  cat "$log"
  die "q3n multi-plane smoke QEMU failed"
fi

cat "$log"
for marker in 'q3n multi-plane guest complete' 'MTD smoke 测试通过'; do
  grep -Fq "$marker" "$log" || die "q3n multi-plane smoke marker missing: $marker"
done

guest_line=$(tr -d '\r' < "$log" | grep \
  '^q3n multi-plane guest complete logical_block=[0-9][0-9]* die=[0-9][0-9]* block_in_plane=[0-9][0-9]* page=0 main_digest=[0-9a-f][0-9a-f]*$' | tail -n 1)
[ -n "$guest_line" ] || die "q3n multi-plane guest metadata missing"
logical_block=$(printf '%s\n' "$guest_line" | sed -n 's/.* logical_block=\([0-9][0-9]*\) .*/\1/p')
main_digest=$(printf '%s\n' "$guest_line" | sed -n 's/.* main_digest=\([0-9a-f][0-9a-f]*\)$/\1/p')
[ "${#main_digest}" -eq 64 ] || die "q3n multi-plane guest digest is invalid"
erased_digest=71189f7fb6aed638640078fba3a35fda6c39c8962e74dcc75935aac948da9063

"$repo_root/scripts/q3n-multiplane-media-verify.sh" \
  --image "$nand_image" --logical-block "$logical_block" \
  --expected-erased-digest "$erased_digest"

echo "q3n multi-plane smoke passed"

info "q3n multi-plane host acceptance passed"
