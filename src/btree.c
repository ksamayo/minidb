#include "btree.h"

#include <inttypes.h>
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

DbStatus btree_update(BTree *tree, int64_t key, const void *value, uint16_t length) {
    if (length > tree->value_size || (!value && length)) {
        return DB_INVALID;
    }
    uint32_t number;
    uint8_t *page;
    DbStatus status = find_leaf(tree, key, &number, &page);
    if (status != DB_OK) {
        return status;
    }
    unsigned index = lower_bound(tree, page, key);
    if (index == read_u16(page + 2) || read_i64(cell(tree, page, index)) != key) {
        return DB_NOT_FOUND;
    }
    put_cell(tree, cell(tree, page, index), key, value, length);
    pager_dirty(tree->pager, number);
    return DB_OK;
}

static DbStatus minimum_key(BTree *tree, uint32_t number, int64_t *key) {
    for (unsigned depth = 0; depth < 64; depth++) {
        uint8_t *page;
        DbStatus status = get_node(tree, number, &page);
        if (status != DB_OK) {
            return status;
        }
        if (page[0] == PAGE_LEAF) {
            if (!read_u16(page + 2)) {
                return DB_NOT_FOUND;
            }
            *key = read_i64(cell(tree, page, 0));
            return DB_OK;
        }
        number = read_u32(page + BTREE_HEADER_SIZE);
    }
    return DB_CORRUPT;
}

static DbStatus balance_child(BTree *tree, Internal *parent, unsigned *index) {
    unsigned at = *index;
    uint8_t *child;
    DbStatus status = get_node(tree, parent->children[at], &child);
    if (status != DB_OK) {
        return status;
    }
    bool leaf = child[0] == PAGE_LEAF;
    unsigned count = read_u16(child + 2);
    unsigned minimum = leaf ? (leaf_max(tree) + 1) / 2 : BTREE_INTERNAL_MAX / 2;
    if (count >= minimum || !parent->count) {
        return DB_OK;
    }

    uint8_t *left = NULL, *right = NULL;
    if (at) {
        status = get_node(tree, parent->children[at - 1], &left);
        if (status != DB_OK) {
            return status;
        }
    }
    if (at < parent->count) {
        status = get_node(tree, parent->children[at + 1], &right);
        if (status != DB_OK) {
            return status;
        }
    }
    if ((left && left[0] != child[0]) || (right && right[0] != child[0])) {
        return DB_CORRUPT;
    }
    if (left && read_u16(left + 2) > minimum) {
        unsigned n = read_u16(left + 2);
        if (leaf) {
            memmove(cell(tree, child, 1), cell(tree, child, 0), count * cell_size(tree));
            memcpy(cell(tree, child, 0), cell(tree, left, n - 1), cell_size(tree));
            memset(cell(tree, left, n - 1), 0, cell_size(tree));
            write_u16(left + 2, (uint16_t)(n - 1));
            write_u16(child + 2, (uint16_t)(count + 1));
            parent->keys[at - 1] = read_i64(cell(tree, child, 0));
            pager_dirty(tree->pager, parent->children[at - 1]);
            pager_dirty(tree->pager, parent->children[at]);
        } else {
            Internal a, b;
            load_internal(left, &a);
            load_internal(child, &b);
            memmove(b.keys + 1, b.keys, b.count * sizeof(b.keys[0]));
            memmove(b.children + 1, b.children, (b.count + 1) * sizeof(b.children[0]));
            b.keys[0] = parent->keys[at - 1];
            b.children[0] = a.children[a.count];
            parent->keys[at - 1] = a.keys[a.count - 1];
            a.count--;
            b.count++;
            save_internal(tree, parent->children[at - 1], left, &a);
            save_internal(tree, parent->children[at], child, &b);
        }
        return DB_OK;
    }
    if (right && read_u16(right + 2) > minimum) {
        unsigned n = read_u16(right + 2);
        if (leaf) {
            memcpy(cell(tree, child, count), cell(tree, right, 0), cell_size(tree));
            memmove(cell(tree, right, 0), cell(tree, right, 1), (n - 1) * cell_size(tree));
            memset(cell(tree, right, n - 1), 0, cell_size(tree));
            write_u16(child + 2, (uint16_t)(count + 1));
            write_u16(right + 2, (uint16_t)(n - 1));
            parent->keys[at] = read_i64(cell(tree, right, 0));
            pager_dirty(tree->pager, parent->children[at]);
            pager_dirty(tree->pager, parent->children[at + 1]);
        } else {
            Internal a, b;
            load_internal(child, &a);
            load_internal(right, &b);
            a.keys[a.count] = parent->keys[at];
            a.children[a.count + 1] = b.children[0];
            a.count++;
            parent->keys[at] = b.keys[0];
            memmove(b.keys, b.keys + 1, (b.count - 1) * sizeof(b.keys[0]));
            memmove(b.children, b.children + 1, b.count * sizeof(b.children[0]));
            b.count--;
            save_internal(tree, parent->children[at], child, &a);
            save_internal(tree, parent->children[at + 1], right, &b);
        }
        return DB_OK;
    }

    unsigned sep = at ? at - 1 : at;
    if (at) {
        right = child;
    } else {
        left = child;
    }
    unsigned a_count = read_u16(left + 2), b_count = read_u16(right + 2);
    if (leaf) {
        if (a_count + b_count > leaf_max(tree)) {
            return DB_CORRUPT;
        }
        memcpy(cell(tree, left, a_count), cell(tree, right, 0), b_count * cell_size(tree));
        write_u16(left + 2, (uint16_t)(a_count + b_count));
        write_u32(left + 4, read_u32(right + 4));
        pager_dirty(tree->pager, parent->children[sep]);
    } else {
        Internal a, b;
        load_internal(left, &a);
        load_internal(right, &b);
        if (a.count + b.count + 1 > BTREE_INTERNAL_MAX) {
            return DB_CORRUPT;
        }
        a.keys[a.count] = parent->keys[sep];
        memcpy(a.keys + a.count + 1, b.keys, b.count * sizeof(b.keys[0]));
        memcpy(a.children + a.count + 1, b.children, (b.count + 1) * sizeof(b.children[0]));
        a.count += b.count + 1;
        save_internal(tree, parent->children[sep], left, &a);
    }
    status = pager_release(tree->pager, parent->children[sep + 1]);
    if (status != DB_OK) {
        return status;
    }
    memmove(parent->keys + sep, parent->keys + sep + 1,
            (parent->count - sep - 1) * sizeof(parent->keys[0]));
    memmove(parent->children + sep + 1, parent->children + sep + 2,
            (parent->count - sep - 1) * sizeof(parent->children[0]));
    parent->count--;
    *index = sep;
    return DB_OK;
}

static DbStatus delete_node(BTree *tree, uint32_t number, int64_t key, unsigned depth) {
    if (depth >= 64) {
        return DB_CORRUPT;
    }
    uint8_t *page;
    DbStatus status = get_node(tree, number, &page);
    if (status != DB_OK) {
        return status;
    }
    unsigned count = read_u16(page + 2);
    if (page[0] == PAGE_LEAF) {
        unsigned index = lower_bound(tree, page, key);
        if (index == count || read_i64(cell(tree, page, index)) != key) {
            return DB_NOT_FOUND;
        }
        memmove(cell(tree, page, index), cell(tree, page, index + 1),
                (count - index - 1) * cell_size(tree));
        memset(cell(tree, page, count - 1), 0, cell_size(tree));
        write_u16(page + 2, (uint16_t)(count - 1));
        pager_dirty(tree->pager, number);
        return DB_OK;
    }
    Internal node;
    load_internal(page, &node);
    unsigned index = child_index(&node, key);
    status = delete_node(tree, node.children[index], key, depth + 1);
    if (status != DB_OK) {
        return status;
    }
    if (index) {
        status = minimum_key(tree, node.children[index], &node.keys[index - 1]);
        if (status != DB_OK && status != DB_NOT_FOUND) {
            return status;
        }
    }
    status = balance_child(tree, &node, &index);
    if (status != DB_OK) {
        return status;
    }
    if (index) {
        status = minimum_key(tree, node.children[index], &node.keys[index - 1]);
        if (status != DB_OK) {
            return status;
        }
    }
    if (index < node.count) {
        status = minimum_key(tree, node.children[index + 1], &node.keys[index]);
        if (status != DB_OK) {
            return status;
        }
    }
    save_internal(tree, number, page, &node);
    return DB_OK;
}

DbStatus btree_delete(BTree *tree, int64_t key) {
    DbStatus status = delete_node(tree, tree->root, key, 0);
    if (status != DB_OK) {
        return status;
    }
    uint8_t *page;
    status = get_node(tree, tree->root, &page);
    if (status != DB_OK) {
        return status;
    }
    if (page[0] == PAGE_INTERNAL && !read_u16(page + 2)) {
        uint32_t old = tree->root;
        tree->root = read_u32(page + BTREE_HEADER_SIZE);
        return pager_release(tree->pager, old);
    }
    return DB_OK;
}

typedef struct {
    bool present;
    int64_t min, max;
} KeyRange;

typedef struct {
    uint8_t *seen;
    uint32_t next_leaf;
    bool has_leaf;
    BTreeStats stats;
} Verification;

static DbStatus verify_node(BTree *tree, uint32_t number, unsigned depth, Verification *check,
                            KeyRange *range) {
    if (depth >= 64 || !number || number >= tree->pager->page_count || check->seen[number]) {
        return DB_CORRUPT;
    }
    check->seen[number] = 1;
    check->stats.nodes++;
    uint8_t *page;
    DbStatus status = get_node(tree, number, &page);
    if (status != DB_OK) {
        return status;
    }
    unsigned count = read_u16(page + 2);
    if (page[0] == PAGE_LEAF) {
        if (depth && count < (leaf_max(tree) + 1) / 2) {
            return DB_CORRUPT;
        }
        if (check->has_leaf && (check->next_leaf != number || check->stats.height != depth + 1)) {
            return DB_CORRUPT;
        }
        check->has_leaf = true;
        check->next_leaf = read_u32(page + 4);
        check->stats.height = depth + 1;
        check->stats.leaves++;
        check->stats.records += count;
        range->present = count != 0;
        for (unsigned i = 0; i < count; i++) {
            uint8_t *entry = cell(tree, page, i);
            int64_t key = read_i64(entry);
            if (read_u16(entry + 8) > tree->value_size || (i && key <= range->max)) {
                return DB_CORRUPT;
            }
            if (!i) {
                range->min = key;
            }
            range->max = key;
        }
        return DB_OK;
    }
    if ((!depth && !count) || (depth && count < BTREE_INTERNAL_MAX / 2)) {
        return DB_CORRUPT;
    }
    Internal node;
    load_internal(page, &node);
    range->present = true;
    for (unsigned i = 0; i <= count; i++) {
        KeyRange child = {0};
        status = verify_node(tree, node.children[i], depth + 1, check, &child);
        if (status != DB_OK) {
            return status;
        }
        if (!child.present || (i && (node.keys[i - 1] != child.min || range->max >= child.min))) {
            return DB_CORRUPT;
        }
        if (!i) {
            range->min = child.min;
        }
        range->max = child.max;
    }
    return DB_OK;
}

DbStatus btree_verify(BTree *tree, BTreeStats *stats) {
    Verification check = {0};
    check.seen = calloc(tree->pager->page_count, 1);
    if (!check.seen) {
        return DB_NOMEM;
    }
    KeyRange range = {0};
    DbStatus status = verify_node(tree, tree->root, 0, &check, &range);
    if (status == DB_OK && check.next_leaf) {
        status = DB_CORRUPT;
    }
    if (status == DB_OK && stats) {
        *stats = check.stats;
    }
    free(check.seen);
    return status;
}

static DbStatus dump_node(BTree *tree, uint32_t number, unsigned depth, FILE *output) {
    uint8_t *page;
    DbStatus status = get_node(tree, number, &page);
    if (status != DB_OK) {
        return status;
    }
    unsigned count = read_u16(page + 2);
    fprintf(output, "%*spage %u %s (%u keys)", (int)(depth * 2), "", number,
            page[0] == PAGE_LEAF ? "leaf" : "internal", count);
    if (page[0] == PAGE_LEAF) {
        fprintf(output, " next=%u:", read_u32(page + 4));
        for (unsigned i = 0; i < count; i++) {
            fprintf(output, " %" PRId64, read_i64(cell(tree, page, i)));
        }
        fputc('\n', output);
        return ferror(output) ? DB_IO : DB_OK;
    }
    Internal node;
    load_internal(page, &node);
    fputs(":", output);
    for (unsigned i = 0; i < count; i++) {
        fprintf(output, " %" PRId64, node.keys[i]);
    }
    fputc('\n', output);
    for (unsigned i = 0; i <= count; i++) {
        status = dump_node(tree, node.children[i], depth + 1, output);
        if (status != DB_OK) {
            return status;
        }
    }
    return ferror(output) ? DB_IO : DB_OK;
}

DbStatus btree_dump(BTree *tree, FILE *output) {
    DbStatus status = btree_verify(tree, NULL);
    return status == DB_OK ? dump_node(tree, tree->root, 0, output) : status;
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
