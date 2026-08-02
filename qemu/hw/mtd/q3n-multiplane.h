/* Standalone fixed four-plane executor for the Q3N NAND model. */

#ifndef Q3N_MULTIPLANE_H
#define Q3N_MULTIPLANE_H

#include <stdbool.h>
#include <stdint.h>

#define Q3N_MULTIPLANE_WIDTH 4U
#define Q3N_MULTIPLANE_DIES 2U
#define Q3N_MULTIPLANE_BLOCKS_PER_PLANE 247U
#define Q3N_MULTIPLANE_DATA_BLOCKS_PER_PLANE 208U
#define Q3N_MULTIPLANE_PAGES_PER_BLOCK 1600U
#define Q3N_MULTIPLANE_MAIN_SLICE_SIZE 16384U
#define Q3N_MULTIPLANE_OOB_SLICE_SIZE 1024U
#define Q3N_MULTIPLANE_MAIN_SIZE \
    (Q3N_MULTIPLANE_WIDTH * Q3N_MULTIPLANE_MAIN_SLICE_SIZE)
#define Q3N_MULTIPLANE_OOB_SIZE \
    (Q3N_MULTIPLANE_WIDTH * Q3N_MULTIPLANE_OOB_SLICE_SIZE)

typedef struct Q3NMultiplaneRequest {
    uint32_t die;
    uint32_t block;
    uint32_t page;
    uint32_t main_len;
    uint32_t oob_len;
    bool raw;
} Q3NMultiplaneRequest;

typedef struct Q3NMultiplaneEccResult {
    uint32_t status;
    uint32_t max_bitflips;
    uint32_t corrected_bits;
    uint32_t failed_step;
} Q3NMultiplaneEccResult;

typedef struct Q3NMultiplaneResult {
    uint8_t done_mask;
    uint8_t fail_mask;
    Q3NMultiplaneEccResult ecc[Q3N_MULTIPLANE_WIDTH];
} Q3NMultiplaneResult;

typedef int (*Q3NMultiplaneReadFn)(void *opaque, uint32_t physical_block,
                                    uint32_t page, uint8_t *destination,
                                    bool raw, Q3NMultiplaneEccResult *ecc);
typedef int (*Q3NMultiplaneProgramFn)(void *opaque, uint32_t physical_block,
                                       uint32_t page, const uint8_t *source);
typedef int (*Q3NMultiplaneReadOobFn)(void *opaque, uint32_t physical_block,
                                       uint32_t page, uint8_t *destination);
typedef int (*Q3NMultiplaneProgramOobFn)(void *opaque,
                                          uint32_t physical_block,
                                          uint32_t page,
                                          const uint8_t *source);
typedef int (*Q3NMultiplaneEraseFn)(void *opaque, uint32_t physical_block,
                                     uint32_t page);

typedef struct Q3NMultiplaneOps {
    Q3NMultiplaneReadFn read;
    Q3NMultiplaneProgramFn program;
    Q3NMultiplaneReadOobFn read_oob;
    Q3NMultiplaneProgramOobFn program_oob;
    Q3NMultiplaneEraseFn erase;
} Q3NMultiplaneOps;

int q3n_multiplane_execute_read(const Q3NMultiplaneRequest *request,
                                 const Q3NMultiplaneOps *ops, void *opaque,
                                 uint8_t *data,
                                 Q3NMultiplaneResult *result);
int q3n_multiplane_execute_program(const Q3NMultiplaneRequest *request,
                                    const Q3NMultiplaneOps *ops, void *opaque,
                                    const uint8_t *data,
                                    Q3NMultiplaneResult *result);
int q3n_multiplane_execute_read_oob(const Q3NMultiplaneRequest *request,
                                     const Q3NMultiplaneOps *ops,
                                     void *opaque, uint8_t *oob,
                                     Q3NMultiplaneResult *result);
int q3n_multiplane_execute_program_oob(const Q3NMultiplaneRequest *request,
                                        const Q3NMultiplaneOps *ops,
                                        void *opaque, const uint8_t *oob,
                                        Q3NMultiplaneResult *result);
int q3n_multiplane_execute_erase(const Q3NMultiplaneRequest *request,
                                  const Q3NMultiplaneOps *ops, void *opaque,
                                  Q3NMultiplaneResult *result);

#endif
