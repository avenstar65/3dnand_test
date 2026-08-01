#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

"$repo_root/scripts/shell.sh" python3 \
    /workspace/tests/test_q3n_controller_mmio.py
printf 'ok: real Q3N controller MMIO multi-plane integration verified\n'
