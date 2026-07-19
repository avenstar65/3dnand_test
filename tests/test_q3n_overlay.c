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

enum {
    Q3N_FAULT_REGION_MAIN = 0,
    Q3N_FAULT_REGION_LDPC = 1,
};

#include "q3n-media-overlay.h"

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

    puts("ok: q3n overlay layout and xor behavior verified");
    return 0;
}
