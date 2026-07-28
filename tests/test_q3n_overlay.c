#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define Q3N_PAGE_SIZE              16384U
#define Q3N_ECC_STEP_SIZE          1024U
#define Q3N_LDPC_BYTES_PER_STEP    96U
#define Q3N_LDPC_STEPS             16U
#define Q3N_LDPC_TOTAL_BYTES       1536U
#define Q3N_LOGICAL_OOB_SIZE        1024U
#define Q3N_PHYSICAL_OOB_HEAD_OFFSET  Q3N_PAGE_SIZE
#define Q3N_PHYSICAL_OOB_HEAD_SIZE    1U
#define Q3N_PHYSICAL_LDPC_OFFSET      (Q3N_PHYSICAL_OOB_HEAD_OFFSET + 1U)
#define Q3N_PHYSICAL_LDPC_SIZE        Q3N_LDPC_TOTAL_BYTES
#define Q3N_PHYSICAL_OOB_TAIL_OFFSET  \
        (Q3N_PHYSICAL_LDPC_OFFSET + Q3N_PHYSICAL_LDPC_SIZE)
#define Q3N_PHYSICAL_OOB_TAIL_SIZE    (Q3N_LOGICAL_OOB_SIZE - 1U)
#define Q3N_PHYSICAL_PAGE_SIZE        \
        (Q3N_PHYSICAL_OOB_TAIL_OFFSET + Q3N_PHYSICAL_OOB_TAIL_SIZE)

enum {
    Q3N_FAULT_REGION_MAIN = 0,
    Q3N_FAULT_REGION_LDPC = 1,
};

#include "q3n-media-overlay.h"
#include "q3n-media-layout-helpers.inc"

static void assert_all_equal(const uint8_t *bytes, uint32_t first,
                             uint32_t end, uint8_t expected)
{
    for (uint32_t i = first; i < end; i++) {
        assert(bytes[i] == expected);
    }
}

static void assert_single_toggle(uint32_t step, uint32_t region,
                                 uint32_t first_bit,
                                 uint32_t expected_byte,
                                 uint8_t expected_mask)
{
    struct {
        uint8_t before;
        uint8_t overlay[Q3N_MEDIA_OVERLAY_STRIDE];
        uint8_t after;
    } guarded = {
        .before = 0xa5,
        .after = 0x5a,
    };

    assert(q3n_media_overlay_xor(guarded.overlay, step, region,
                                  first_bit, 1));
    for (uint32_t i = 0; i < Q3N_MEDIA_OVERLAY_STRIDE; i++) {
        assert(guarded.overlay[i] ==
               (i == expected_byte ? expected_mask : 0));
    }
    assert(guarded.before == 0xa5);
    assert(guarded.after == 0x5a);

    assert(q3n_media_overlay_xor(guarded.overlay, step, region,
                                  first_bit, 1));
    for (uint32_t i = 0; i < Q3N_MEDIA_OVERLAY_STRIDE; i++) {
        assert(guarded.overlay[i] == 0);
    }
    assert(guarded.before == 0xa5);
    assert(guarded.after == 0x5a);
}

static void test_merge_program_is_bitwise_and(void)
{
    uint8_t stored[] = { 0xff, 0xf0, 0x55, 0x00 };
    const uint8_t incoming[] = { 0x0f, 0xcc, 0xaa, 0xff };
    const uint8_t expected[] = { 0x0f, 0xc0, 0x00, 0x00 };

    q3n_media_merge_program(stored, incoming, sizeof(stored));
    assert(!memcmp(stored, expected, sizeof(stored)));
}

static void test_erased_detection_reads_bytes(void)
{
    uint8_t bytes[32];

    memset(bytes, 0xff, sizeof(bytes));
    assert(q3n_media_is_erased(bytes, sizeof(bytes)));
    bytes[17] = 0xfe;
    assert(!q3n_media_is_erased(bytes, sizeof(bytes)));
}

static void test_sparse_zero_decodes_as_erased(void)
{
    uint8_t stored[32] = { 0 };

    q3n_media_invert(stored, sizeof(stored));
    assert(q3n_media_is_erased(stored, sizeof(stored)));
}

static void test_logical_oob_merge_preserves_main_and_ldpc(void)
{
    uint8_t page[Q3N_PHYSICAL_PAGE_SIZE];
    uint8_t logical_oob[Q3N_LOGICAL_OOB_SIZE];
    uint8_t roundtrip[Q3N_LOGICAL_OOB_SIZE];
    const uint8_t main_sentinel = 0xa5;
    const uint8_t ldpc_sentinel = 0x5a;

    memset(page, 0xff, sizeof(page));
    memset(page, main_sentinel, Q3N_PAGE_SIZE);
    memset(page + Q3N_PHYSICAL_LDPC_OFFSET, ldpc_sentinel,
           Q3N_PHYSICAL_LDPC_SIZE);
    for (uint32_t i = 0; i < sizeof(logical_oob); i++) {
        logical_oob[i] = (uint8_t)(i ^ 0x5a);
    }

    q3n_media_merge_logical_oob(page, logical_oob);

    assert(Q3N_PHYSICAL_OOB_HEAD_OFFSET == 0x4000U);
    assert(Q3N_PHYSICAL_LDPC_OFFSET == 0x4001U);
    assert(Q3N_PHYSICAL_OOB_TAIL_OFFSET == 0x4601U);
    assert(Q3N_PHYSICAL_PAGE_SIZE == 0x4a00U);
    assert(page[0x4000] == logical_oob[0]);
    assert(!memcmp(page + 0x4601, logical_oob + 1, 1023));
    assert_all_equal(page, 0, 0x4000, main_sentinel);
    assert_all_equal(page, 0x4001, 0x4601, ldpc_sentinel);
    q3n_media_extract_logical_oob(page, roundtrip);
    assert(!memcmp(roundtrip, logical_oob, sizeof(logical_oob)));
    assert(q3n_media_logical_oob_matches(page, logical_oob));
}

int main(void)
{
    static const uint32_t steps[] = { 0, 1, 2, 15 };
    uint8_t overlay[Q3N_MEDIA_OVERLAY_STRIDE] = { 0 };

    assert(Q3N_MEDIA_MAIN_OVERLAY_BYTES == 16384U);
    assert(Q3N_MEDIA_LDPC_OVERLAY_BYTES == 1536U);
    assert(Q3N_MEDIA_OVERLAY_STRIDE == 17920U);

    for (uint32_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        uint32_t step = steps[i];

        assert_single_toggle(step, Q3N_FAULT_REGION_MAIN, 0,
                             step * Q3N_ECC_STEP_SIZE, 0x01);
        assert_single_toggle(step, Q3N_FAULT_REGION_MAIN,
                             Q3N_ECC_STEP_SIZE * 8 - 1,
                             step * Q3N_ECC_STEP_SIZE +
                             Q3N_ECC_STEP_SIZE - 1,
                             0x80);
        assert_single_toggle(step, Q3N_FAULT_REGION_LDPC, 0,
                             Q3N_MEDIA_MAIN_OVERLAY_BYTES +
                             step * Q3N_LDPC_BYTES_PER_STEP,
                             0x01);
        assert_single_toggle(step, Q3N_FAULT_REGION_LDPC,
                             Q3N_LDPC_BYTES_PER_STEP * 8 - 1,
                             Q3N_MEDIA_MAIN_OVERLAY_BYTES +
                             step * Q3N_LDPC_BYTES_PER_STEP +
                             Q3N_LDPC_BYTES_PER_STEP - 1,
                             0x80);
    }

    assert(!q3n_media_overlay_xor(overlay, Q3N_LDPC_STEPS,
                                   Q3N_FAULT_REGION_MAIN, 0, 1));
    assert(!q3n_media_overlay_xor(overlay, 0, 99, 0, 1));
    assert(!q3n_media_overlay_xor(overlay, 0, Q3N_FAULT_REGION_MAIN,
                                   Q3N_ECC_STEP_SIZE * 8, 1));
    assert(!q3n_media_overlay_xor(overlay, 0, Q3N_FAULT_REGION_LDPC,
                                   Q3N_LDPC_BYTES_PER_STEP * 8 - 1, 2));

    test_merge_program_is_bitwise_and();
    test_erased_detection_reads_bytes();
    test_sparse_zero_decodes_as_erased();
    test_logical_oob_merge_preserves_main_and_ldpc();

    puts("ok: q3n media overlay mapping verified");
    return 0;
}
