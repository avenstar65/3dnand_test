#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define Q3N_PAGE_SIZE              16384U
#define Q3N_LOGICAL_OOB_SIZE       128U
#define Q3N_LDPC_BYTES_PER_STEP    96U
#define Q3N_LDPC_STEPS             16U
#define Q3N_LDPC_TOTAL_BYTES       1536U
#define Q3N_PHYSICAL_OOB_HEAD_OFFSET Q3N_PAGE_SIZE
#define Q3N_PHYSICAL_OOB_HEAD_SIZE   1U
#define Q3N_PHYSICAL_LDPC_OFFSET     \
    (Q3N_PHYSICAL_OOB_HEAD_OFFSET + Q3N_PHYSICAL_OOB_HEAD_SIZE)
#define Q3N_PHYSICAL_LDPC_SIZE       Q3N_LDPC_TOTAL_BYTES
#define Q3N_PHYSICAL_OOB_TAIL_OFFSET \
    (Q3N_PHYSICAL_LDPC_OFFSET + Q3N_PHYSICAL_LDPC_SIZE)
#define Q3N_PHYSICAL_OOB_TAIL_SIZE   (Q3N_LOGICAL_OOB_SIZE - 1U)
#define Q3N_PHYSICAL_PAGE_SIZE       \
    (Q3N_PHYSICAL_OOB_TAIL_OFFSET + Q3N_PHYSICAL_OOB_TAIL_SIZE)
#define Q3N_ECC_STEP_SIZE          1024U
#define Q3N_ECC_STRENGTH           40U
#define Q3N_ECC_NO_FAILED_STEP     UINT32_MAX
#define Q3N_DIES                   2U
#define Q3N_PLANES_PER_DIE         4U
#define Q3N_BLOCKS_PER_PLANE       247U
#define Q3N_PAGES_PER_BLOCK        1600U
#define Q3N_MP_ALL_PLANES          0x0fU

#define cpu_to_le32(value) (value)
#define cpu_to_le64(value) (value)

enum {
    Q3N_ECC_STATUS_CLEAN = 0,
    Q3N_ECC_STATUS_CORRECTED = 1U << 0,
    Q3N_ECC_STATUS_UNCORRECTABLE = 1U << 1,
};

static int ctpop8(uint8_t value)
{
    return __builtin_popcount(value);
}

static uint32_t crc32c(uint32_t crc, const uint8_t *data,
                       unsigned int length)
{
    while (length--) {
        crc = (crc << 5) ^ (crc >> 27) ^ *data++;
    }
    return crc ^ UINT32_MAX;
}

#include "q3n-controller-helpers.inc"

static void test_multiplane_descriptor_validation(void)
{
    Q3NMultiPlaneDesc d = {
        .die = 0,
        .plane_mask = 0x0f,
        .addr = {
            3ULL * Q3N_PAGE_SIZE,
            ((uint64_t)Q3N_BLOCKS_PER_PLANE * Q3N_PAGES_PER_BLOCK + 3) *
                Q3N_PAGE_SIZE,
            ((uint64_t)2 * Q3N_BLOCKS_PER_PLANE * Q3N_PAGES_PER_BLOCK + 3) *
                Q3N_PAGE_SIZE,
            ((uint64_t)3 * Q3N_BLOCKS_PER_PLANE * Q3N_PAGES_PER_BLOCK + 3) *
                Q3N_PAGE_SIZE,
        },
        .staged = {
            Q3N_PAGE_SIZE,
            Q3N_PAGE_SIZE,
            Q3N_PAGE_SIZE,
            Q3N_PAGE_SIZE,
        },
    };

    assert(q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));

    d.addr[3] += Q3N_PAGE_SIZE;
    assert(!q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));
    d.addr[3] -= Q3N_PAGE_SIZE;

    d.die = 1;
    assert(!q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));
    d.die = 0;

    d.plane_mask = 0;
    assert(!q3n_mp_validate_desc(&d, Q3N_PAGE_SIZE, false));
}

static void q3n_media_merge_program(uint8_t *stored,
                                    const uint8_t *incoming, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        stored[i] &= incoming[i];
    }
}

#include "q3n-media-layout-helpers.inc"

static void assert_all_equal(const uint8_t *bytes, uint32_t first,
                             uint32_t end, uint8_t expected)
{
    for (uint32_t i = first; i < end; i++) {
        assert(bytes[i] == expected);
    }
}

static void set_first_bits(uint8_t *overlay, uint32_t count)
{
    memset(overlay, 0xff, count / 8);
    if (count % 8) {
        overlay[count / 8] = (1U << (count % 8)) - 1;
    }
}

static Q3NEccResult decode_erased(const uint8_t *main_overlay,
                                  const uint8_t *ldpc_overlay)
{
    uint8_t data[Q3N_PAGE_SIZE];
    uint8_t ldpc[Q3N_LDPC_TOTAL_BYTES];

    memset(data, 0xff, sizeof(data));
    memset(ldpc, 0xff, sizeof(ldpc));
    return q3n_decode_ldpc(0, data, ldpc, main_overlay,
                           ldpc_overlay, true);
}

static void test_decode_thresholds(void)
{
    uint8_t main_overlay[Q3N_PAGE_SIZE] = { 0 };
    uint8_t ldpc_overlay[Q3N_LDPC_TOTAL_BYTES] = { 0 };
    Q3NEccResult result;

    result = decode_erased(main_overlay, ldpc_overlay);
    assert(result.status == Q3N_ECC_STATUS_CLEAN);
    assert(result.ecc_max_bitflips == 0);
    assert(result.ecc_corrected_bits == 0);
    assert(result.failed_step == Q3N_ECC_NO_FAILED_STEP);

    set_first_bits(main_overlay, 39);
    result = decode_erased(main_overlay, ldpc_overlay);
    assert(result.status == Q3N_ECC_STATUS_CORRECTED);
    assert(result.ecc_max_bitflips == 39);
    assert(result.ecc_corrected_bits == 39);
    assert(result.failed_step == Q3N_ECC_NO_FAILED_STEP);

    memset(main_overlay, 0, sizeof(main_overlay));
    set_first_bits(main_overlay, 40);
    result = decode_erased(main_overlay, ldpc_overlay);
    assert(result.status == Q3N_ECC_STATUS_CORRECTED);
    assert(result.ecc_max_bitflips == 40);
    assert(result.ecc_corrected_bits == 40);

    memset(main_overlay, 0, sizeof(main_overlay));
    set_first_bits(main_overlay, 41);
    result = decode_erased(main_overlay, ldpc_overlay);
    assert(result.status == Q3N_ECC_STATUS_UNCORRECTABLE);
    assert(result.failed_step == 0);
    assert(result.failed_steps == 1);

    memset(main_overlay, 0, sizeof(main_overlay));
    set_first_bits(main_overlay, 25);
    set_first_bits(ldpc_overlay, 16);
    result = decode_erased(main_overlay, ldpc_overlay);
    assert(result.status == Q3N_ECC_STATUS_UNCORRECTABLE);
    assert(result.failed_step == 0);
    assert(result.failed_steps == 1);
}

static void test_decode_aggregates_steps(void)
{
    uint8_t main_overlay[Q3N_PAGE_SIZE] = { 0 };
    uint8_t ldpc_overlay[Q3N_LDPC_TOTAL_BYTES] = { 0 };
    Q3NEccResult result;

    set_first_bits(main_overlay, 30);
    set_first_bits(main_overlay + Q3N_ECC_STEP_SIZE, 20);
    result = decode_erased(main_overlay, ldpc_overlay);
    assert(result.status == Q3N_ECC_STATUS_CORRECTED);
    assert(result.ecc_max_bitflips == 30);
    assert(result.ecc_corrected_bits == 50);
}

static void test_ldpc_mismatch_is_uncorrectable(void)
{
    uint8_t data[Q3N_PAGE_SIZE];
    uint8_t ldpc[Q3N_LDPC_TOTAL_BYTES];
    uint8_t main_overlay[Q3N_PAGE_SIZE] = { 0 };
    uint8_t ldpc_overlay[Q3N_LDPC_TOTAL_BYTES] = { 0 };
    Q3NEccResult result;

    memset(data, 0x3c, sizeof(data));
    memset(ldpc, 0xff, sizeof(ldpc));
    for (uint32_t step = 0; step < Q3N_LDPC_STEPS; step++) {
        q3n_generate_ldpc_step(7, step,
                               data + step * Q3N_ECC_STEP_SIZE,
                               ldpc + step * Q3N_LDPC_BYTES_PER_STEP);
    }
    ldpc[Q3N_LDPC_BYTES_PER_STEP] ^= 1;

    result = q3n_decode_ldpc(7, data, ldpc, main_overlay,
                             ldpc_overlay, false);
    assert(result.status == Q3N_ECC_STATUS_UNCORRECTABLE);
    assert(result.failed_step == 1);
    assert(result.failed_steps == 1);
}

static void test_oob_transfers_are_independent_of_main_length(void)
{
    assert(q3n_oob_transfer_valid(0, Q3N_LOGICAL_OOB_SIZE, false));
    assert(q3n_oob_transfer_valid(Q3N_LOGICAL_OOB_SIZE,
                                  Q3N_LOGICAL_OOB_SIZE, true));
    assert(!q3n_oob_transfer_valid(Q3N_LOGICAL_OOB_SIZE - 1,
                                   Q3N_LOGICAL_OOB_SIZE, true));
    assert(q3n_oob_transfer_valid(Q3N_LOGICAL_OOB_SIZE + 1,
                                  Q3N_LOGICAL_OOB_SIZE, true));
    assert(q3n_oob_transfer_valid(Q3N_PAGE_SIZE, Q3N_LOGICAL_OOB_SIZE,
                                  true));
    assert(!q3n_oob_transfer_valid(0, Q3N_LOGICAL_OOB_SIZE - 1, false));
}

static void test_oob_staging_replaces_completed_main_transfer(void)
{
    uint8_t staging[Q3N_PAGE_SIZE + Q3N_LOGICAL_OOB_SIZE];
    uint8_t logical_oob[Q3N_LOGICAL_OOB_SIZE];
    uint32_t data_pos = Q3N_PAGE_SIZE;
    uint32_t data_count = Q3N_PAGE_SIZE;

    memset(staging, 0xa5, sizeof(staging));
    for (uint32_t i = 0; i < sizeof(logical_oob); i++) {
        logical_oob[i] = (uint8_t)(i ^ 0x5a);
    }

    q3n_reset_oob_staging(&data_pos, &data_count);
    assert(data_pos == 0);
    assert(data_count == 0);
    memcpy(staging + data_pos, logical_oob, sizeof(logical_oob));
    data_pos += sizeof(logical_oob);
    data_count = data_pos;

    assert(data_count == Q3N_LOGICAL_OOB_SIZE);
    assert(!memcmp(staging, logical_oob, sizeof(logical_oob)));
    assert(staging[Q3N_LOGICAL_OOB_SIZE] == 0xa5);
}

static void test_physical_page_oob_mapping_preserves_main_and_ldpc(void)
{
    uint8_t physical_page[Q3N_PHYSICAL_PAGE_SIZE];
    uint8_t programmed_page[Q3N_PHYSICAL_PAGE_SIZE];
    uint8_t logical[Q3N_LOGICAL_OOB_SIZE];
    const uint8_t main_sentinel = 0xa5;
    const uint8_t ldpc_sentinel = 0x5a;

    memset(physical_page, 0xff, sizeof(physical_page));
    memset(physical_page, main_sentinel, Q3N_PAGE_SIZE);
    memset(physical_page + Q3N_PHYSICAL_LDPC_OFFSET, ldpc_sentinel,
           Q3N_PHYSICAL_LDPC_SIZE);
    physical_page[0x4000] = 0x3c;
    for (uint32_t i = 0; i < Q3N_PHYSICAL_OOB_TAIL_SIZE; i++) {
        physical_page[0x4601 + i] = (uint8_t)(i ^ 0x5a);
    }

    q3n_media_extract_logical_oob(physical_page, logical);
    assert(logical[0] == physical_page[0x4000]);
    assert(!memcmp(logical + 1, physical_page + 0x4601, 127));
    assert_all_equal(physical_page, 0, 0x4000, main_sentinel);
    assert_all_equal(physical_page, 0x4001, 0x4601, ldpc_sentinel);

    memset(programmed_page, 0xff, sizeof(programmed_page));
    memset(programmed_page, main_sentinel, Q3N_PAGE_SIZE);
    memset(programmed_page + Q3N_PHYSICAL_LDPC_OFFSET, ldpc_sentinel,
           Q3N_PHYSICAL_LDPC_SIZE);
    q3n_media_merge_logical_oob(programmed_page, logical);
    assert(programmed_page[0x4000] == logical[0]);
    assert(!memcmp(programmed_page + 0x4601, logical + 1, 127));
    assert(q3n_media_logical_oob_matches(programmed_page, logical));
    assert_all_equal(programmed_page, 0, 0x4000, main_sentinel);
    assert_all_equal(programmed_page, 0x4001, 0x4601, ldpc_sentinel);
}

int main(void)
{
    test_multiplane_descriptor_validation();
    test_decode_thresholds();
    test_decode_aggregates_steps();
    test_ldpc_mismatch_is_uncorrectable();
    test_oob_transfers_are_independent_of_main_length();
    test_oob_staging_replaces_completed_main_transfer();
    test_physical_page_oob_mapping_preserves_main_and_ldpc();
    puts("ok: q3n controller OOB and LDPC behavior verified");
    return 0;
}
