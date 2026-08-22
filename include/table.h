#ifndef MINIDB_TABLE_H
#define MINIDB_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define TABLE_CAPACITY 1024u
#define ROW_NAME_SIZE 64u

typedef struct {
    int64_t id;
    char name[ROW_NAME_SIZE];
    int64_t age;
} Row;

typedef struct {
    Row rows[TABLE_CAPACITY];
    size_t row_count;
} MemoryTable;

typedef enum {
    TABLE_OK,
    TABLE_FULL,
    TABLE_DUPLICATE,
    TABLE_NOT_FOUND,
    TABLE_IO,
    TABLE_INVALID
} TableStatus;

void table_init(MemoryTable *table);
TableStatus table_insert(MemoryTable *table, const Row *row);
TableStatus table_select(const MemoryTable *table, FILE *output);
TableStatus table_delete(MemoryTable *table, int64_t id);
#endif
