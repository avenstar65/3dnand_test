#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/q3n-qemu-multiplane-abi.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

mkdir -p "$test_dir/qemu-stubs/hw/core" "$test_dir/qemu-stubs/qom" \
    "$test_dir/qemu-stubs/system"

cat >"$test_dir/qemu-stubs/hw/core/irq.h" <<'EOF'
typedef void *qemu_irq;
EOF
cat >"$test_dir/qemu-stubs/qom/object.h" <<'EOF'
#define OBJECT_DECLARE_SIMPLE_TYPE(type, name) typedef struct type type;
EOF
cat >"$test_dir/qemu-stubs/system/memory.h" <<'EOF'
typedef struct MemoryRegion MemoryRegion;
EOF

cat >"$test_dir/linux.c" <<'EOF'
#include <stdio.h>
#define Q3N_HOST_TEST 1
#include "qemu_3dnand_regs.h"

int main(void)
{
    printf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
           Q3N_CAP_MULTIPLANE,
           Q3N_CMD_MP_READ_PAGE, Q3N_CMD_MP_PROGRAM_PAGE,
           Q3N_CMD_MP_READ_OOB, Q3N_CMD_MP_PROGRAM_OOB,
           Q3N_CMD_MP_ERASE_GROUP,
           Q3N_REG_MP_DIE, Q3N_REG_MP_BLOCK, Q3N_REG_MP_PAGE,
           Q3N_REG_MP_DONE_MASK, Q3N_REG_MP_FAIL_MASK,
           Q3N_REG_MP_ECC_SELECT, Q3N_REG_MP_ECC_STATUS,
           Q3N_REG_MP_ECC_MAX_BITFLIPS, Q3N_REG_MP_ECC_CORRECTED_BITS,
           Q3N_REG_MP_ECC_FAILED_STEP);
    return 0;
}
EOF
cat >"$test_dir/qemu.c" <<'EOF'
#include <stdio.h>
#include "hw/mtd/q3n-nand.h"

int main(void)
{
    printf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
           Q3N_CAP_MULTIPLANE,
           Q3N_CMD_MP_READ_PAGE, Q3N_CMD_MP_PROGRAM_PAGE,
           Q3N_CMD_MP_READ_OOB, Q3N_CMD_MP_PROGRAM_OOB,
           Q3N_CMD_MP_ERASE_GROUP,
           Q3N_REG_MP_DIE, Q3N_REG_MP_BLOCK, Q3N_REG_MP_PAGE,
           Q3N_REG_MP_DONE_MASK, Q3N_REG_MP_FAIL_MASK,
           Q3N_REG_MP_ECC_SELECT, Q3N_REG_MP_ECC_STATUS,
           Q3N_REG_MP_ECC_MAX_BITFLIPS, Q3N_REG_MP_ECC_CORRECTED_BITS,
           Q3N_REG_MP_ECC_FAILED_STEP);
    return 0;
}
EOF

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$repo_root/linux/drivers/mtd/nand/raw" "$test_dir/linux.c" \
    -o "$test_dir/linux"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I"$test_dir/qemu-stubs" \
    -I"$repo_root/qemu/include" "$test_dir/qemu.c" -o "$test_dir/qemu"

linux_values=$("$test_dir/linux")
qemu_values=$("$test_dir/qemu")
[ "$linux_values" = "$qemu_values" ] || {
    printf 'FAIL: Linux/QEMU multi-plane ABI differs\nlinux: %s\nqemu:  %s\n' \
        "$linux_values" "$qemu_values" >&2
    exit 1
}

printf 'ok: Q3N multi-plane Linux/QEMU ABI matches\n'
