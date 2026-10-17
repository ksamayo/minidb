#include "btree.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

enum {
    NODE_COUNT = 2,
    LEAF_NEXT = 4,
    NODE_VALUE_SIZE = 12,
    LEAF_VALUE_LENGTH = 8,
    LEAF_VALUE_DATA = 10,
    INTERNAL_KEY_OFFSET = 4,
    INTERNAL_CELL_SIZE = 12,
    TREE_MAX_DEPTH = 64
};

typedef struct {
    unsigned count;
    int64_t keys[BTREE_INTERNAL_MAX + 1];
    uint32_t children[BTREE_INTERNAL_MAX + 2];
} InternalNode;

typedef struct {
    bool present;
    int64_t key;
    uint32_t right;
} Split;

typedef struct {
    uint32_t numbers[TREE_MAX_DEPTH + 1];
    uint8_t *pages[TREE_MAX_DEPTH + 1];
    unsigned count, used;
} ReservedPages;

static unsigned leaf_cell_size(const BTree *tree) {
    return LEAF_VALUE_DATA + tree->value_size;
}

static unsigned leaf_capacity(const BTree *tree) {
    return (PAGE_SIZE - BTREE_HEADER_SIZE) / leaf_cell_size(tree);
}

static uint8_t *leaf_cell(const BTree *tree, uint8_t *page, unsigned index) {
    return page + BTREE_HEADER_SIZE + index * leaf_cell_size(tree);
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

    if ((type != PAGE_LEAF && type != PAGE_INTERNAL) ||
        read_u16(*page + NODE_VALUE_SIZE) != tree->value_size ||
        read_u16(*page + NODE_COUNT) >
            (type == PAGE_LEAF ? leaf_capacity(tree) : BTREE_INTERNAL_MAX) ||
        (type == PAGE_LEAF && read_u32(*page + LEAF_NEXT) >= tree->pager->page_count)) {
        return DB_CORRUPT;
    }

    return DB_OK;
}

static void load_internal(const uint8_t *page, InternalNode *node) {
    node->count = read_u16(page + NODE_COUNT);
    node->children[0] = read_u32(page + BTREE_HEADER_SIZE);

    for (unsigned i = 0; i < node->count; i++) {
        node->keys[i] =
            read_i64(page + BTREE_HEADER_SIZE + INTERNAL_KEY_OFFSET + i * INTERNAL_CELL_SIZE);
        node->children[i + 1] =
            read_u32(page + BTREE_HEADER_SIZE + INTERNAL_CELL_SIZE + i * INTERNAL_CELL_SIZE);
    }
}

static void save_internal(BTree *tree, uint32_t number, uint8_t *page, const InternalNode *node) {
    memset(page, 0, PAGE_SIZE);
    page[0] = PAGE_INTERNAL;
    write_u16(page + NODE_COUNT, (uint16_t)node->count);
    write_u16(page + NODE_VALUE_SIZE, tree->value_size);
    write_u32(page + BTREE_HEADER_SIZE, node->children[0]);

    for (unsigned i = 0; i < node->count; i++) {
        write_u64(page + BTREE_HEADER_SIZE + INTERNAL_KEY_OFFSET + i * INTERNAL_CELL_SIZE,
                  (uint64_t)node->keys[i]);
        write_u32(page + BTREE_HEADER_SIZE + INTERNAL_CELL_SIZE + i * INTERNAL_CELL_SIZE,
                  node->children[i + 1]);
    }

    pager_dirty(tree->pager, number);
}

static unsigned child_index(const InternalNode *node, int64_t key) {
    unsigned low = 0, high = node->count;

    while (low < high) {
        unsigned middle = low + (high - low) / 2;

        if (key >= node->keys[middle]) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low;
}

static DbStatus find_leaf(BTree *tree, int64_t key, uint32_t *number, uint8_t **page) {
    *number = tree->root;

    for (unsigned depth = 0; depth < TREE_MAX_DEPTH; depth++) {
        DbStatus status = get_node(tree, *number, page);

        if (status != DB_OK || (*page)[0] == PAGE_LEAF) {
            return status;
        }

        InternalNode node;

        load_internal(*page, &node);
        *number = node.children[child_index(&node, key)];
    }

    return DB_CORRUPT;
}

static unsigned leaf_key_index(BTree *tree, uint8_t *page, int64_t key) {
    unsigned low = 0, high = read_u16(page + NODE_COUNT);

    while (low < high) {
        unsigned middle = low + (high - low) / 2;

        if (read_i64(leaf_cell(tree, page, middle)) < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low;
}

static void put_cell(BTree *tree, uint8_t *target, int64_t key, const void *value,
                     uint16_t length) {
    memset(target, 0, leaf_cell_size(tree));
    write_u64(target, (uint64_t)key);
    write_u16(target + LEAF_VALUE_LENGTH, length);

    if (length) {
        memcpy(target + LEAF_VALUE_DATA, value, length);
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
    write_u16(page + NODE_VALUE_SIZE, value_size);
    *tree = (BTree){pager, root, value_size};

    return DB_OK;
}

/* Reserve split pages before mutation. */
static unsigned split_page_count(const bool *full, unsigned depth) {
    unsigned needed = 1;
    bool new_root = true;

    for (unsigned i = depth; i > 0; i--) {
        if (!full[i - 1]) {
            new_root = false;

            break;
        }

        needed++;
    }

    if (new_root) {
        needed++;
    }

    return needed;
}

static DbStatus reserve_pages(BTree *tree, ReservedPages *reserved, unsigned needed) {
    while (reserved->count < needed) {
        unsigned i = reserved->count;
        DbStatus status = pager_allocate(tree->pager, &reserved->numbers[i], &reserved->pages[i]);

        if (status != DB_OK) {
            return status;
        }

        reserved->count++;
    }

    return DB_OK;
}

static DbStatus reserve_insert(BTree *tree, int64_t key, ReservedPages *reserved) {
    uint32_t number = tree->root;
    bool full[TREE_MAX_DEPTH];

    for (unsigned depth = 0; depth < TREE_MAX_DEPTH; depth++) {
        uint8_t *page;
        DbStatus status = get_node(tree, number, &page);

        if (status != DB_OK) {
            return status;
        }

        if (page[0] == PAGE_INTERNAL) {
            InternalNode node;

            load_internal(page, &node);
            full[depth] = node.count == BTREE_INTERNAL_MAX;
            number = node.children[child_index(&node, key)];

            continue;
        }

        unsigned count = read_u16(page + NODE_COUNT), index = leaf_key_index(tree, page, key);

        if (index < count && read_i64(leaf_cell(tree, page, index)) == key) {
            return DB_DUPLICATE;
        }

        if (count < leaf_capacity(tree)) {
            return DB_OK;
        }

        return reserve_pages(tree, reserved, split_page_count(full, depth));
    }

    return DB_CORRUPT;
}

static void take_reserved(ReservedPages *reserved, uint32_t *number, uint8_t **page) {
    *number = reserved->numbers[reserved->used];
    *page = reserved->pages[reserved->used++];
}

static DbStatus split_leaf(BTree *tree, uint32_t number, uint8_t *page, unsigned index, int64_t key,
                           const void *value, uint16_t length, Split *split,
                           ReservedPages *reserved) {
    unsigned count = read_u16(page + NODE_COUNT);
    size_t size = leaf_cell_size(tree);
    uint8_t *buffer = malloc((count + 1) * size);

    if (!buffer) {
        return DB_NOMEM;
    }

    memcpy(buffer, leaf_cell(tree, page, 0), index * size);
    put_cell(tree, buffer + index * size, key, value, length);
    memcpy(buffer + (index + 1) * size, leaf_cell(tree, page, index), (count - index) * size);

    uint8_t *right;
    uint32_t right_id;

    take_reserved(reserved, &right_id, &right);

    unsigned left_count = (count + 1) / 2;
    unsigned right_count = count + 1 - left_count;

    right[0] = PAGE_LEAF;
    write_u16(right + NODE_VALUE_SIZE, tree->value_size);
    write_u16(right + NODE_COUNT, (uint16_t)right_count);
    write_u32(right + LEAF_NEXT, read_u32(page + LEAF_NEXT));
    memcpy(leaf_cell(tree, right, 0), buffer + left_count * size, right_count * size);

    memset(leaf_cell(tree, page, 0), 0, count * size);
    memcpy(leaf_cell(tree, page, 0), buffer, left_count * size);
    write_u16(page + NODE_COUNT, (uint16_t)left_count);
    write_u32(page + LEAF_NEXT, right_id);

    pager_dirty(tree->pager, number);
    *split = (Split){true, read_i64(leaf_cell(tree, right, 0)), right_id};

    free(buffer);

    return DB_OK;
}

static DbStatus insert_leaf(BTree *tree, uint32_t number, uint8_t *page, int64_t key,
                            const void *value, uint16_t length, Split *split,
                            ReservedPages *reserved) {
    unsigned count = read_u16(page + NODE_COUNT);
    unsigned index = leaf_key_index(tree, page, key);

    if (index < count && read_i64(leaf_cell(tree, page, index)) == key) {
        return DB_DUPLICATE;
    }

    if (count < leaf_capacity(tree)) {
        memmove(leaf_cell(tree, page, index + 1), leaf_cell(tree, page, index),
                (count - index) * leaf_cell_size(tree));
        put_cell(tree, leaf_cell(tree, page, index), key, value, length);
        write_u16(page + NODE_COUNT, (uint16_t)(count + 1));
        pager_dirty(tree->pager, number);

        return DB_OK;
    }

    return split_leaf(tree, number, page, index, key, value, length, split, reserved);
}

static void insert_child_split(InternalNode *node, unsigned index, const Split *split) {
    memmove(node->keys + index + 1, node->keys + index,
            (node->count - index) * sizeof(node->keys[0]));
    memmove(node->children + index + 2, node->children + index + 1,
            (node->count - index) * sizeof(node->children[0]));

    node->keys[index] = split->key;
    node->children[index + 1] = split->right;
    node->count++;
}

static DbStatus split_internal(BTree *tree, uint32_t number, uint8_t *page, InternalNode *node,
                               Split *split, ReservedPages *reserved) {
    uint32_t right_id;
    uint8_t *right_page;

    take_reserved(reserved, &right_id, &right_page);

    unsigned middle = node->count / 2;
    InternalNode right;

    right.count = node->count - middle - 1;
    memcpy(right.keys, node->keys + middle + 1, right.count * sizeof(right.keys[0]));
    memcpy(right.children, node->children + middle + 1,
           (right.count + 1) * sizeof(right.children[0]));

    *split = (Split){true, node->keys[middle], right_id};
    node->count = middle;

    save_internal(tree, number, page, node);
    save_internal(tree, right_id, right_page, &right);

    return DB_OK;
}

static DbStatus insert_node(BTree *tree, uint32_t number, int64_t key, const void *value,
                            uint16_t length, Split *split, ReservedPages *reserved,
                            unsigned depth) {
    if (depth >= TREE_MAX_DEPTH) {
        return DB_CORRUPT;
    }

    split->present = false;

    uint8_t *page;
    DbStatus status = get_node(tree, number, &page);

    if (status != DB_OK) {
        return status;
    }

    if (page[0] == PAGE_LEAF) {
        return insert_leaf(tree, number, page, key, value, length, split, reserved);
    }

    InternalNode node;

    load_internal(page, &node);

    unsigned index = child_index(&node, key);
    Split child_split;

    status = insert_node(tree, node.children[index], key, value, length, &child_split, reserved,
                         depth + 1);

    if (status != DB_OK || !child_split.present) {
        return status;
    }

    insert_child_split(&node, index, &child_split);

    if (node.count <= BTREE_INTERNAL_MAX) {
        save_internal(tree, number, page, &node);

        return DB_OK;
    }

    return split_internal(tree, number, page, &node, split, reserved);
}

static void create_split_root(BTree *tree, const Split *split, ReservedPages *reserved) {
    uint32_t root;
    uint8_t *page;

    take_reserved(reserved, &root, &page);

    InternalNode node;

    node.count = 1;
    node.keys[0] = split->key;
    node.children[0] = tree->root;
    node.children[1] = split->right;
    save_internal(tree, root, page, &node);
    tree->root = root;
}

DbStatus btree_insert(BTree *tree, int64_t key, const void *value, uint16_t length) {
    if (length > tree->value_size || (!value && length)) {
        return DB_INVALID;
    }

    ReservedPages reserved = {0};
    Split split;
    DbStatus status = reserve_insert(tree, key, &reserved);

    if (status == DB_OK) {
        status = insert_node(tree, tree->root, key, value, length, &split, &reserved, 0);
    }

    if (status == DB_OK && split.present) {
        create_split_root(tree, &split, &reserved);
    }

    for (unsigned i = reserved.used; i < reserved.count; i++) {
        pager_release(tree->pager, reserved.numbers[i]);
    }

    return status;
}

DbStatus btree_find(BTree *tree, int64_t key, void *value, size_t capacity, uint16_t *length) {
    uint8_t *page;
    uint32_t number;
    DbStatus status = find_leaf(tree, key, &number, &page);

    if (status != DB_OK) {
        return status;
    }

    unsigned index = leaf_key_index(tree, page, key);

    if (index == read_u16(page + NODE_COUNT) || read_i64(leaf_cell(tree, page, index)) != key) {
        return DB_NOT_FOUND;
    }

    uint8_t *entry = leaf_cell(tree, page, index);
    uint16_t size = read_u16(entry + LEAF_VALUE_LENGTH);

    if (size > tree->value_size) {
        return DB_CORRUPT;
    }

    if (size > capacity || (!value && size)) {
        return DB_INVALID;
    }

    if (size) {
        memcpy(value, entry + LEAF_VALUE_DATA, size);
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

    unsigned index = leaf_key_index(tree, page, key);

    if (index == read_u16(page + NODE_COUNT) || read_i64(leaf_cell(tree, page, index)) != key) {
        return DB_NOT_FOUND;
    }

    put_cell(tree, leaf_cell(tree, page, index), key, value, length);
    pager_dirty(tree->pager, number);

    return DB_OK;
}

typedef struct {
    bool min_changed, has_min;
    int64_t min;
} DeleteResult;

static void borrow_from_left(BTree *tree, InternalNode *parent, unsigned at, uint8_t *left,
                             uint8_t *child) {
    bool leaf = child[0] == PAGE_LEAF;
    unsigned count = read_u16(child + NODE_COUNT);
    unsigned left_count = read_u16(left + NODE_COUNT);

    if (leaf) {
        memmove(leaf_cell(tree, child, 1), leaf_cell(tree, child, 0), count * leaf_cell_size(tree));
        memcpy(leaf_cell(tree, child, 0), leaf_cell(tree, left, left_count - 1),
               leaf_cell_size(tree));

        memset(leaf_cell(tree, left, left_count - 1), 0, leaf_cell_size(tree));

        write_u16(left + NODE_COUNT, (uint16_t)(left_count - 1));
        write_u16(child + NODE_COUNT, (uint16_t)(count + 1));
        parent->keys[at - 1] = read_i64(leaf_cell(tree, child, 0));

        pager_dirty(tree->pager, parent->children[at - 1]);
        pager_dirty(tree->pager, parent->children[at]);
    } else {
        InternalNode left_node, child_node;

        load_internal(left, &left_node);
        load_internal(child, &child_node);

        memmove(child_node.keys + 1, child_node.keys,
                child_node.count * sizeof(child_node.keys[0]));
        memmove(child_node.children + 1, child_node.children,
                (child_node.count + 1) * sizeof(child_node.children[0]));

        child_node.keys[0] = parent->keys[at - 1];
        child_node.children[0] = left_node.children[left_node.count];
        parent->keys[at - 1] = left_node.keys[left_node.count - 1];
        left_node.count--;
        child_node.count++;

        save_internal(tree, parent->children[at - 1], left, &left_node);
        save_internal(tree, parent->children[at], child, &child_node);
    }
}

static void borrow_from_right(BTree *tree, InternalNode *parent, unsigned at, uint8_t *child,
                              uint8_t *right) {
    bool leaf = child[0] == PAGE_LEAF;
    unsigned count = read_u16(child + NODE_COUNT);
    unsigned right_count = read_u16(right + NODE_COUNT);

    if (leaf) {
        memcpy(leaf_cell(tree, child, count), leaf_cell(tree, right, 0), leaf_cell_size(tree));

        memmove(leaf_cell(tree, right, 0), leaf_cell(tree, right, 1),
                (right_count - 1) * leaf_cell_size(tree));
        memset(leaf_cell(tree, right, right_count - 1), 0, leaf_cell_size(tree));

        write_u16(child + NODE_COUNT, (uint16_t)(count + 1));
        write_u16(right + NODE_COUNT, (uint16_t)(right_count - 1));
        parent->keys[at] = read_i64(leaf_cell(tree, right, 0));

        if (at) {
            parent->keys[at - 1] = read_i64(leaf_cell(tree, child, 0));
        }

        pager_dirty(tree->pager, parent->children[at]);
        pager_dirty(tree->pager, parent->children[at + 1]);
    } else {
        InternalNode child_node, right_node;

        load_internal(child, &child_node);
        load_internal(right, &right_node);

        child_node.keys[child_node.count] = parent->keys[at];
        child_node.children[child_node.count + 1] = right_node.children[0];
        child_node.count++;

        parent->keys[at] = right_node.keys[0];
        memmove(right_node.keys, right_node.keys + 1,
                (right_node.count - 1) * sizeof(right_node.keys[0]));
        memmove(right_node.children, right_node.children + 1,
                right_node.count * sizeof(right_node.children[0]));
        right_node.count--;

        save_internal(tree, parent->children[at], child, &child_node);
        save_internal(tree, parent->children[at + 1], right, &right_node);
    }
}

static DbStatus merge_leaf_pages(BTree *tree, uint32_t number, uint8_t *left, uint8_t *right) {
    unsigned left_count = read_u16(left + NODE_COUNT);
    unsigned right_count = read_u16(right + NODE_COUNT);

    if (left_count + right_count > leaf_capacity(tree)) {
        return DB_CORRUPT;
    }

    memcpy(leaf_cell(tree, left, left_count), leaf_cell(tree, right, 0),
           right_count * leaf_cell_size(tree));
    write_u16(left + NODE_COUNT, (uint16_t)(left_count + right_count));
    write_u32(left + LEAF_NEXT, read_u32(right + LEAF_NEXT));

    pager_dirty(tree->pager, number);

    return DB_OK;
}

static DbStatus merge_internal_pages(BTree *tree, uint32_t number, uint8_t *left,
                                     const uint8_t *right, int64_t separator) {
    InternalNode left_node, right_node;

    load_internal(left, &left_node);
    load_internal(right, &right_node);

    if (left_node.count + right_node.count + 1 > BTREE_INTERNAL_MAX) {
        return DB_CORRUPT;
    }

    left_node.keys[left_node.count] = separator;
    memcpy(left_node.keys + left_node.count + 1, right_node.keys,
           right_node.count * sizeof(right_node.keys[0]));
    memcpy(left_node.children + left_node.count + 1, right_node.children,
           (right_node.count + 1) * sizeof(right_node.children[0]));
    left_node.count += right_node.count + 1;

    save_internal(tree, number, left, &left_node);

    return DB_OK;
}

static DbStatus merge_children(BTree *tree, InternalNode *parent, unsigned sep, uint8_t *left,
                               uint8_t *right) {
    DbStatus status;

    if (left[0] == PAGE_LEAF) {
        status = merge_leaf_pages(tree, parent->children[sep], left, right);
    } else {
        status = merge_internal_pages(tree, parent->children[sep], left, right, parent->keys[sep]);
    }

    if (status != DB_OK) {
        return status;
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

    return DB_OK;
}

static DbStatus load_siblings(BTree *tree, const InternalNode *parent, unsigned at, uint8_t **left,
                              uint8_t **right) {
    DbStatus status;

    if (at) {
        status = get_node(tree, parent->children[at - 1], left);

        if (status != DB_OK) {
            return status;
        }
    }

    if (at < parent->count) {
        status = get_node(tree, parent->children[at + 1], right);

        if (status != DB_OK) {
            return status;
        }
    }

    return DB_OK;
}

static DbStatus balance_child(BTree *tree, InternalNode *parent, unsigned at) {
    uint8_t *child;
    DbStatus status = get_node(tree, parent->children[at], &child);

    if (status != DB_OK) {
        return status;
    }

    bool leaf = child[0] == PAGE_LEAF;
    unsigned count = read_u16(child + NODE_COUNT);
    unsigned minimum = leaf ? (leaf_capacity(tree) + 1) / 2 : BTREE_INTERNAL_MAX / 2;

    if (count >= minimum || !parent->count) {
        return DB_OK;
    }

    uint8_t *left = NULL, *right = NULL;

    status = load_siblings(tree, parent, at, &left, &right);

    if (status != DB_OK) {
        return status;
    }

    if ((left && left[0] != child[0]) || (right && right[0] != child[0])) {
        return DB_CORRUPT;
    }

    if (left && read_u16(left + NODE_COUNT) > minimum) {
        borrow_from_left(tree, parent, at, left, child);

        return DB_OK;
    }

    if (right && read_u16(right + NODE_COUNT) > minimum) {
        borrow_from_right(tree, parent, at, child, right);

        return DB_OK;
    }

    unsigned separator = at ? at - 1 : at;

    if (at) {
        right = child;
    } else {
        left = child;
    }

    return merge_children(tree, parent, separator, left, right);
}

static DbStatus delete_leaf(BTree *tree, uint32_t number, uint8_t *page, int64_t key,
                            DeleteResult *result) {
    unsigned count = read_u16(page + NODE_COUNT);
    unsigned index = leaf_key_index(tree, page, key);

    if (index == count || read_i64(leaf_cell(tree, page, index)) != key) {
        return DB_NOT_FOUND;
    }

    memmove(leaf_cell(tree, page, index), leaf_cell(tree, page, index + 1),
            (count - index - 1) * leaf_cell_size(tree));
    memset(leaf_cell(tree, page, count - 1), 0, leaf_cell_size(tree));
    write_u16(page + NODE_COUNT, (uint16_t)(count - 1));
    pager_dirty(tree->pager, number);
    result->min_changed = index == 0;
    result->has_min = count > 1;

    if (result->has_min) {
        result->min = read_i64(leaf_cell(tree, page, 0));
    }

    return DB_OK;
}

static DbStatus delete_node(BTree *tree, uint32_t number, int64_t key, DeleteResult *result,
                            unsigned depth) {
    if (depth >= TREE_MAX_DEPTH) {
        return DB_CORRUPT;
    }

    uint8_t *page;
    DbStatus status = get_node(tree, number, &page);

    if (status != DB_OK) {
        return status;
    }

    if (page[0] == PAGE_LEAF) {
        return delete_leaf(tree, number, page, key, result);
    }

    InternalNode node;

    load_internal(page, &node);

    unsigned index = child_index(&node, key);
    DeleteResult child = {0};

    status = delete_node(tree, node.children[index], key, &child, depth + 1);

    if (status != DB_OK) {
        return status;
    }

    result->min_changed = !index && child.min_changed;
    result->has_min = true;

    if (result->min_changed) {
        if (!child.has_min && !node.count) {
            return DB_CORRUPT;
        }

        result->min = child.has_min ? child.min : node.keys[0];
    }

    if (index && child.min_changed && child.has_min) {
        node.keys[index - 1] = child.min;
    }

    status = balance_child(tree, &node, index);

    if (status != DB_OK) {
        return status;
    }

    save_internal(tree, number, page, &node);

    return DB_OK;
}

/* Load siblings before deletion mutates pages. */
static DbStatus load_delete_path(BTree *tree, int64_t key) {
    uint32_t number = tree->root;

    for (unsigned depth = 0; depth < TREE_MAX_DEPTH; depth++) {
        uint8_t *page;
        DbStatus status = get_node(tree, number, &page);

        if (status != DB_OK) {
            return status;
        }

        if (page[0] == PAGE_LEAF) {
            unsigned index = leaf_key_index(tree, page, key);

            return index < read_u16(page + NODE_COUNT) &&
                           read_i64(leaf_cell(tree, page, index)) == key
                       ? DB_OK
                       : DB_NOT_FOUND;
        }

        InternalNode node;

        load_internal(page, &node);

        unsigned index = child_index(&node, key);
        uint8_t *sibling;

        if (index) {
            status = get_node(tree, node.children[index - 1], &sibling);

            if (status != DB_OK) {
                return status;
            }
        }

        if (index < node.count) {
            status = get_node(tree, node.children[index + 1], &sibling);

            if (status != DB_OK) {
                return status;
            }
        }

        number = node.children[index];
    }

    return DB_CORRUPT;
}

static DbStatus collapse_empty_root(BTree *tree) {
    uint8_t *page;
    DbStatus status = get_node(tree, tree->root, &page);

    if (status != DB_OK) {
        return status;
    }

    if (page[0] == PAGE_INTERNAL && !read_u16(page + NODE_COUNT)) {
        uint32_t old = tree->root;

        tree->root = read_u32(page + BTREE_HEADER_SIZE);

        return pager_release(tree->pager, old);
    }

    return DB_OK;
}

DbStatus btree_delete(BTree *tree, int64_t key) {
    DbStatus status = load_delete_path(tree, key);

    if (status != DB_OK) {
        return status;
    }

    DeleteResult result = {0};

    status = delete_node(tree, tree->root, key, &result, 0);

    if (status != DB_OK) {
        return status;
    }

    return collapse_empty_root(tree);
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
                            KeyRange *range);

static DbStatus verify_leaf(BTree *tree, uint32_t number, uint8_t *page, unsigned depth,
                            Verification *check, KeyRange *range) {
    unsigned count = read_u16(page + NODE_COUNT);

    if (depth && count < (leaf_capacity(tree) + 1) / 2) {
        return DB_CORRUPT;
    }

    if (check->has_leaf && (check->next_leaf != number || check->stats.height != depth + 1)) {
        return DB_CORRUPT;
    }

    check->has_leaf = true;
    check->next_leaf = read_u32(page + LEAF_NEXT);
    check->stats.height = depth + 1;
    check->stats.leaves++;
    check->stats.records += count;
    range->present = count != 0;

    for (unsigned i = 0; i < count; i++) {
        uint8_t *entry = leaf_cell(tree, page, i);
        int64_t key = read_i64(entry);

        if (read_u16(entry + LEAF_VALUE_LENGTH) > tree->value_size || (i && key <= range->max)) {
            return DB_CORRUPT;
        }

        if (!i) {
            range->min = key;
        }

        range->max = key;
    }

    return DB_OK;
}

static DbStatus verify_internal(BTree *tree, uint8_t *page, unsigned depth, Verification *check,
                                KeyRange *range) {
    unsigned count = read_u16(page + NODE_COUNT);

    if ((!depth && !count) || (depth && count < BTREE_INTERNAL_MAX / 2)) {
        return DB_CORRUPT;
    }

    InternalNode node;

    load_internal(page, &node);
    range->present = true;

    for (unsigned i = 0; i <= count; i++) {
        KeyRange child = {0};
        DbStatus status = verify_node(tree, node.children[i], depth + 1, check, &child);

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

static DbStatus verify_node(BTree *tree, uint32_t number, unsigned depth, Verification *check,
                            KeyRange *range) {
    if (depth >= TREE_MAX_DEPTH || !number || number >= tree->pager->page_count ||
        check->seen[number]) {
        return DB_CORRUPT;
    }

    check->seen[number] = 1;
    check->stats.nodes++;

    uint8_t *page;
    DbStatus status = get_node(tree, number, &page);

    if (status != DB_OK) {
        return status;
    }

    if (page[0] == PAGE_LEAF) {
        return verify_leaf(tree, number, page, depth, check, range);
    }

    return verify_internal(tree, page, depth, check, range);
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

    unsigned count = read_u16(page + NODE_COUNT);

    fprintf(output, "%*spage %u %s (%u keys)", (int)(depth * 2), "", number,
            page[0] == PAGE_LEAF ? "leaf" : "internal", count);

    if (page[0] == PAGE_LEAF) {
        fprintf(output, " next=%u:", read_u32(page + LEAF_NEXT));

        for (unsigned i = 0; i < count; i++) {
            fprintf(output, " %" PRId64, read_i64(leaf_cell(tree, page, i)));
        }

        fputc('\n', output);

        return ferror(output) ? DB_IO : DB_OK;
    }

    InternalNode node;

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

static DbStatus scan_leaf(BTree *tree, uint8_t *page, BTreeVisitor visitor, void *context) {
    if (page[0] != PAGE_LEAF) {
        return DB_CORRUPT;
    }

    for (unsigned i = 0; i < read_u16(page + NODE_COUNT); i++) {
        uint8_t *entry = leaf_cell(tree, page, i);
        uint16_t size = read_u16(entry + LEAF_VALUE_LENGTH);

        if (size > tree->value_size) {
            return DB_CORRUPT;
        }

        DbStatus status = visitor(read_i64(entry), entry + LEAF_VALUE_DATA, size, context);

        if (status != DB_OK) {
            return status;
        }
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

        status = scan_leaf(tree, page, visitor, context);

        if (status != DB_OK) {
            return status;
        }

        number = read_u32(page + LEAF_NEXT);
    }

    return DB_OK;
}
