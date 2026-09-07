#ifndef MINIDB_RECORD_H
#define MINIDB_RECORD_H

#include "pager.h"
#include <stdio.h>

#define MAX_COLUMNS 8u
#define MAX_NAME 31u
#define MAX_TEXT 255u
#define MAX_RECORD_SIZE (8u + (MAX_COLUMNS - 1u) * (MAX_TEXT + 1u))

typedef enum {
    TYPE_INT = 1,
    TYPE_TEXT = 2
} ColumnType;

typedef struct {
    char name[MAX_NAME + 1];
    ColumnType type;
} Column;

typedef struct {
    unsigned count;
    Column columns[MAX_COLUMNS];
} Schema;

typedef struct {
    ColumnType type;
    union {
        int64_t integer;
        char text[MAX_TEXT + 1];
    } as;
} Value;

bool record_valid_name(const char *name);
DbStatus schema_validate(const Schema *schema);
uint16_t record_max_size(const Schema *schema);
DbStatus record_encode(const Schema *schema, const Value *values, unsigned count, uint8_t *buffer,
                       size_t capacity, uint16_t *length);
DbStatus record_decode(const Schema *schema, const uint8_t *buffer, size_t length, Value *values);
DbStatus record_print(const Schema *schema, const Value *values, FILE *output);

#endif
