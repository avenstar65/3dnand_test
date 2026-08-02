#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "q3n-multiplane.h"

struct fake_media {
    uint32_t blocks[Q3N_MULTIPLANE_WIDTH];
    uint32_t pages[Q3N_MULTIPLANE_WIDTH];
    bool raw[Q3N_MULTIPLANE_WIDTH];
    unsigned calls;
    unsigned fail_plane;
    unsigned ecc_bad_plane;
    uint8_t read_byte;
};

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %u, want %u\n", name, got, want);
        exit(1);
    }
}

static void expect_true(const char *name, bool value)
{
    if (!value) {
        fail(name);
    }
}

static int record(struct fake_media *fake, uint32_t block, uint32_t page)
{
    if (fake->calls == Q3N_MULTIPLANE_WIDTH) {
        fail("too many media callbacks");
    }
    fake->blocks[fake->calls] = block;
    fake->pages[fake->calls] = page;
    return fake->calls++ == fake->fail_plane ? -EIO : 0;
}

static int fake_read(void *opaque, uint32_t block, uint32_t page,
                     uint8_t *destination, bool raw,
                     Q3NMultiplaneEccResult *ecc)
{
    struct fake_media *fake = opaque;
    unsigned plane = fake->calls;
    int ret = record(fake, block, page);

    fake->raw[plane] = raw;
    if (ret) {
        memset(destination, 0, Q3N_MULTIPLANE_MAIN_SLICE_SIZE);
        return ret;
    }
    memset(destination, fake->read_byte + plane,
           Q3N_MULTIPLANE_MAIN_SLICE_SIZE);
    ecc->status = plane == fake->ecc_bad_plane ? 2U : 1U;
    ecc->max_bitflips = plane + 10;
    ecc->corrected_bits = plane + 20;
    ecc->failed_step = plane + 30;
    return 0;
}

static int fake_program(void *opaque, uint32_t block, uint32_t page,
                        const uint8_t *source)
{
    struct fake_media *fake = opaque;

    expect_u32("program slice byte", source[0], fake->read_byte + fake->calls);
    return record(fake, block, page);
}

static int fake_read_oob(void *opaque, uint32_t block, uint32_t page,
                         uint8_t *destination)
{
    struct fake_media *fake = opaque;
    unsigned plane = fake->calls;
    int ret = record(fake, block, page);

    if (!ret) {
        memset(destination, fake->read_byte + plane,
               Q3N_MULTIPLANE_OOB_SLICE_SIZE);
    }
    return ret;
}

static int fake_program_oob(void *opaque, uint32_t block, uint32_t page,
                            const uint8_t *source)
{
    struct fake_media *fake = opaque;

    expect_u32("oob program slice byte", source[0],
               fake->read_byte + fake->calls);
    return record(fake, block, page);
}

static int fake_erase(void *opaque, uint32_t block, uint32_t page)
{
    return record(opaque, block, page);
}

static const Q3NMultiplaneOps fake_ops = {
    .read = fake_read,
    .program = fake_program,
    .read_oob = fake_read_oob,
    .program_oob = fake_program_oob,
    .erase = fake_erase,
};

static Q3NMultiplaneRequest valid_request(void)
{
    return (Q3NMultiplaneRequest) {
        .die = 1,
        .block = 207,
        .page = 1599,
        .main_len = Q3N_MULTIPLANE_MAIN_SIZE,
        .oob_len = Q3N_MULTIPLANE_OOB_SIZE,
    };
}

static void expect_four_members(const struct fake_media *fake,
                                uint32_t first_block)
{
    unsigned plane;

    expect_u32("callback count", fake->calls, Q3N_MULTIPLANE_WIDTH);
    for (plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        expect_u32("physical block", fake->blocks[plane],
                   first_block + plane * Q3N_MULTIPLANE_BLOCKS_PER_PLANE);
        expect_u32("physical page", fake->pages[plane], 1599);
    }
}

static void expect_complete(const Q3NMultiplaneResult *result,
                            uint8_t done, uint8_t failed)
{
    expect_u32("done mask", result->done_mask, done);
    expect_u32("fail mask", result->fail_mask, failed);
    expect_u32("completed group members", result->done_mask | result->fail_mask,
               0x0f);
    expect_u32("disjoint group masks", result->done_mask & result->fail_mask,
               0);
}

static void test_main_read_maps_slices_and_ecc(void)
{
    struct fake_media fake = { .fail_plane = 1, .ecc_bad_plane = 2,
                               .read_byte = 0x20 };
    Q3NMultiplaneRequest request = valid_request();
    Q3NMultiplaneResult result;
    uint8_t data[Q3N_MULTIPLANE_MAIN_SIZE];

    memset(data, 0, sizeof(data));
    expect_u32("read return", q3n_multiplane_execute_read(&request, &fake_ops,
               &fake, data, &result), 0);
    expect_four_members(&fake, 1195);
    expect_complete(&result, 0x0d, 0x02);
    expect_u32("failed read remains erased", data[Q3N_MULTIPLANE_MAIN_SLICE_SIZE],
               0xff);
    expect_u32("successful read slice", data[2 * Q3N_MULTIPLANE_MAIN_SLICE_SIZE],
               0x22);
    expect_u32("uncorrectable is done", result.ecc[2].status, 2);
    expect_u32("uncorrectable is not media fail", result.fail_mask, 0x02);
    expect_u32("ecc record retained", result.ecc[3].corrected_bits, 23);
}

static void test_raw_read_forwards_raw_and_clears_ecc(void)
{
    struct fake_media fake = { .fail_plane = Q3N_MULTIPLANE_WIDTH,
                               .ecc_bad_plane = 1, .read_byte = 0x40 };
    Q3NMultiplaneRequest request = valid_request();
    Q3NMultiplaneResult result;
    uint8_t data[Q3N_MULTIPLANE_MAIN_SIZE];
    unsigned plane;

    request.raw = true;
    expect_u32("raw read return", q3n_multiplane_execute_read(&request, &fake_ops,
               &fake, data, &result), 0);
    expect_four_members(&fake, 1195);
    expect_complete(&result, 0x0f, 0);
    for (plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        expect_true("raw forwarded", fake.raw[plane]);
        expect_u32("raw ECC status is zero", result.ecc[plane].status, 0);
        expect_u32("raw corrected bits is zero", result.ecc[plane].corrected_bits,
                   0);
    }
}

static void test_oob_and_mutating_operations_continue_after_failure(void)
{
    struct fake_media fake = { .fail_plane = 2, .read_byte = 0x60 };
    Q3NMultiplaneRequest request = valid_request();
    Q3NMultiplaneResult result;
    uint8_t oob[Q3N_MULTIPLANE_OOB_SIZE];
    uint8_t main[Q3N_MULTIPLANE_MAIN_SIZE];

    memset(oob, 0, sizeof(oob));
    expect_u32("oob read return", q3n_multiplane_execute_read_oob(&request,
               &fake_ops, &fake, oob, &result), 0);
    expect_four_members(&fake, 1195);
    expect_complete(&result, 0x0b, 0x04);
    expect_u32("failed OOB stays erased", oob[2 * Q3N_MULTIPLANE_OOB_SLICE_SIZE],
               0xff);
    expect_u32("OOB plane slice", oob[3 * Q3N_MULTIPLANE_OOB_SLICE_SIZE], 0x63);

    memset(main, 0, sizeof(main));
    fake = (struct fake_media) { .fail_plane = 1, .read_byte = 0x70 };
    for (unsigned plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        memset(main + plane * Q3N_MULTIPLANE_MAIN_SLICE_SIZE,
               fake.read_byte + plane, Q3N_MULTIPLANE_MAIN_SLICE_SIZE);
    }
    expect_u32("program return", q3n_multiplane_execute_program(&request,
               &fake_ops, &fake, main, &result), 0);
    expect_four_members(&fake, 1195);
    expect_complete(&result, 0x0d, 0x02);

    fake = (struct fake_media) { .fail_plane = 0, .read_byte = 0x80 };
    for (unsigned plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        memset(oob + plane * Q3N_MULTIPLANE_OOB_SLICE_SIZE,
               fake.read_byte + plane, Q3N_MULTIPLANE_OOB_SLICE_SIZE);
    }
    expect_u32("oob program return", q3n_multiplane_execute_program_oob(&request,
               &fake_ops, &fake, oob, &result), 0);
    expect_four_members(&fake, 1195);
    expect_complete(&result, 0x0e, 0x01);

    fake = (struct fake_media) { .fail_plane = 3 };
    request.main_len = 0;
    request.oob_len = 0;
    expect_u32("erase return", q3n_multiplane_execute_erase(&request,
               &fake_ops, &fake, &result), 0);
    expect_four_members(&fake, 1195);
    expect_complete(&result, 0x07, 0x08);
}

static void test_die_zero_mapping_and_prevalidation_have_no_callbacks(void)
{
    struct fake_media fake = { .fail_plane = Q3N_MULTIPLANE_WIDTH };
    Q3NMultiplaneRequest request = valid_request();
    Q3NMultiplaneResult result;
    uint8_t data[Q3N_MULTIPLANE_MAIN_SIZE];

    request.die = 0;
    request.block = 0;
    expect_u32("die zero read", q3n_multiplane_execute_read(&request, &fake_ops,
               &fake, data, &result), 0);
    expect_four_members(&fake, 0);

    request = valid_request();
    request.die = Q3N_MULTIPLANE_DIES;
    request.main_len = 0;
    request.oob_len = 0;
    fake.calls = 0;
    expect_true("invalid die fails", q3n_multiplane_execute_erase(&request,
                &fake_ops, &fake, &result) < 0);
    expect_u32("invalid die calls", fake.calls, 0);
    expect_u32("invalid die done", result.done_mask, 0);
    expect_u32("invalid die fail", result.fail_mask, 0);

    request = valid_request();
    request.block = Q3N_MULTIPLANE_DATA_BLOCKS_PER_PLANE;
    fake.calls = 0;
    expect_true("invalid block fails", q3n_multiplane_execute_read(&request,
                &fake_ops, &fake, data, &result) < 0);
    expect_u32("invalid block calls", fake.calls, 0);
    expect_u32("invalid block done", result.done_mask, 0);

    request = valid_request();
    request.page = Q3N_MULTIPLANE_PAGES_PER_BLOCK;
    fake.calls = 0;
    expect_true("invalid page fails", q3n_multiplane_execute_program(&request,
                &fake_ops, &fake, data, &result) < 0);
    expect_u32("invalid page calls", fake.calls, 0);

    request = valid_request();
    request.main_len--;
    fake.calls = 0;
    expect_true("invalid main length fails",
                q3n_multiplane_execute_read(&request, &fake_ops, &fake, data,
                                             &result) < 0);
    expect_u32("invalid main length calls", fake.calls, 0);

    request = valid_request();
    request.oob_len--;
    fake.calls = 0;
    expect_true("invalid OOB length fails",
                q3n_multiplane_execute_read_oob(&request, &fake_ops, &fake,
                                                 data, &result) < 0);
    expect_u32("invalid OOB length calls", fake.calls, 0);
}

int main(void)
{
    test_main_read_maps_slices_and_ecc();
    test_raw_read_forwards_raw_and_clears_ecc();
    test_oob_and_mutating_operations_continue_after_failure();
    test_die_zero_mapping_and_prevalidation_have_no_callbacks();
    puts("ok: Q3N QEMU multi-plane engine verified");
    return 0;
}
