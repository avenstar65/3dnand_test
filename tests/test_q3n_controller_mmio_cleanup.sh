#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

PYTHONPATH="$repo_root/tests${PYTHONPATH:+:$PYTHONPATH}" \
    python3 "$repo_root/tests/test_q3n_controller_mmio_cleanup.py"
