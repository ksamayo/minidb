#ifndef MINIDB_DB_H
#define MINIDB_DB_H

#include "btree.h"
#include "record.h"

typedef struct {
    char name[MAX_NAME + 1];
    Schema schema;
    uint32_t catalog_page;
    BTree tree;
} Table;

typedef struct {
    Pager pager;
    Table *tables;
    size_t table_count, table_capacity;
} Database;

typedef DbStatus (*DbVisitor)(const Value *values, unsigned count, void *context);

DbStatus db_open(Database *db, const char *path);
DbStatus db_close(Database *db);
/* Durability requires sync or close; no journal. */
DbStatus db_sync(Database *db);
DbStatus db_create_table(Database *db, const char *name, const Schema *schema);

/* Invalidated by table creation or close. */
Table *db_table(Database *db, const char *name);
DbStatus db_insert(Table *table, const Value *values, unsigned count);
DbStatus db_get(Table *table, int64_t id, Value *values);
DbStatus db_update(Table *table, int64_t id, const char *column, const Value *value);
DbStatus db_delete(Table *table, int64_t id);
DbStatus db_scan(Table *table, DbVisitor visitor, void *context);

#endif
