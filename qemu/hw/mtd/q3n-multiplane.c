#include "q3n-multiplane.h"

#include <errno.h>
#include <string.h>

static int q3n_multiplane_validate_address(const Q3NMultiplaneRequest *request)
{
    if (!request || request->die >= Q3N_MULTIPLANE_DIES ||
        request->block >= Q3N_MULTIPLANE_DATA_BLOCKS_PER_PLANE ||
        request->page >= Q3N_MULTIPLANE_PAGES_PER_BLOCK) {
        return -EINVAL;
    }
    return 0;
}

static int q3n_multiplane_prepare(const Q3NMultiplaneRequest *request,
                                  Q3NMultiplaneResult *result,
                                  bool main_transfer, bool oob_transfer,
                                  bool no_transfer)
{
    if (!result) {
        return -EINVAL;
    }
    memset(result, 0, sizeof(*result));
    if (q3n_multiplane_validate_address(request) ||
        (main_transfer && request->main_len != Q3N_MULTIPLANE_MAIN_SIZE) ||
        (oob_transfer && request->oob_len != Q3N_MULTIPLANE_OOB_SIZE) ||
        (no_transfer && (request->main_len || request->oob_len))) {
        return -EINVAL;
    }
    return 0;
}

static uint32_t q3n_multiplane_physical_block(
    const Q3NMultiplaneRequest *request, unsigned plane)
{
    return (request->die * Q3N_MULTIPLANE_WIDTH + plane) *
           Q3N_MULTIPLANE_BLOCKS_PER_PLANE + request->block;
}

static void q3n_multiplane_record(Q3NMultiplaneResult *result,
                                  unsigned plane, int callback_ret)
{
    if (callback_ret) {
        result->fail_mask |= 1U << plane;
    } else {
        result->done_mask |= 1U << plane;
    }
}

int q3n_multiplane_execute_read(const Q3NMultiplaneRequest *request,
                                 const Q3NMultiplaneOps *ops, void *opaque,
                                 uint8_t *data,
                                 Q3NMultiplaneResult *result)
{
    int ret;

    ret = q3n_multiplane_prepare(request, result, true, false, false);
    if (ret || !ops || !ops->read || !data) {
        return -EINVAL;
    }
    memset(data, 0xff, Q3N_MULTIPLANE_MAIN_SIZE);
    for (unsigned plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        Q3NMultiplaneEccResult ecc = { 0 };
        uint8_t *slice = data + plane * Q3N_MULTIPLANE_MAIN_SLICE_SIZE;

        ret = ops->read(opaque, q3n_multiplane_physical_block(request, plane),
                        request->page, slice, request->raw, &ecc);
        q3n_multiplane_record(result, plane, ret);
        if (ret) {
            memset(slice, 0xff, Q3N_MULTIPLANE_MAIN_SLICE_SIZE);
        } else if (!request->raw) {
            result->ecc[plane] = ecc;
        }
    }
    return 0;
}

int q3n_multiplane_execute_program(const Q3NMultiplaneRequest *request,
                                    const Q3NMultiplaneOps *ops, void *opaque,
                                    const uint8_t *data,
                                    Q3NMultiplaneResult *result)
{
    int ret;

    ret = q3n_multiplane_prepare(request, result, true, false, false);
    if (ret || !ops || !ops->program || !data) {
        return -EINVAL;
    }
    for (unsigned plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        ret = ops->program(opaque,
                           q3n_multiplane_physical_block(request, plane),
                           request->page,
                           data + plane * Q3N_MULTIPLANE_MAIN_SLICE_SIZE);
        q3n_multiplane_record(result, plane, ret);
    }
    return 0;
}

int q3n_multiplane_execute_read_oob(const Q3NMultiplaneRequest *request,
                                     const Q3NMultiplaneOps *ops,
                                     void *opaque, uint8_t *oob,
                                     Q3NMultiplaneResult *result)
{
    int ret;

    ret = q3n_multiplane_prepare(request, result, false, true, false);
    if (ret || !ops || !ops->read_oob || !oob) {
        return -EINVAL;
    }
    memset(oob, 0xff, Q3N_MULTIPLANE_OOB_SIZE);
    for (unsigned plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        ret = ops->read_oob(opaque,
                            q3n_multiplane_physical_block(request, plane),
                            request->page,
                            oob + plane * Q3N_MULTIPLANE_OOB_SLICE_SIZE);
        q3n_multiplane_record(result, plane, ret);
        if (ret) {
            memset(oob + plane * Q3N_MULTIPLANE_OOB_SLICE_SIZE, 0xff,
                   Q3N_MULTIPLANE_OOB_SLICE_SIZE);
        }
    }
    return 0;
}

int q3n_multiplane_execute_program_oob(const Q3NMultiplaneRequest *request,
                                        const Q3NMultiplaneOps *ops,
                                        void *opaque, const uint8_t *oob,
                                        Q3NMultiplaneResult *result)
{
    int ret;

    ret = q3n_multiplane_prepare(request, result, false, true, false);
    if (ret || !ops || !ops->program_oob || !oob) {
        return -EINVAL;
    }
    for (unsigned plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        ret = ops->program_oob(opaque,
                               q3n_multiplane_physical_block(request, plane),
                               request->page,
                               oob + plane * Q3N_MULTIPLANE_OOB_SLICE_SIZE);
        q3n_multiplane_record(result, plane, ret);
    }
    return 0;
}

int q3n_multiplane_execute_erase(const Q3NMultiplaneRequest *request,
                                  const Q3NMultiplaneOps *ops, void *opaque,
                                  Q3NMultiplaneResult *result)
{
    int ret;

    ret = q3n_multiplane_prepare(request, result, false, false, true);
    if (ret || !ops || !ops->erase) {
        return -EINVAL;
    }
    for (unsigned plane = 0; plane < Q3N_MULTIPLANE_WIDTH; plane++) {
        ret = ops->erase(opaque,
                         q3n_multiplane_physical_block(request, plane),
                         request->page);
        q3n_multiplane_record(result, plane, ret);
    }
    return 0;
}
