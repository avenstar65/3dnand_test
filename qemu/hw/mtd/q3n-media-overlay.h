/* Shared physical-bit overlay layout and XOR helpers for q3n media. */

#ifndef HW_MTD_Q3N_MEDIA_OVERLAY_H
#define HW_MTD_Q3N_MEDIA_OVERLAY_H

#define Q3N_MEDIA_MAIN_OVERLAY_BYTES  Q3N_PAGE_SIZE
#define Q3N_MEDIA_LDPC_OVERLAY_BYTES  Q3N_LDPC_TOTAL_BYTES
#define Q3N_MEDIA_OVERLAY_STRIDE      \
    (Q3N_MEDIA_MAIN_OVERLAY_BYTES + Q3N_MEDIA_LDPC_OVERLAY_BYTES)

static inline bool q3n_media_overlay_range(uint32_t step, uint32_t region,
                                           uint32_t first_bit, uint32_t count,
                                           uint32_t *overlay_first_bit)
{
    uint32_t limit;
    uint32_t region_first_bit;

    if (step >= Q3N_LDPC_STEPS) {
        return false;
    }
    switch (region) {
    case Q3N_FAULT_REGION_MAIN:
        limit = Q3N_ECC_STEP_SIZE * 8;
        region_first_bit = 0;
        break;
    case Q3N_FAULT_REGION_LDPC:
        limit = Q3N_LDPC_BYTES_PER_STEP * 8;
        region_first_bit = Q3N_MEDIA_MAIN_OVERLAY_BYTES * 8;
        break;
    default:
        return false;
    }
    if (first_bit > limit || count > limit - first_bit) {
        return false;
    }
    if (overlay_first_bit) {
        *overlay_first_bit = region_first_bit + step * limit + first_bit;
    }
    return true;
}

static inline bool q3n_media_overlay_xor(uint8_t *overlay, uint32_t step,
                                         uint32_t region, uint32_t first_bit,
                                         uint32_t count)
{
    uint32_t overlay_first_bit;
    uint32_t bit;

    if (!q3n_media_overlay_range(step, region, first_bit, count,
                                 &overlay_first_bit)) {
        return false;
    }
    for (bit = 0; bit < count; bit++) {
        uint32_t overlay_bit = overlay_first_bit + bit;

        overlay[overlay_bit / 8] ^= 1U << (overlay_bit % 8);
    }
    return true;
}

#endif
