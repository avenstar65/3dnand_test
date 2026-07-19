/*
 * Versioned sparse image for physical q3n NAND media.
 *
 * This layer knows only physical blocks and pages.  RAID layout and parity
 * ownership remain Linux driver policy.
 */

#include "qemu/osdep.h"
#include "hw/mtd/q3n-media.h"
#include "hw/mtd/q3n-nand.h"
#include "q3n-media-overlay.h"
#include "block/block_int-common.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"

#define Q3N_MEDIA_MAGIC             "Q3NMEDIA"
#define Q3N_MEDIA_VERSION           2
#define Q3N_MEDIA_HEADER_SIZE       4096
#define Q3N_MEDIA_PAGE_ERASED       0
#define Q3N_MEDIA_PAGE_PRESENT      1
#define Q3N_MEDIA_PAGE_LOST         2

typedef struct Q3NMediaHeader {
    uint8_t magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t block_count;
    uint32_t pages_per_block;
    uint32_t page_size;
    uint32_t oob_size;
    uint64_t block_state_offset;
    uint64_t block_state_length;
    uint64_t page_state_offset;
    uint64_t page_state_length;
    uint64_t page_slots_offset;
    uint64_t page_stride;
    uint64_t image_size;
    uint32_t physical_oob_size;
    uint32_t logical_oob_size;
    uint32_t ldpc_bytes_per_step;
    uint32_t ldpc_steps;
    uint64_t overlay_slots_offset;
    uint64_t overlay_stride;
    uint64_t overlay_slots_length;
    uint8_t reserved[3968];
} Q3NMediaHeader;

QEMU_BUILD_BUG_ON(sizeof(Q3NMediaHeader) != Q3N_MEDIA_HEADER_SIZE);
QEMU_BUILD_BUG_ON(Q3N_METADATA_OOB_OFFSET + Q3N_LOGICAL_OOB_SIZE - 1 !=
                  Q3N_PHYSICAL_OOB_SIZE);

struct Q3NMedia {
    BlockBackend *blk;
    uint32_t block_count;
    uint32_t pages_per_block;
    uint32_t page_size;
    uint32_t physical_oob_size;
    uint64_t page_count;
    uint64_t block_state_offset;
    uint64_t block_state_length;
    uint64_t page_state_offset;
    uint64_t page_state_length;
    uint64_t page_slots_offset;
    uint64_t page_stride;
    uint64_t overlay_slots_offset;
    uint64_t overlay_stride;
    uint64_t overlay_slots_length;
    uint64_t image_size;
    uint32_t *next_prog_page;
    uint8_t *page_state;
    bool *bad;
};

static uint64_t q3n_media_page_index(const Q3NMedia *m, uint32_t block,
                                     uint32_t page)
{
    return (uint64_t)block * m->pages_per_block + page;
}

static uint64_t q3n_media_slot_offset(const Q3NMedia *m, uint32_t block,
                                      uint32_t page)
{
    return m->page_slots_offset +
           q3n_media_page_index(m, block, page) * m->page_stride;
}

static uint64_t q3n_media_overlay_slot_offset(const Q3NMedia *m,
                                              uint32_t block, uint32_t page)
{
    return m->overlay_slots_offset +
           q3n_media_page_index(m, block, page) * m->overlay_stride;
}

static int q3n_media_write_header(Q3NMedia *m)
{
    Q3NMediaHeader h = { 0 };

    memcpy(h.magic, Q3N_MEDIA_MAGIC, sizeof(h.magic));
    h.version = cpu_to_le32(Q3N_MEDIA_VERSION);
    h.header_size = cpu_to_le32(Q3N_MEDIA_HEADER_SIZE);
    h.block_count = cpu_to_le32(m->block_count);
    h.pages_per_block = cpu_to_le32(m->pages_per_block);
    h.page_size = cpu_to_le32(m->page_size);
    h.oob_size = cpu_to_le32(m->physical_oob_size);
    h.block_state_offset = cpu_to_le64(m->block_state_offset);
    h.block_state_length = cpu_to_le64(m->block_state_length);
    h.page_state_offset = cpu_to_le64(m->page_state_offset);
    h.page_state_length = cpu_to_le64(m->page_state_length);
    h.page_slots_offset = cpu_to_le64(m->page_slots_offset);
    h.page_stride = cpu_to_le64(m->page_stride);
    h.image_size = cpu_to_le64(m->image_size);
    h.physical_oob_size = cpu_to_le32(m->physical_oob_size);
    h.logical_oob_size = cpu_to_le32(Q3N_LOGICAL_OOB_SIZE);
    h.ldpc_bytes_per_step = cpu_to_le32(Q3N_LDPC_BYTES_PER_STEP);
    h.ldpc_steps = cpu_to_le32(Q3N_LDPC_STEPS);
    h.overlay_slots_offset = cpu_to_le64(m->overlay_slots_offset);
    h.overlay_stride = cpu_to_le64(m->overlay_stride);
    h.overlay_slots_length = cpu_to_le64(m->overlay_slots_length);

    return blk_pwrite(m->blk, 0, sizeof(h), &h, 0);
}

static bool q3n_media_align_up(uint64_t value, uint64_t alignment,
                               uint64_t *result)
{
    uint64_t adjusted;

    if (uadd64_overflow(value, alignment - 1, &adjusted)) {
        return false;
    }
    *result = QEMU_ALIGN_DOWN(adjusted, alignment);
    return true;
}

static bool q3n_media_calculate_layout(Q3NMedia *m)
{
    uint64_t page_slots_end;

    if (umul64_overflow(m->block_count, m->pages_per_block,
                        &m->page_count)) {
        return false;
    }
    m->block_state_offset = Q3N_MEDIA_HEADER_SIZE;
    if (umul64_overflow(m->block_count, sizeof(uint32_t),
                        &m->block_state_length) ||
        uadd64_overflow(m->block_state_offset, m->block_state_length,
                        &m->page_state_offset)) {
        return false;
    }
    m->page_state_length = m->page_count;
    if (uadd64_overflow(m->page_state_offset, m->page_state_length,
                        &page_slots_end) ||
        !q3n_media_align_up(page_slots_end, Q3N_MEDIA_HEADER_SIZE,
                            &m->page_slots_offset) ||
        uadd64_overflow(m->page_size, Q3N_PHYSICAL_OOB_SIZE,
                        &m->page_stride) ||
        umul64_overflow(m->page_count, m->page_stride, &page_slots_end) ||
        uadd64_overflow(m->page_slots_offset, page_slots_end,
                        &page_slots_end) ||
        !q3n_media_align_up(page_slots_end, Q3N_MEDIA_HEADER_SIZE,
                            &m->overlay_slots_offset) ||
        uadd64_overflow(Q3N_MEDIA_MAIN_OVERLAY_BYTES,
                        Q3N_MEDIA_LDPC_OVERLAY_BYTES,
                        &m->overlay_stride) ||
        umul64_overflow(m->page_count, m->overlay_stride,
                        &m->overlay_slots_length) ||
        uadd64_overflow(m->overlay_slots_offset, m->overlay_slots_length,
                        &m->image_size) ||
        m->image_size > INT64_MAX) {
        return false;
    }
    return true;
}

static int q3n_media_create(Q3NMedia *m, Error **errp)
{
    uint8_t marker = Q3N_BBM_GOOD;
    uint32_t block;
    int ret;

    ret = blk_truncate(m->blk, m->image_size, true, PREALLOC_MODE_OFF, 0,
                       errp);
    if (ret < 0) {
        return ret;
    }
    ret = q3n_media_write_header(m);
    if (ret < 0) {
        return ret;
    }
    ret = blk_pwrite(m->blk, m->block_state_offset,
                     (int64_t)m->block_count * sizeof(uint32_t),
                     m->next_prog_page, 0);
    if (ret < 0) {
        return ret;
    }
    ret = blk_pwrite(m->blk, m->page_state_offset, m->page_count,
                     m->page_state, 0);
    if (ret < 0) {
        return ret;
    }

    /* OOB[0] of the first physical page is the persistent bad-block marker. */
    for (block = 0; block < m->block_count; block++) {
        ret = blk_pwrite(m->blk,
                         q3n_media_slot_offset(m, block, 0) + m->page_size +
                         Q3N_BBM_OOB_OFFSET,
                         1, &marker, 0);
        if (ret < 0) {
            return ret;
        }
    }
    return blk_flush(m->blk);
}

static bool q3n_media_header_matches(Q3NMedia *m, const Q3NMediaHeader *h)
{
    return !memcmp(h->magic, Q3N_MEDIA_MAGIC, sizeof(h->magic)) &&
           le32_to_cpu(h->version) == Q3N_MEDIA_VERSION &&
           le32_to_cpu(h->header_size) == Q3N_MEDIA_HEADER_SIZE &&
           le32_to_cpu(h->block_count) == m->block_count &&
           le32_to_cpu(h->pages_per_block) == m->pages_per_block &&
           le32_to_cpu(h->page_size) == m->page_size &&
           le32_to_cpu(h->oob_size) == m->physical_oob_size &&
           le64_to_cpu(h->block_state_offset) == m->block_state_offset &&
           le64_to_cpu(h->block_state_length) == m->block_state_length &&
           le64_to_cpu(h->page_state_offset) == m->page_state_offset &&
           le64_to_cpu(h->page_state_length) == m->page_state_length &&
           le64_to_cpu(h->page_slots_offset) == m->page_slots_offset &&
           le64_to_cpu(h->page_stride) == m->page_stride &&
           le32_to_cpu(h->physical_oob_size) == m->physical_oob_size &&
           le32_to_cpu(h->logical_oob_size) == Q3N_LOGICAL_OOB_SIZE &&
           le32_to_cpu(h->ldpc_bytes_per_step) == Q3N_LDPC_BYTES_PER_STEP &&
           le32_to_cpu(h->ldpc_steps) == Q3N_LDPC_STEPS &&
           le64_to_cpu(h->overlay_slots_offset) == m->overlay_slots_offset &&
           le64_to_cpu(h->overlay_stride) == m->overlay_stride &&
           le64_to_cpu(h->overlay_slots_length) == m->overlay_slots_length &&
           le64_to_cpu(h->image_size) == m->image_size;
}

static int q3n_media_load(Q3NMedia *m, int64_t length, Error **errp)
{
    Q3NMediaHeader h;
    uint8_t marker;
    uint32_t block;
    uint32_t page;
    int ret;

    ret = blk_pread(m->blk, 0, sizeof(h), &h, 0);
    if (ret < 0) {
        return ret;
    }
    if (!memcmp(h.magic, Q3N_MEDIA_MAGIC, sizeof(h.magic)) &&
        le32_to_cpu(h.version) == 1) {
        error_setg(errp,
                   "q3n NAND image version 1 is incompatible with version 2; recreate it");
        return -EINVAL;
    }
    if (!q3n_media_header_matches(m, &h) || length != m->image_size) {
        error_setg(errp, "q3n NAND image has incompatible header or size");
        return -EINVAL;
    }
    ret = blk_pread(m->blk, m->block_state_offset,
                    (int64_t)m->block_count * sizeof(uint32_t),
                    m->next_prog_page, 0);
    if (ret < 0) {
        return ret;
    }
    ret = blk_pread(m->blk, m->page_state_offset, m->page_count,
                    m->page_state, 0);
    if (ret < 0) {
        return ret;
    }
    for (block = 0; block < m->block_count; block++) {
        m->next_prog_page[block] = le32_to_cpu(m->next_prog_page[block]);
        if (m->next_prog_page[block] > m->pages_per_block) {
            error_setg(errp, "q3n NAND image has invalid program frontier");
            return -EINVAL;
        }
        for (page = 0; page < m->pages_per_block; page++) {
            uint8_t state = m->page_state[
                q3n_media_page_index(m, block, page)];

            if (state > Q3N_MEDIA_PAGE_LOST) {
                error_setg(errp, "q3n NAND image has invalid page state");
                return -EINVAL;
            }
            if ((page < m->next_prog_page[block] &&
                 state == Q3N_MEDIA_PAGE_ERASED) ||
                (page >= m->next_prog_page[block] &&
                 state != Q3N_MEDIA_PAGE_ERASED)) {
                error_setg(errp,
                           "q3n NAND image has inconsistent page state/frontier");
                return -EINVAL;
            }
        }
        ret = blk_pread(m->blk,
                        q3n_media_slot_offset(m, block, 0) + m->page_size +
                        Q3N_BBM_OOB_OFFSET,
                        1, &marker, 0);
        if (ret < 0) {
            return ret;
        }
        m->bad[block] = marker != Q3N_BBM_GOOD;
    }
    return 0;
}

Q3NMedia *q3n_media_open(BlockBackend *blk, uint32_t block_count,
                         uint32_t pages_per_block, uint32_t page_size,
                         uint32_t oob_size, Error **errp)
{
    Q3NMedia *m;
    int64_t length;
    uint64_t perm;
    int ret;

    if (!blk) {
        error_setg(errp, "q3n-nand requires a writable drive");
        return NULL;
    }
    perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE | BLK_PERM_RESIZE;
    ret = blk_set_perm(blk, perm, BLK_PERM_ALL, errp);
    if (ret < 0) {
        return NULL;
    }
    blk_set_allow_write_beyond_eof(blk, true);

    m = g_new0(Q3NMedia, 1);
    m->blk = blk;
    m->block_count = block_count;
    m->pages_per_block = pages_per_block;
    m->page_size = page_size;
    m->physical_oob_size = oob_size;
    if (page_size != Q3N_PAGE_SIZE || oob_size != Q3N_PHYSICAL_OOB_SIZE) {
        error_setg(errp,
                   "q3n NAND media requires %u-byte pages and %u-byte physical OOB",
                   Q3N_PAGE_SIZE, Q3N_PHYSICAL_OOB_SIZE);
        goto fail;
    }
    if (!q3n_media_calculate_layout(m)) {
        error_setg(errp, "q3n NAND image layout exceeds INT64_MAX");
        goto fail;
    }
    m->next_prog_page = g_new0(uint32_t, block_count);
    m->page_state = g_new0(uint8_t, m->page_count);
    m->bad = g_new0(bool, block_count);

    length = blk_getlength(blk);
    if (length < 0) {
        error_setg_errno(errp, -length, "cannot get q3n NAND image size");
        goto fail;
    }
    ret = length == 0 ? q3n_media_create(m, errp) :
                        q3n_media_load(m, length, errp);
    if (ret < 0) {
        if (!*errp) {
            error_setg_errno(errp, -ret, "cannot open q3n NAND image");
        }
        goto fail;
    }
    return m;

fail:
    q3n_media_close(m);
    return NULL;
}

void q3n_media_close(Q3NMedia *m)
{
    int ret;

    if (!m) {
        return;
    }
    if (m->blk) {
        ret = blk_flush(m->blk);
        if (ret < 0) {
            error_report("cannot flush q3n NAND image: %s",
                         strerror(-ret));
        }
    }
    g_free(m->bad);
    g_free(m->page_state);
    g_free(m->next_prog_page);
    g_free(m);
}

int q3n_media_read_page(Q3NMedia *m, uint32_t block, uint32_t page,
                        uint8_t *data, uint8_t *physical_oob,
                        uint8_t *main_overlay, uint8_t *ldpc_overlay)
{
    uint64_t index;
    uint64_t overlay_slot;
    uint64_t slot;
    int ret;

    if (block >= m->block_count || page >= m->pages_per_block) {
        return -EINVAL;
    }
    index = q3n_media_page_index(m, block, page);
    if (m->page_state[index] == Q3N_MEDIA_PAGE_LOST) {
        return -EIO;
    }
    slot = q3n_media_slot_offset(m, block, page);
    if (m->page_state[index] == Q3N_MEDIA_PAGE_PRESENT) {
        ret = blk_pread(m->blk, slot, m->page_size, data, 0);
        if (ret < 0) {
            return ret;
        }
        if (physical_oob) {
            ret = blk_pread(m->blk, slot + m->page_size,
                            m->physical_oob_size, physical_oob, 0);
            if (ret < 0) {
                return ret;
            }
        }
    } else {
        memset(data, 0xff, m->page_size);
        if (physical_oob) {
            memset(physical_oob, 0xff, m->physical_oob_size);
            if (page == 0) {
                ret = blk_pread(m->blk,
                                slot + m->page_size + Q3N_BBM_OOB_OFFSET,
                                1, physical_oob + Q3N_BBM_OOB_OFFSET, 0);
                if (ret < 0) {
                    return ret;
                }
            }
        }
    }

    overlay_slot = q3n_media_overlay_slot_offset(m, block, page);
    if (main_overlay) {
        ret = blk_pread(m->blk, overlay_slot,
                        Q3N_MEDIA_MAIN_OVERLAY_BYTES,
                        main_overlay, 0);
        if (ret < 0) {
            return ret;
        }
    }
    if (ldpc_overlay) {
        ret = blk_pread(m->blk,
                        overlay_slot + Q3N_MEDIA_MAIN_OVERLAY_BYTES,
                        Q3N_MEDIA_LDPC_OVERLAY_BYTES, ldpc_overlay, 0);
        if (ret < 0) {
            return ret;
        }
    }
    return 0;
}

int q3n_media_program_page(Q3NMedia *m, uint32_t block, uint32_t page,
                           const uint8_t *data,
                           const uint8_t *physical_oob)
{
    uint64_t index;
    uint64_t slot;
    uint8_t *storage;
    uint8_t present = Q3N_MEDIA_PAGE_PRESENT;
    uint32_t next;
    uint32_t next_le;
    int ret;

    if (block >= m->block_count || page >= m->pages_per_block) {
        return -EINVAL;
    }
    if (m->bad[block]) {
        return -EIO;
    }
    index = q3n_media_page_index(m, block, page);
    if (page != m->next_prog_page[block]) {
        return -ERANGE;
    }
    if (m->page_state[index] != Q3N_MEDIA_PAGE_ERASED) {
        return -EEXIST;
    }

    storage = g_malloc(m->page_stride);
    memcpy(storage, data, m->page_size);
    if (physical_oob) {
        memcpy(storage + m->page_size, physical_oob,
               m->physical_oob_size);
    } else {
        memset(storage + m->page_size, 0xff, m->physical_oob_size);
    }
    slot = q3n_media_slot_offset(m, block, page);
    ret = blk_pwrite(m->blk, slot, m->page_stride, storage, 0);
    g_free(storage);
    if (ret < 0) {
        return ret;
    }
    ret = blk_pwrite(m->blk, m->page_state_offset + index, 1,
                     &present, 0);
    if (ret < 0) {
        return ret;
    }
    next = m->next_prog_page[block] + 1;
    next_le = cpu_to_le32(next);
    ret = blk_pwrite(m->blk,
                     m->block_state_offset + block * sizeof(next_le),
                     sizeof(next_le), &next_le, 0);
    if (ret < 0) {
        return ret;
    }
    m->page_state[index] = present;
    m->next_prog_page[block] = next;
    if (page == 0) {
        m->bad[block] = physical_oob && physical_oob[Q3N_BBM_OOB_OFFSET] !=
                        Q3N_BBM_GOOD;
    }
    return 0;
}

int q3n_media_erase_block(Q3NMedia *m, uint32_t block)
{
    uint64_t first;
    uint64_t overlay_block_length;
    uint8_t *clear_overlay;
    uint8_t *erased;
    uint32_t next_le = 0;
    int ret;

    if (block >= m->block_count) {
        return -EINVAL;
    }
    if (m->bad[block]) {
        return -EIO;
    }
    first = q3n_media_page_index(m, block, 0);
    erased = g_new0(uint8_t, m->pages_per_block);
    ret = blk_pwrite(m->blk, m->page_state_offset + first,
                     m->pages_per_block, erased, 0);
    g_free(erased);
    if (ret < 0) {
        return ret;
    }
    ret = blk_pwrite(m->blk,
                     m->block_state_offset + block * sizeof(next_le),
                     sizeof(next_le), &next_le, 0);
    if (ret < 0) {
        return ret;
    }
    if (umul64_overflow(m->pages_per_block, m->overlay_stride,
                        &overlay_block_length)) {
        return -EOVERFLOW;
    }
    clear_overlay = g_malloc0(overlay_block_length);
    ret = blk_pwrite(m->blk,
                     q3n_media_overlay_slot_offset(m, block, 0),
                     overlay_block_length, clear_overlay, 0);
    g_free(clear_overlay);
    if (ret < 0) {
        return ret;
    }
    memset(&m->page_state[first], Q3N_MEDIA_PAGE_ERASED,
           m->pages_per_block);
    m->next_prog_page[block] = 0;
    return 0;
}

int q3n_media_inject_loss(Q3NMedia *m, uint32_t block, uint32_t page)
{
    uint64_t index;
    uint8_t lost = Q3N_MEDIA_PAGE_LOST;
    int ret;

    if (block >= m->block_count || page >= m->pages_per_block) {
        return -EINVAL;
    }
    index = q3n_media_page_index(m, block, page);
    if (m->page_state[index] != Q3N_MEDIA_PAGE_PRESENT) {
        return -ENOENT;
    }
    ret = blk_pwrite(m->blk, m->page_state_offset + index, 1, &lost, 0);
    if (ret < 0) {
        return ret;
    }
    m->page_state[index] = lost;
    return 0;
}

int q3n_media_inject_bitflips(Q3NMedia *m, uint32_t block, uint32_t page,
                              uint32_t step, uint32_t region,
                              uint32_t first_bit, uint32_t count)
{
    uint64_t overlay_slot;
    uint8_t *overlay;
    int ret;

    if (block >= m->block_count || page >= m->pages_per_block ||
        !q3n_media_overlay_range(step, region, first_bit, count, NULL)) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }

    overlay = g_malloc(m->overlay_stride);
    overlay_slot = q3n_media_overlay_slot_offset(m, block, page);
    ret = blk_pread(m->blk, overlay_slot, m->overlay_stride, overlay, 0);
    if (ret < 0) {
        g_free(overlay);
        return ret;
    }
    if (!q3n_media_overlay_xor(overlay, step, region, first_bit, count)) {
        g_free(overlay);
        return -EINVAL;
    }
    ret = blk_pwrite(m->blk, overlay_slot, m->overlay_stride, overlay, 0);
    g_free(overlay);
    if (ret < 0) {
        return ret;
    }
    return blk_flush(m->blk);
}

uint32_t q3n_media_next_prog_page(const Q3NMedia *m, uint32_t block)
{
    return block < m->block_count ? m->next_prog_page[block] : UINT32_MAX;
}

int q3n_media_get_block_status(Q3NMedia *m, uint32_t block,
                               uint32_t *status, uint32_t *next_page)
{
    uint8_t marker;
    int ret;

    if (block >= m->block_count || !status || !next_page) {
        return -EINVAL;
    }
    ret = blk_pread(m->blk,
                    q3n_media_slot_offset(m, block, 0) + m->page_size +
                    Q3N_BBM_OOB_OFFSET,
                    1, &marker, 0);
    if (ret < 0) {
        return ret;
    }
    m->bad[block] = marker != Q3N_BBM_GOOD;
    *status = m->bad[block] ? Q3N_BLOCK_STATUS_BAD : 0;
    if (m->next_prog_page[block] == 0) {
        *status |= Q3N_BLOCK_STATUS_ERASED;
    }
    *next_page = m->next_prog_page[block];
    return 0;
}

int q3n_media_mark_bad(Q3NMedia *m, uint32_t block)
{
    uint64_t marker_offset;
    uint8_t marker;
    int ret;

    if (block >= m->block_count) {
        return -EINVAL;
    }
    marker_offset = q3n_media_slot_offset(m, block, 0) + m->page_size +
                    Q3N_BBM_OOB_OFFSET;
    ret = blk_pread(m->blk, marker_offset, 1, &marker, 0);
    if (ret < 0) {
        return ret;
    }
    if (marker != Q3N_BBM_GOOD) {
        m->bad[block] = true;
        return 0;
    }
    marker = Q3N_BBM_BAD;
    ret = blk_pwrite(m->blk, marker_offset, 1, &marker, 0);
    if (ret < 0) {
        return ret;
    }
    m->bad[block] = true;
    return 0;
}
