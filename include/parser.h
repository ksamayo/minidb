#ifndef MINIDB_PARSER_H
#define MINIDB_PARSER_H

#include "record.h"

typedef enum {
    STMT_CREATE,
    STMT_INSERT,
    STMT_SELECT
} StatementType;

typedef struct Statement {
    StatementType type;
    char table[MAX_NAME + 1];
    Schema schema;
    Value values[MAX_COLUMNS];
    unsigned value_count;
    bool has_where;
    int64_t id;
} Statement;

DbStatus parser_parse(const char *sql, Statement *statement, char *error, size_t error_size);

#endif
