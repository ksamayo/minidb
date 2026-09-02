#include "btree.h"

#include <string.h>

static unsigned cell_size(const BTree *tree) {
    return 10u + tree->value_size;
}

static unsigned leaf_max(const BTree *tree) {
    return (PAGE_SIZE - BTREE_HEADER_SIZE) / cell_size(tree);
}

static uint8_t *cell(const BTree *tree, uint8_t *page, unsigned index) {
    return page + BTREE_HEADER_SIZE + index * cell_size(tree);
}

static DbStatus get_node(BTree *tree, uint32_t number, uint8_t **page) {
    if (!number || number >= tree->pager->page_count) {
        return DB_CORRUPT;
    }
    DbStatus status = pager_get(tree->pager, number, page);
    if (status != DB_OK) {
        return status;
    }
    if ((*page)[0] != PAGE_LEAF || read_u16(*page + 12) != tree->value_size ||
        read_u16(*page + 2) > leaf_max(tree) || read_u32(*page + 4) >= tree->pager->page_count) {
        return DB_CORRUPT;
    }
    return DB_OK;
}

static unsigned lower_bound(BTree *tree, uint8_t *page, int64_t key) {
    unsigned lo = 0, hi = read_u16(page + 2);
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (read_i64(cell(tree, page, mid)) < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void put_cell(BTree *tree, uint8_t *target, int64_t key, const void *value,
                     uint16_t length) {
    memset(target, 0, cell_size(tree));
    write_u64(target, (uint64_t)key);
    write_u16(target + 8, length);
    if (length) {
        memcpy(target + 10, value, length);
    }
}

DbStatus btree_open(BTree *tree, Pager *pager, uint32_t root, uint16_t value_size) {
    if (!tree || !pager || !value_size || value_size > BTREE_MAX_VALUE) {
        return DB_INVALID;
    }
    *tree = (BTree){pager, root, value_size};
    uint8_t *page;
    return get_node(tree, root, &page);
}

DbStatus btree_create(BTree *tree, Pager *pager, uint16_t value_size) {
    if (!tree || !pager || !value_size || value_size > BTREE_MAX_VALUE) {
        return DB_INVALID;
    }
    uint32_t root;
    uint8_t *page;
    DbStatus status = pager_allocate(pager, &root, &page);
    if (status != DB_OK) {
        return status;
    }
    page[0] = PAGE_LEAF;
    write_u16(page + 12, value_size);
    *tree = (BTree){pager, root, value_size};
    return DB_OK;
}

DbStatus btree_insert(BTree *tree, int64_t key, const void *value, uint16_t length) {
    if (length > tree->value_size || (!value && length)) {
        return DB_INVALID;
    }
    uint8_t *page;
    DbStatus status = get_node(tree, tree->root, &page);
    if (status != DB_OK) {
        return status;
    }
    unsigned count = read_u16(page + 2);
    unsigned index = lower_bound(tree, page, key);
    if (index < count && read_i64(cell(tree, page, index)) == key) {
        return DB_DUPLICATE;
    }
    if (count == leaf_max(tree)) {
        return DB_FULL;
    }
    memmove(cell(tree, page, index + 1), cell(tree, page, index),
            (count - index) * cell_size(tree));
    put_cell(tree, cell(tree, page, index), key, value, length);
    write_u16(page + 2, (uint16_t)(count + 1));
    pager_dirty(tree->pager, tree->root);
    return DB_OK;
}

DbStatus btree_find(BTree *tree, int64_t key, void *value, size_t capacity, uint16_t *length) {
    uint8_t *page;
    DbStatus status = get_node(tree, tree->root, &page);
    if (status != DB_OK) {
        return status;
    }
    unsigned index = lower_bound(tree, page, key);
    if (index == read_u16(page + 2) || read_i64(cell(tree, page, index)) != key) {
        return DB_NOT_FOUND;
    }
    uint8_t *entry = cell(tree, page, index);
    uint16_t size = read_u16(entry + 8);
    if (size > tree->value_size) {
        return DB_CORRUPT;
    }
    if (size > capacity || (!value && size)) {
        return DB_INVALID;
    }
    if (size) {
        memcpy(value, entry + 10, size);
    }
    if (length) {
        *length = size;
    }
    return DB_OK;
}

DbStatus btree_scan(BTree *tree, BTreeVisitor visitor, void *context) {
    if (!visitor) {
        return DB_INVALID;
    }
    uint32_t number = tree->root;
    uint32_t steps = 0;
    while (number) {
        if (++steps >= tree->pager->page_count) {
            return DB_CORRUPT;
        }
        uint8_t *page;
        DbStatus status = get_node(tree, number, &page);
        if (status != DB_OK) {
            return status;
        }
        for (unsigned i = 0; i < read_u16(page + 2); i++) {
            uint8_t *entry = cell(tree, page, i);
            uint16_t size = read_u16(entry + 8);
            if (size > tree->value_size) {
                return DB_CORRUPT;
            }
            status = visitor(read_i64(entry), entry + 10, size, context);
            if (status != DB_OK) {
                return status;
            }
        }
        number = read_u32(page + 4);
    }
    return DB_OK;
}
