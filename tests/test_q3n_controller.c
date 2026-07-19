#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define Q3N_PAGE_SIZE              16384U
#define Q3N_PHYSICAL_OOB_SIZE      1664U
#define Q3N_LOGICAL_OOB_SIZE       128U
#define Q3N_BBM_OOB_OFFSET         0U
#define Q3N_LDPC_OOB_OFFSET        1U
#define Q3N_LDPC_BYTES_PER_STEP    96U
#define Q3N_LDPC_STEPS             16U
#define Q3N_LDPC_TOTAL_BYTES       1536U
#define Q3N_METADATA_OOB_OFFSET    1537U
#define Q3N_ECC_STEP_SIZE          1024U
#define Q3N_ECC_STRENGTH           40U
#define Q3N_ECC_NO_FAILED_STEP     UINT32_MAX

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

static void set_first_bits(uint8_t *overlay, uint32_t count)
{
    memset(overlay, 0xff, count / 8);
    if (count % 8) {
        overlay[count / 8] = (1U << (count % 8)) - 1;
    }
}

static void test_oob_mapping_preserves_ldpc(void)
{
    uint8_t logical[Q3N_LOGICAL_OOB_SIZE];
    uint8_t physical[Q3N_PHYSICAL_OOB_SIZE];
    uint8_t roundtrip[Q3N_LOGICAL_OOB_SIZE];

    for (uint32_t i = 0; i < Q3N_LOGICAL_OOB_SIZE; i++) {
        logical[i] = (uint8_t)(i ^ 0x5a);
    }
    memset(physical, 0xa5, sizeof(physical));
    q3n_logical_to_physical_oob(logical, physical);

    assert(physical[Q3N_BBM_OOB_OFFSET] == logical[0]);
    assert(!memcmp(physical + Q3N_METADATA_OOB_OFFSET, logical + 1,
                   Q3N_LOGICAL_OOB_SIZE - 1));
    for (uint32_t i = Q3N_LDPC_OOB_OFFSET;
         i < Q3N_METADATA_OOB_OFFSET; i++) {
        assert(physical[i] == 0xa5);
    }

    memset(roundtrip, 0, sizeof(roundtrip));
    q3n_physical_to_logical_oob(physical, roundtrip);
    assert(!memcmp(roundtrip, logical, sizeof(logical)));
}

static Q3NEccResult decode_erased(const uint8_t *main_overlay,
                                  const uint8_t *ldpc_overlay)
{
    uint8_t data[Q3N_PAGE_SIZE];
    uint8_t physical_oob[Q3N_PHYSICAL_OOB_SIZE];

    memset(data, 0xff, sizeof(data));
    memset(physical_oob, 0xff, sizeof(physical_oob));
    return q3n_decode_ldpc(0, data, physical_oob, main_overlay,
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
    uint8_t physical_oob[Q3N_PHYSICAL_OOB_SIZE];
    uint8_t main_overlay[Q3N_PAGE_SIZE] = { 0 };
    uint8_t ldpc_overlay[Q3N_LDPC_TOTAL_BYTES] = { 0 };
    Q3NEccResult result;

    memset(data, 0x3c, sizeof(data));
    memset(physical_oob, 0xff, sizeof(physical_oob));
    for (uint32_t step = 0; step < Q3N_LDPC_STEPS; step++) {
        q3n_generate_ldpc_step(7, step,
                               data + step * Q3N_ECC_STEP_SIZE,
                               physical_oob + Q3N_LDPC_OOB_OFFSET +
                               step * Q3N_LDPC_BYTES_PER_STEP);
    }
    physical_oob[Q3N_LDPC_OOB_OFFSET + Q3N_LDPC_BYTES_PER_STEP] ^= 1;

    result = q3n_decode_ldpc(7, data, physical_oob, main_overlay,
                             ldpc_overlay, false);
    assert(result.status == Q3N_ECC_STATUS_UNCORRECTABLE);
    assert(result.failed_step == 1);
    assert(result.failed_steps == 1);
}

int main(void)
{
    test_oob_mapping_preserves_ldpc();
    test_decode_thresholds();
    test_decode_aggregates_steps();
    test_ldpc_mismatch_is_uncorrectable();
    puts("ok: q3n controller OOB and LDPC behavior verified");
    return 0;
}
