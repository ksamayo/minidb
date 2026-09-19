#ifndef MINIDB_BTREE_H
#define MINIDB_BTREE_H

#include "pager.h"
#include <stdio.h>

#define BTREE_HEADER_SIZE 16u
#define BTREE_MAX_VALUE 2030u
#define BTREE_INTERNAL_MAX ((PAGE_SIZE - BTREE_HEADER_SIZE - 4u) / 12u)

typedef struct {
    Pager *pager;
    uint32_t root;
    uint16_t value_size;
} BTree;

/* Values are borrowed until the callback returns. */
typedef DbStatus (*BTreeVisitor)(int64_t key, const uint8_t *value, uint16_t length, void *context);

typedef struct {
    uint64_t records;
    uint32_t nodes, leaves;
    unsigned height;
} BTreeStats;

DbStatus btree_create(BTree *tree, Pager *pager, uint16_t value_size);
DbStatus btree_open(BTree *tree, Pager *pager, uint32_t root, uint16_t value_size);
DbStatus btree_insert(BTree *tree, int64_t key, const void *value, uint16_t length);
DbStatus btree_find(BTree *tree, int64_t key, void *value, size_t capacity, uint16_t *length);
DbStatus btree_scan(BTree *tree, BTreeVisitor visitor, void *context);
DbStatus btree_update(BTree *tree, int64_t key, const void *value, uint16_t length);
DbStatus btree_delete(BTree *tree, int64_t key);
DbStatus btree_verify(BTree *tree, BTreeStats *stats);
DbStatus btree_dump(BTree *tree, FILE *output);

#endif
