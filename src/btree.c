#include "btree.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned count;
    int64_t keys[BTREE_INTERNAL_MAX + 1];
    uint32_t children[BTREE_INTERNAL_MAX + 2];
} Internal;

typedef struct {
    bool present;
    int64_t key;
    uint32_t right;
} Split;

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
    unsigned type = (*page)[0];
    if ((type != PAGE_LEAF && type != PAGE_INTERNAL) || read_u16(*page + 12) != tree->value_size ||
        read_u16(*page + 2) > (type == PAGE_LEAF ? leaf_max(tree) : BTREE_INTERNAL_MAX) ||
        (type == PAGE_LEAF && read_u32(*page + 4) >= tree->pager->page_count)) {
        return DB_CORRUPT;
    }
    return DB_OK;
}

static void load_internal(const uint8_t *page, Internal *node) {
    node->count = read_u16(page + 2);
    node->children[0] = read_u32(page + BTREE_HEADER_SIZE);
    for (unsigned i = 0; i < node->count; i++) {
        node->keys[i] = read_i64(page + BTREE_HEADER_SIZE + 4 + i * 12);
        node->children[i + 1] = read_u32(page + BTREE_HEADER_SIZE + 12 + i * 12);
    }
}

static void save_internal(BTree *tree, uint32_t number, uint8_t *page, const Internal *node) {
    memset(page, 0, PAGE_SIZE);
    page[0] = PAGE_INTERNAL;
    write_u16(page + 2, (uint16_t)node->count);
    write_u16(page + 12, tree->value_size);
    write_u32(page + BTREE_HEADER_SIZE, node->children[0]);
    for (unsigned i = 0; i < node->count; i++) {
        write_u64(page + BTREE_HEADER_SIZE + 4 + i * 12, (uint64_t)node->keys[i]);
        write_u32(page + BTREE_HEADER_SIZE + 12 + i * 12, node->children[i + 1]);
    }
    pager_dirty(tree->pager, number);
}

static unsigned child_index(const Internal *node, int64_t key) {
    unsigned lo = 0, hi = node->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (key >= node->keys[mid]) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static DbStatus find_leaf(BTree *tree, int64_t key, uint32_t *number, uint8_t **page) {
    *number = tree->root;
    for (unsigned depth = 0; depth < 64; depth++) {
        DbStatus status = get_node(tree, *number, page);
        if (status != DB_OK || (*page)[0] == PAGE_LEAF) {
            return status;
        }
        Internal node;
        load_internal(*page, &node);
        *number = node.children[child_index(&node, key)];
    }
    return DB_CORRUPT;
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

static DbStatus insert_node(BTree *tree, uint32_t number, int64_t key, const void *value,
                            uint16_t length, Split *split, unsigned depth) {
    if (depth >= 64) {
        return DB_CORRUPT;
    }
    split->present = false;
    uint8_t *page;
    DbStatus status = get_node(tree, number, &page);
    if (status != DB_OK) {
        return status;
    }
    if (page[0] == PAGE_LEAF) {
        unsigned count = read_u16(page + 2);
        unsigned index = lower_bound(tree, page, key);
        if (index < count && read_i64(cell(tree, page, index)) == key) {
            return DB_DUPLICATE;
        }
        if (count < leaf_max(tree)) {
            memmove(cell(tree, page, index + 1), cell(tree, page, index),
                    (count - index) * cell_size(tree));
            put_cell(tree, cell(tree, page, index), key, value, length);
            write_u16(page + 2, (uint16_t)(count + 1));
            pager_dirty(tree->pager, number);
            return DB_OK;
        }
        size_t size = cell_size(tree);
        uint8_t *buffer = malloc((count + 1) * size);
        if (!buffer) {
            return DB_NOMEM;
        }
        memcpy(buffer, cell(tree, page, 0), index * size);
        put_cell(tree, buffer + index * size, key, value, length);
        memcpy(buffer + (index + 1) * size, cell(tree, page, index), (count - index) * size);
        uint8_t *right;
        uint32_t right_id;
        status = pager_allocate(tree->pager, &right_id, &right);
        if (status != DB_OK) {
            free(buffer);
            return status;
        }
        unsigned left_count = (count + 1) / 2;
        unsigned right_count = count + 1 - left_count;
        right[0] = PAGE_LEAF;
        write_u16(right + 12, tree->value_size);
        write_u16(right + 2, (uint16_t)right_count);
        write_u32(right + 4, read_u32(page + 4));
        memcpy(cell(tree, right, 0), buffer + left_count * size, right_count * size);
        memset(cell(tree, page, 0), 0, count * size);
        memcpy(cell(tree, page, 0), buffer, left_count * size);
        write_u16(page + 2, (uint16_t)left_count);
        write_u32(page + 4, right_id);
        pager_dirty(tree->pager, number);
        *split = (Split){true, read_i64(cell(tree, right, 0)), right_id};
        free(buffer);
        return DB_OK;
    }

    Internal node;
    load_internal(page, &node);
    unsigned index = child_index(&node, key);
    Split child_split;
    status = insert_node(tree, node.children[index], key, value, length, &child_split, depth + 1);
    if (status != DB_OK || !child_split.present) {
        return status;
    }
    memmove(node.keys + index + 1, node.keys + index, (node.count - index) * sizeof(node.keys[0]));
    memmove(node.children + index + 2, node.children + index + 1,
            (node.count - index) * sizeof(node.children[0]));
    node.keys[index] = child_split.key;
    node.children[index + 1] = child_split.right;
    node.count++;
    if (node.count <= BTREE_INTERNAL_MAX) {
        save_internal(tree, number, page, &node);
        return DB_OK;
    }
    uint32_t right_id;
    uint8_t *right_page;
    status = pager_allocate(tree->pager, &right_id, &right_page);
    if (status != DB_OK) {
        return status;
    }
    unsigned middle = node.count / 2;
    Internal right;
    right.count = node.count - middle - 1;
    memcpy(right.keys, node.keys + middle + 1, right.count * sizeof(right.keys[0]));
    memcpy(right.children, node.children + middle + 1,
           (right.count + 1) * sizeof(right.children[0]));
    *split = (Split){true, node.keys[middle], right_id};
    node.count = middle;
    save_internal(tree, number, page, &node);
    save_internal(tree, right_id, right_page, &right);
    return DB_OK;
}

DbStatus btree_insert(BTree *tree, int64_t key, const void *value, uint16_t length) {
    if (length > tree->value_size || (!value && length)) {
        return DB_INVALID;
    }
    Split split;
    DbStatus status = insert_node(tree, tree->root, key, value, length, &split, 0);
    if (status != DB_OK || !split.present) {
        return status;
    }
    uint32_t root;
    uint8_t *page;
    status = pager_allocate(tree->pager, &root, &page);
    if (status != DB_OK) {
        return status;
    }
    Internal node;
    node.count = 1;
    node.keys[0] = split.key;
    node.children[0] = tree->root;
    node.children[1] = split.right;
    save_internal(tree, root, page, &node);
    tree->root = root;
    return DB_OK;
}

DbStatus btree_find(BTree *tree, int64_t key, void *value, size_t capacity, uint16_t *length) {
    uint8_t *page;
    uint32_t number;
    DbStatus status = find_leaf(tree, key, &number, &page);
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
    uint32_t number;
    uint8_t *first;
    DbStatus start = find_leaf(tree, INT64_MIN, &number, &first);
    if (start != DB_OK) {
        return start;
    }
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
        if (page[0] != PAGE_LEAF) {
            return DB_CORRUPT;
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
