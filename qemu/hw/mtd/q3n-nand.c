/*
 * QEMU 3D NAND model for scheme D experiments.
 *
 * This is a functional base model, not a timing-accurate NVDDR3 model. It
 * implements sparse NAND media, 16KiB page read/program, 25MiB data-block
 * erase, data-block generation, and append-only parity records for 8-lane
 * XOR recovery.
 */

#include "qemu/osdep.h"
#include "hw/mtd/q3n-nand.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/module.h"

#define Q3N_ID_VALUE                  0x314e3351U /* "Q3N1" */
#define Q3N_CAP_SCHEME_D              (1U << 0)
#define Q3N_RAID_PROFILE_SCHEME_D     4
#define Q3N_KEY_POOL_DATA             0
#define Q3N_KEY_POOL_PARITY           1

typedef struct Q3NPage {
    uint8_t data[Q3N_PAGE_SIZE];
} Q3NPage;

typedef struct Q3NPhysAddr {
    uint8_t pool;
    uint8_t lane;
    uint16_t block;
    uint16_t page;
} Q3NPhysAddr;

typedef struct Q3NDataBlockMeta {
    uint32_t generation;
    bool bad;
    bool erased;
} Q3NDataBlockMeta;

typedef struct Q3NParityIndexEntry {
    Q3NPhysAddr parity_addr;
    uint32_t parity_version;
    uint32_t data_generation[Q3N_LANES];
    uint64_t sequence;
    bool valid;
} Q3NParityIndexEntry;

typedef struct Q3NStats {
    uint64_t data_programs;
    uint64_t data_erases;
    uint64_t generation_updates;
    uint64_t parity_appends;
    uint64_t parity_stale;
    uint64_t raid_recovered;
    uint64_t raid_failed;
} Q3NStats;

struct Q3NNandState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t data_blocks_per_plane;
    uint32_t parity_blocks_per_plane;
    uint32_t metadata_blocks_per_plane;
    uint32_t reserve_blocks_per_plane;

    uint32_t status;
    uint32_t irq_status;
    uint32_t irq_mask;
    uint32_t cmd;
    uint32_t len;
    uint64_t addr;

    uint8_t data_buf[Q3N_PAGE_SIZE];
    uint32_t data_pos;
    uint32_t data_count;

    uint32_t data_block_count;
    uint32_t parity_block_count;
    uint64_t visible_size;
    uint64_t erase_size;

    Q3NDataBlockMeta *data_meta;
    uint8_t *data_page_valid;

    GHashTable *pages;        /* uint64 key -> Q3NPage */
    GHashTable *parity_index; /* uint64 stripe key -> Q3NParityIndexEntry */

    uint64_t parity_next_page;
    uint64_t parity_sequence;

    Q3NStats stats;
};

static uint64_t q3n_make_page_key(Q3NPhysAddr a)
{
    return ((uint64_t)a.pool << 56) |
           ((uint64_t)a.lane << 48) |
           ((uint64_t)a.block << 24) |
           (uint64_t)a.page;
}

static uint64_t q3n_make_stripe_key(uint64_t group_id, uint32_t page)
{
    return group_id * Q3N_PAGES_PER_BLOCK + page;
}

static uint64_t *q3n_u64_key_new(uint64_t value)
{
    uint64_t *key = g_new(uint64_t, 1);

    *key = value;
    return key;
}

static Q3NPage *q3n_lookup_page(Q3NNandState *s, Q3NPhysAddr addr)
{
    uint64_t key = q3n_make_page_key(addr);

    return g_hash_table_lookup(s->pages, &key);
}

static Q3NPage *q3n_get_or_create_page(Q3NNandState *s, Q3NPhysAddr addr)
{
    uint64_t raw_key = q3n_make_page_key(addr);
    Q3NPage *page = g_hash_table_lookup(s->pages, &raw_key);

    if (page) {
        return page;
    }

    page = g_new0(Q3NPage, 1);
    memset(page->data, 0xff, sizeof(page->data));
    g_hash_table_insert(s->pages, q3n_u64_key_new(raw_key), page);
    return page;
}

static uint32_t q3n_data_page_index(Q3NNandState *s, uint32_t block, uint32_t page)
{
    return block * Q3N_PAGES_PER_BLOCK + page;
}

static bool q3n_data_page_is_valid(Q3NNandState *s, uint32_t block, uint32_t page)
{
    return s->data_page_valid[q3n_data_page_index(s, block, page)] != 0;
}

static void q3n_set_data_page_valid(Q3NNandState *s, uint32_t block,
                                    uint32_t page, bool valid)
{
    s->data_page_valid[q3n_data_page_index(s, block, page)] = valid ? 1 : 0;
}

static bool q3n_decode_data_addr(Q3NNandState *s, uint64_t byte_addr,
                                 uint32_t *block, uint32_t *page,
                                 uint32_t *column)
{
    uint64_t logical_page = byte_addr / Q3N_PAGE_SIZE;

    *column = byte_addr % Q3N_PAGE_SIZE;
    *block = logical_page / Q3N_PAGES_PER_BLOCK;
    *page = logical_page % Q3N_PAGES_PER_BLOCK;

    return *block < s->data_block_count;
}

static void q3n_data_phys_addr(uint32_t data_block, uint32_t page,
                               Q3NPhysAddr *addr)
{
    addr->pool = Q3N_KEY_POOL_DATA;
    addr->lane = data_block % Q3N_LANES;
    addr->block = data_block / Q3N_LANES;
    addr->page = page;
}

static void q3n_read_data_page(Q3NNandState *s, uint32_t data_block,
                               uint32_t page, uint8_t *buf)
{
    Q3NPhysAddr addr;
    Q3NPage *stored;

    if (!q3n_data_page_is_valid(s, data_block, page)) {
        memset(buf, 0xff, Q3N_PAGE_SIZE);
        return;
    }

    q3n_data_phys_addr(data_block, page, &addr);
    stored = q3n_lookup_page(s, addr);
    if (!stored) {
        memset(buf, 0xff, Q3N_PAGE_SIZE);
        return;
    }

    memcpy(buf, stored->data, Q3N_PAGE_SIZE);
}

static bool q3n_program_data_page(Q3NNandState *s, uint32_t data_block,
                                  uint32_t page, const uint8_t *buf)
{
    Q3NPhysAddr addr;
    Q3NPage *stored;

    if (data_block >= s->data_block_count || page >= Q3N_PAGES_PER_BLOCK) {
        return false;
    }
    if (s->data_meta[data_block].bad ||
        q3n_data_page_is_valid(s, data_block, page)) {
        return false;
    }

    q3n_data_phys_addr(data_block, page, &addr);
    stored = q3n_get_or_create_page(s, addr);
    memcpy(stored->data, buf, Q3N_PAGE_SIZE);
    q3n_set_data_page_valid(s, data_block, page, true);
    s->data_meta[data_block].erased = false;
    s->stats.data_programs++;
    return true;
}

static void q3n_parity_phys_addr(Q3NNandState *s, uint64_t parity_page,
                                 Q3NPhysAddr *addr)
{
    uint64_t block_global = parity_page / Q3N_PAGES_PER_BLOCK;

    addr->pool = Q3N_KEY_POOL_PARITY;
    addr->lane = block_global % Q3N_LANES;
    addr->block = s->data_blocks_per_plane + (block_global / Q3N_LANES);
    addr->page = parity_page % Q3N_PAGES_PER_BLOCK;
}

static bool q3n_all_stripe_pages_valid(Q3NNandState *s, uint64_t group_id,
                                       uint32_t page)
{
    uint32_t base_block = group_id * Q3N_LANES;
    int lane;

    if (base_block + Q3N_LANES > s->data_block_count) {
        return false;
    }

    for (lane = 0; lane < Q3N_LANES; lane++) {
        if (!q3n_data_page_is_valid(s, base_block + lane, page)) {
            return false;
        }
    }

    return true;
}

static bool q3n_append_parity_record(Q3NNandState *s, uint64_t group_id,
                                     uint32_t page)
{
    uint8_t parity[Q3N_PAGE_SIZE];
    uint8_t tmp[Q3N_PAGE_SIZE];
    uint32_t base_block = group_id * Q3N_LANES;
    uint64_t parity_capacity = (uint64_t)s->parity_block_count *
                               Q3N_PAGES_PER_BLOCK;
    Q3NPhysAddr parity_addr;
    Q3NPage *parity_page;
    Q3NParityIndexEntry *entry;
    uint64_t stripe_key;
    int lane;
    int i;

    if (s->parity_next_page >= parity_capacity) {
        return false;
    }

    memset(parity, 0, sizeof(parity));
    for (lane = 0; lane < Q3N_LANES; lane++) {
        q3n_read_data_page(s, base_block + lane, page, tmp);
        for (i = 0; i < Q3N_PAGE_SIZE; i++) {
            parity[i] ^= tmp[i];
        }
    }

    q3n_parity_phys_addr(s, s->parity_next_page, &parity_addr);
    parity_page = q3n_get_or_create_page(s, parity_addr);
    memcpy(parity_page->data, parity, Q3N_PAGE_SIZE);

    stripe_key = q3n_make_stripe_key(group_id, page);
    entry = g_hash_table_lookup(s->parity_index, &stripe_key);
    if (!entry) {
        entry = g_new0(Q3NParityIndexEntry, 1);
        g_hash_table_insert(s->parity_index, q3n_u64_key_new(stripe_key), entry);
    }

    entry->parity_addr = parity_addr;
    entry->parity_version++;
    entry->sequence = ++s->parity_sequence;
    entry->valid = true;
    for (lane = 0; lane < Q3N_LANES; lane++) {
        entry->data_generation[lane] = s->data_meta[base_block + lane].generation;
    }

    s->parity_next_page++;
    s->stats.parity_appends++;
    return true;
}

static bool q3n_parity_entry_matches(Q3NNandState *s,
                                     Q3NParityIndexEntry *entry,
                                     uint64_t group_id)
{
    uint32_t base_block = group_id * Q3N_LANES;
    int lane;

    if (!entry || !entry->valid) {
        return false;
    }

    for (lane = 0; lane < Q3N_LANES; lane++) {
        if (entry->data_generation[lane] !=
            s->data_meta[base_block + lane].generation) {
            return false;
        }
    }

    return true;
}

static void q3n_try_update_parity(Q3NNandState *s, uint32_t data_block,
                                  uint32_t page)
{
    uint64_t group_id = data_block / Q3N_LANES;

    if (!q3n_all_stripe_pages_valid(s, group_id, page)) {
        s->status |= Q3N_STATUS_PARITY_STALE;
        return;
    }

    if (q3n_append_parity_record(s, group_id, page)) {
        s->status |= Q3N_STATUS_PARITY_VALID;
        s->status &= ~Q3N_STATUS_PARITY_STALE;
    } else {
        s->status |= Q3N_STATUS_PARITY_STALE;
    }
}

static bool q3n_recover_data_page(Q3NNandState *s, uint32_t data_block,
                                  uint32_t page, uint8_t *buf)
{
    uint64_t group_id = data_block / Q3N_LANES;
    uint32_t base_block = group_id * Q3N_LANES;
    uint32_t missing_lane = data_block % Q3N_LANES;
    uint64_t stripe_key = q3n_make_stripe_key(group_id, page);
    Q3NParityIndexEntry *entry = g_hash_table_lookup(s->parity_index, &stripe_key);
    Q3NPage *parity_page;
    uint8_t tmp[Q3N_PAGE_SIZE];
    int lane;
    int i;

    if (!q3n_parity_entry_matches(s, entry, group_id)) {
        s->stats.parity_stale++;
        return false;
    }

    parity_page = q3n_lookup_page(s, entry->parity_addr);
    if (!parity_page) {
        return false;
    }

    memcpy(buf, parity_page->data, Q3N_PAGE_SIZE);
    for (lane = 0; lane < Q3N_LANES; lane++) {
        if (lane == missing_lane) {
            continue;
        }
        if (!q3n_data_page_is_valid(s, base_block + lane, page)) {
            return false;
        }
        q3n_read_data_page(s, base_block + lane, page, tmp);
        for (i = 0; i < Q3N_PAGE_SIZE; i++) {
            buf[i] ^= tmp[i];
        }
    }

    s->stats.raid_recovered++;
    s->status |= Q3N_STATUS_RECOVERED;
    return true;
}

static bool q3n_erase_data_block(Q3NNandState *s, uint32_t data_block)
{
    uint32_t i;

    if (data_block >= s->data_block_count || s->data_meta[data_block].bad) {
        return false;
    }

    memset(&s->data_page_valid[data_block * Q3N_PAGES_PER_BLOCK], 0,
           Q3N_PAGES_PER_BLOCK);
    s->data_meta[data_block].generation++;
    s->data_meta[data_block].erased = true;
    s->stats.data_erases++;
    s->stats.generation_updates++;

    for (i = 0; i < Q3N_PAGES_PER_BLOCK; i++) {
        uint64_t stripe_key = q3n_make_stripe_key(data_block / Q3N_LANES, i);
        Q3NParityIndexEntry *entry = g_hash_table_lookup(s->parity_index,
                                                         &stripe_key);
        if (entry) {
            entry->valid = false;
        }
    }

    return true;
}

static void q3n_raise_irq(Q3NNandState *s, uint32_t bits)
{
    s->irq_status |= bits;
    if (s->irq_status & s->irq_mask) {
        qemu_irq_raise(s->irq);
    }
}

static void q3n_clear_error(Q3NNandState *s)
{
    s->status &= ~(Q3N_STATUS_ERROR | Q3N_STATUS_RECOVERED |
                   Q3N_STATUS_PARITY_VALID | Q3N_STATUS_PARITY_STALE);
}

static void q3n_finish_ok(Q3NNandState *s)
{
    s->status |= Q3N_STATUS_READY;
    q3n_raise_irq(s, Q3N_IRQ_DONE);
}

static void q3n_finish_error(Q3NNandState *s)
{
    s->status |= Q3N_STATUS_READY | Q3N_STATUS_ERROR;
    q3n_raise_irq(s, Q3N_IRQ_DONE | Q3N_IRQ_ERROR);
}

static void q3n_cmd_read_id(Q3NNandState *s)
{
    static const uint8_t id[] = { 0x2c, 0xd7, 0x90, 0xa6, 'Q', '3', 'N', 'D' };

    memset(s->data_buf, 0xff, sizeof(s->data_buf));
    memcpy(s->data_buf, id, sizeof(id));
    s->data_count = sizeof(id);
    s->data_pos = 0;
    q3n_finish_ok(s);
}

static void q3n_cmd_read_page(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;
    Q3NPhysAddr phys;

    if (!q3n_decode_data_addr(s, s->addr, &block, &page, &column) ||
        column != 0) {
        q3n_finish_error(s);
        return;
    }

    q3n_data_phys_addr(block, page, &phys);
    if (q3n_data_page_is_valid(s, block, page) &&
        !q3n_lookup_page(s, phys)) {
        if (!q3n_recover_data_page(s, block, page, s->data_buf)) {
            s->stats.raid_failed++;
            q3n_finish_error(s);
            return;
        }
        s->data_count = Q3N_PAGE_SIZE;
        s->data_pos = 0;
        q3n_finish_ok(s);
        return;
    }

    q3n_read_data_page(s, block, page, s->data_buf);
    s->data_count = Q3N_PAGE_SIZE;
    s->data_pos = 0;
    q3n_finish_ok(s);
}

static void q3n_cmd_program_page(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;

    if (s->data_count < Q3N_PAGE_SIZE ||
        !q3n_decode_data_addr(s, s->addr, &block, &page, &column) ||
        column != 0 ||
        !q3n_program_data_page(s, block, page, s->data_buf)) {
        q3n_finish_error(s);
        return;
    }

    q3n_try_update_parity(s, block, page);
    q3n_finish_ok(s);
}

static void q3n_cmd_erase_block(Q3NNandState *s)
{
    uint32_t block;
    uint32_t page;
    uint32_t column;

    if (!q3n_decode_data_addr(s, s->addr, &block, &page, &column) ||
        page != 0 || column != 0 ||
        !q3n_erase_data_block(s, block)) {
        q3n_finish_error(s);
        return;
    }

    q3n_finish_ok(s);
}

static void q3n_cmd_reset(Q3NNandState *s)
{
    q3n_clear_error(s);
    s->cmd = Q3N_CMD_NOP;
    s->data_pos = 0;
    s->data_count = 0;
    s->irq_status = 0;
    qemu_irq_lower(s->irq);
    q3n_finish_ok(s);
}

static void q3n_execute_cmd(Q3NNandState *s, uint32_t cmd)
{
    s->cmd = cmd;
    s->status &= ~Q3N_STATUS_READY;
    q3n_clear_error(s);

    switch (cmd) {
    case Q3N_CMD_NOP:
        q3n_finish_ok(s);
        break;
    case Q3N_CMD_READ_ID:
        q3n_cmd_read_id(s);
        break;
    case Q3N_CMD_READ_PAGE:
        q3n_cmd_read_page(s);
        break;
    case Q3N_CMD_PROGRAM_PAGE:
        q3n_cmd_program_page(s);
        break;
    case Q3N_CMD_ERASE_BLOCK:
        q3n_cmd_erase_block(s);
        break;
    case Q3N_CMD_RESET:
        q3n_cmd_reset(s);
        break;
    default:
        q3n_finish_error(s);
        break;
    }
}

static uint32_t q3n_read_data_window(Q3NNandState *s, unsigned size)
{
    uint32_t value = 0xffffffffU;
    unsigned i;

    if (s->data_pos >= s->data_count) {
        return value;
    }

    value = 0;
    for (i = 0; i < size; i++) {
        uint32_t shift = i * 8;
        uint8_t byte = 0xff;

        if (s->data_pos < s->data_count) {
            byte = s->data_buf[s->data_pos++];
        }
        value |= (uint32_t)byte << shift;
    }

    return value;
}

static void q3n_write_data_window(Q3NNandState *s, uint64_t value, unsigned size)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        if (s->data_pos >= sizeof(s->data_buf)) {
            s->status |= Q3N_STATUS_ERROR;
            return;
        }
        s->data_buf[s->data_pos++] = extract32(value, i * 8, 8);
    }

    if (s->data_count < s->data_pos) {
        s->data_count = s->data_pos;
    }
}

static uint64_t q3n_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    Q3NNandState *s = opaque;

    if (offset >= Q3N_REG_DATA && offset < Q3N_REG_DATA + Q3N_PAGE_SIZE) {
        return q3n_read_data_window(s, size);
    }

    switch (offset) {
    case Q3N_REG_ID:
        return Q3N_ID_VALUE;
    case Q3N_REG_CAP:
        return Q3N_CAP_SCHEME_D;
    case Q3N_REG_STATUS:
        return s->status;
    case Q3N_REG_CMD:
        return s->cmd;
    case Q3N_REG_ADDR_LO:
        return (uint32_t)s->addr;
    case Q3N_REG_ADDR_HI:
        return (uint32_t)(s->addr >> 32);
    case Q3N_REG_LEN:
        return s->len;
    case Q3N_REG_GEOM0:
        return (Q3N_PAGE_SIZE & 0xffffU) | (Q3N_OOB_SIZE << 16);
    case Q3N_REG_GEOM1:
        return (Q3N_PAGES_PER_BLOCK & 0xffffU) |
               (Q3N_BLOCKS_PER_PLANE << 16);
    case Q3N_REG_POOL0:
        return (s->data_blocks_per_plane & 0xffffU) |
               (s->parity_blocks_per_plane << 16);
    case Q3N_REG_POOL1:
        return (s->metadata_blocks_per_plane & 0xffffU) |
               (s->reserve_blocks_per_plane << 16);
    case Q3N_REG_RAID_PROFILE:
        return Q3N_RAID_PROFILE_SCHEME_D;
    case Q3N_REG_RAID_STATUS:
        return s->status & (Q3N_STATUS_RECOVERED |
                            Q3N_STATUS_PARITY_VALID |
                            Q3N_STATUS_PARITY_STALE);
    case Q3N_REG_IRQ_STATUS:
        return s->irq_status;
    case Q3N_REG_IRQ_MASK:
        return s->irq_mask;
    case Q3N_REG_STAT_DATA_PROGRAMS:
        return (uint32_t)s->stats.data_programs;
    case Q3N_REG_STAT_DATA_ERASES:
        return (uint32_t)s->stats.data_erases;
    case Q3N_REG_STAT_PARITY_APPENDS:
        return (uint32_t)s->stats.parity_appends;
    case Q3N_REG_STAT_RAID_RECOVERED:
        return (uint32_t)s->stats.raid_recovered;
    case Q3N_REG_STAT_RAID_FAILED:
        return (uint32_t)s->stats.raid_failed;
    default:
        return 0;
    }
}

static void q3n_mmio_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    Q3NNandState *s = opaque;

    if (offset >= Q3N_REG_DATA && offset < Q3N_REG_DATA + Q3N_PAGE_SIZE) {
        q3n_write_data_window(s, value, size);
        return;
    }

    switch (offset) {
    case Q3N_REG_CTRL:
        if (value & 1U) {
            q3n_execute_cmd(s, Q3N_CMD_RESET);
        }
        break;
    case Q3N_REG_CMD:
        q3n_execute_cmd(s, value);
        break;
    case Q3N_REG_ADDR_LO:
        s->addr = (s->addr & 0xffffffff00000000ULL) | (uint32_t)value;
        break;
    case Q3N_REG_ADDR_HI:
        s->addr = ((uint64_t)(uint32_t)value << 32) | (uint32_t)s->addr;
        break;
    case Q3N_REG_LEN:
        s->len = value;
        s->data_pos = 0;
        s->data_count = 0;
        break;
    case Q3N_REG_IRQ_STATUS:
        s->irq_status &= ~(uint32_t)value;
        if (!(s->irq_status & s->irq_mask)) {
            qemu_irq_lower(s->irq);
        }
        break;
    case Q3N_REG_IRQ_MASK:
        s->irq_mask = value;
        if (s->irq_status & s->irq_mask) {
            qemu_irq_raise(s->irq);
        } else {
            qemu_irq_lower(s->irq);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps q3n_mmio_ops = {
    .read = q3n_mmio_read,
    .write = q3n_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

MemoryRegion *q3n_nand_get_mmio(Q3NNandState *s)
{
    return &s->mmio;
}

void q3n_nand_set_irq(Q3NNandState *s, qemu_irq irq)
{
    s->irq = irq;
}

static void q3n_realize(DeviceState *dev, Error **errp)
{
    Q3NNandState *s = Q3N_NAND(dev);
    uint32_t pool_sum = s->data_blocks_per_plane +
                        s->parity_blocks_per_plane +
                        s->metadata_blocks_per_plane +
                        s->reserve_blocks_per_plane;
    uint32_t i;

    if (pool_sum > Q3N_BLOCKS_PER_PLANE) {
        error_setg(errp, "q3n-nand block pools exceed %u blocks per plane",
                   Q3N_BLOCKS_PER_PLANE);
        return;
    }

    s->data_block_count = s->data_blocks_per_plane * Q3N_LANES;
    s->parity_block_count = s->parity_blocks_per_plane * Q3N_LANES;
    if (s->parity_block_count < s->data_block_count / Q3N_LANES) {
        error_setg(errp, "q3n-nand parity log pool is too small for 8:1 data coverage");
        return;
    }

    s->erase_size = (uint64_t)Q3N_PAGES_PER_BLOCK * Q3N_PAGE_SIZE;
    s->visible_size = (uint64_t)s->data_block_count * s->erase_size;
    s->data_meta = g_new0(Q3NDataBlockMeta, s->data_block_count);
    s->data_page_valid = g_new0(uint8_t, s->data_block_count *
                                         Q3N_PAGES_PER_BLOCK);
    for (i = 0; i < s->data_block_count; i++) {
        s->data_meta[i].generation = 1;
        s->data_meta[i].erased = true;
    }

    s->pages = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    s->parity_index = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);
    s->status = Q3N_STATUS_READY;
}

static void q3n_unrealize(DeviceState *dev)
{
    Q3NNandState *s = Q3N_NAND(dev);

    g_clear_pointer(&s->pages, g_hash_table_destroy);
    g_clear_pointer(&s->parity_index, g_hash_table_destroy);
    g_clear_pointer(&s->data_meta, g_free);
    g_clear_pointer(&s->data_page_valid, g_free);
}

static void q3n_instance_init(Object *obj)
{
    Q3NNandState *s = Q3N_NAND(obj);

    memory_region_init_io(&s->mmio, obj, &q3n_mmio_ops, s, TYPE_Q3N_NAND,
                          Q3N_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property q3n_properties[] = {
    DEFINE_PROP_UINT32("data-blocks-per-plane", Q3NNandState,
                       data_blocks_per_plane,
                       Q3N_DEFAULT_DATA_BLOCKS_PER_PLANE),
    DEFINE_PROP_UINT32("parity-log-blocks-per-plane", Q3NNandState,
                       parity_blocks_per_plane,
                       Q3N_DEFAULT_PARITY_BLOCKS_PER_PLANE),
    DEFINE_PROP_UINT32("metadata-blocks-per-plane", Q3NNandState,
                       metadata_blocks_per_plane,
                       Q3N_DEFAULT_METADATA_BLOCKS_PER_PLANE),
    DEFINE_PROP_UINT32("reserve-blocks-per-plane", Q3NNandState,
                       reserve_blocks_per_plane,
                       Q3N_DEFAULT_RESERVE_BLOCKS_PER_PLANE),
};

static void q3n_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = q3n_realize;
    dc->unrealize = q3n_unrealize;
    device_class_set_props(dc, q3n_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo q3n_type_info = {
    .name = TYPE_Q3N_NAND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Q3NNandState),
    .instance_init = q3n_instance_init,
    .class_init = q3n_class_init,
};

static void q3n_register_types(void)
{
    type_register_static(&q3n_type_info);
}

type_init(q3n_register_types)
