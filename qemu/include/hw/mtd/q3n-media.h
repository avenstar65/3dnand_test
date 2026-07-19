/* Persistent physical NAND media backing for the q3n controller. */

#ifndef HW_MTD_Q3N_MEDIA_H
#define HW_MTD_Q3N_MEDIA_H

#include "qapi/error.h"

typedef struct BlockBackend BlockBackend;
typedef struct Q3NMedia Q3NMedia;

#define Q3N_BBM_GOOD 0xff
#define Q3N_BBM_BAD  0x00

Q3NMedia *q3n_media_open(BlockBackend *blk, uint32_t block_count,
                         uint32_t pages_per_block, uint32_t page_size,
                         uint32_t oob_size, Error **errp);
void q3n_media_close(Q3NMedia *media);

int q3n_media_read_page(Q3NMedia *media, uint32_t block, uint32_t page,
                        uint8_t *data, uint8_t *physical_oob,
                        uint8_t *main_overlay, uint8_t *ldpc_overlay);
int q3n_media_program_page(Q3NMedia *media, uint32_t block, uint32_t page,
                           const uint8_t *data,
                           const uint8_t *physical_oob);
int q3n_media_erase_block(Q3NMedia *media, uint32_t block);
int q3n_media_inject_loss(Q3NMedia *media, uint32_t block, uint32_t page);
int q3n_media_inject_bitflips(Q3NMedia *media, uint32_t block, uint32_t page,
                              uint32_t step, uint32_t region,
                              uint32_t first_bit, uint32_t count);
uint32_t q3n_media_next_prog_page(const Q3NMedia *media, uint32_t block);
int q3n_media_get_block_status(Q3NMedia *media, uint32_t block,
                               uint32_t *status, uint32_t *next_page);
int q3n_media_mark_bad(Q3NMedia *media, uint32_t block);

#endif
