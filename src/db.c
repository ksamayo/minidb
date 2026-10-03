#include "db.h"
#include "parser.h"

#include <stdlib.h>
#include <string.h>

enum {
    CAT_NEXT = 4,
    CAT_ROOT = 8,
    CAT_COUNT = 12,
    CAT_NAME = 16,
    CAT_COLUMNS = 48
};

static DbStatus reserve_tables(Database *db) {
    if (db->table_count < db->table_capacity) {
        return DB_OK;
    }
    size_t capacity = db->table_capacity ? db->table_capacity * 2 : 4;
    if (capacity < db->table_capacity || capacity > SIZE_MAX / sizeof(Table)) {
        return DB_NOMEM;
    }
    Table *tables = realloc(db->tables, capacity * sizeof(*tables));
    if (!tables) {
        return DB_NOMEM;
    }
    db->tables = tables;
    db->table_capacity = capacity;
    return DB_OK;
}

Table *db_table(Database *db, const char *name) {
    for (size_t i = 0; i < db->table_count; i++) {
        if (!strcmp(db->tables[i].name, name)) {
            return &db->tables[i];
        }
    }
    return NULL;
}

DbStatus db_open(Database *db, const char *path) {
    memset(db, 0, sizeof(*db));
    DbStatus status = pager_open(&db->pager, path);
    if (status != DB_OK) {
        return status;
    }
    uint32_t number = read_u32(db->pager.header + HEADER_CATALOG);
    uint8_t *seen = calloc(db->pager.page_count, 1);
    if (!seen) {
        pager_close(&db->pager);
        return DB_NOMEM;
    }
    while (number) {
        status = DB_CORRUPT;
        if (number >= db->pager.page_count || seen[number]) {
            goto fail;
        }
        seen[number] = 1;
        uint8_t *page;
        status = pager_get(&db->pager, number, &page);
        if (status != DB_OK) {
            goto fail;
        }
        status = DB_CORRUPT;
        if (page[0] != PAGE_CATALOG || !memchr(page + CAT_NAME, 0, MAX_NAME + 1)) {
            goto fail;
        }
        Table table = {0};
        table.catalog_page = number;
        memcpy(table.name, page + CAT_NAME, MAX_NAME + 1);
        table.schema.count = read_u16(page + CAT_COUNT);
        if (!record_valid_name(table.name) || db_table(db, table.name) || !table.schema.count ||
            table.schema.count > MAX_COLUMNS) {
            goto fail;
        }
        for (unsigned i = 0; i < table.schema.count; i++) {
            const uint8_t *col = page + CAT_COLUMNS + i * 33;
            if (!memchr(col, 0, MAX_NAME + 1)) {
                goto fail;
            }
            memcpy(table.schema.columns[i].name, col, MAX_NAME + 1);
            table.schema.columns[i].type = col[32];
        }
        if (schema_validate(&table.schema) != DB_OK) {
            goto fail;
        }
        status = btree_open(&table.tree, &db->pager, read_u32(page + CAT_ROOT),
                            record_max_size(&table.schema));
        if (status != DB_OK) {
            goto fail;
        }
        for (size_t i = 0; i < db->table_count; i++) {
            if (db->tables[i].tree.root == table.tree.root) {
                status = DB_CORRUPT;
                goto fail;
            }
        }
        status = reserve_tables(db);
        if (status != DB_OK) {
            goto fail;
        }
        db->tables[db->table_count++] = table;
        number = read_u32(page + CAT_NEXT);
    }
    free(seen);
    return DB_OK;
fail:
    free(seen);
    free(db->tables);
    db->tables = NULL;
    db->table_count = db->table_capacity = 0;
    pager_close(&db->pager);
    return status;
}

DbStatus db_sync(Database *db) {
    return pager_sync(&db->pager);
}

DbStatus db_close(Database *db) {
    DbStatus status = pager_close(&db->pager);
    free(db->tables);
    db->tables = NULL;
    db->table_count = db->table_capacity = 0;
    return status;
}

DbStatus db_create_table(Database *db, const char *name, const Schema *schema) {
    if (!record_valid_name(name) || schema_validate(schema) != DB_OK) {
        return DB_INVALID;
    }
    if (db_table(db, name)) {
        return DB_DUPLICATE;
    }
    DbStatus status = reserve_tables(db);
    if (status != DB_OK) {
        return status;
    }
    uint32_t catalog;
    uint8_t *page;
    status = pager_allocate(&db->pager, &catalog, &page);
    if (status != DB_OK) {
        return status;
    }
    Table table = {0};
    status = btree_create(&table.tree, &db->pager, record_max_size(schema));
    if (status != DB_OK) {
        pager_release(&db->pager, catalog);
        return status;
    }
    table.schema = *schema;
    table.catalog_page = catalog;
    strcpy(table.name, name);
    page[0] = PAGE_CATALOG;
    write_u32(page + CAT_ROOT, table.tree.root);
    write_u16(page + CAT_COUNT, (uint16_t)schema->count);
    memcpy(page + CAT_NAME, table.name, MAX_NAME + 1);
    for (unsigned i = 0; i < schema->count; i++) {
        memcpy(page + CAT_COLUMNS + i * 33, schema->columns[i].name, MAX_NAME + 1);
        page[CAT_COLUMNS + i * 33 + 32] = (uint8_t)schema->columns[i].type;
    }
    if (db->table_count) {
        Table *last = &db->tables[db->table_count - 1];
        uint8_t *previous;
        status = pager_get(&db->pager, last->catalog_page, &previous);
        if (status != DB_OK) {
            return status;
        }
        write_u32(previous + CAT_NEXT, catalog);
        pager_dirty(&db->pager, last->catalog_page);
    } else {
        status = pager_set_catalog(&db->pager, catalog);
        if (status != DB_OK) {
            return status;
        }
    }
    db->tables[db->table_count++] = table;
    return DB_OK;
}

static DbStatus save_root(Table *table) {
    uint8_t *page;
    DbStatus status = pager_get(table->tree.pager, table->catalog_page, &page);
    if (status != DB_OK) {
        return status;
    }
    write_u32(page + CAT_ROOT, table->tree.root);
    pager_dirty(table->tree.pager, table->catalog_page);
    return DB_OK;
}

DbStatus db_insert(Table *table, const Value *values, unsigned count) {
    uint8_t buffer[MAX_RECORD_SIZE];
    uint16_t length;
    DbStatus status = record_encode(&table->schema, values, count, buffer, sizeof(buffer), &length);
    if (status != DB_OK) {
        return status;
    }
    status = btree_insert(&table->tree, values[0].as.integer, buffer, length);
    return status == DB_OK ? save_root(table) : status;
}

DbStatus db_get(Table *table, int64_t id, Value *values) {
    uint8_t buffer[MAX_RECORD_SIZE];
    uint16_t length;
    DbStatus status = btree_find(&table->tree, id, buffer, sizeof(buffer), &length);
    if (status != DB_OK) {
        return status;
    }
    status = record_decode(&table->schema, buffer, length, values);
    if (status == DB_OK && values[0].as.integer != id) {
        return DB_CORRUPT;
    }
    return status;
}

DbStatus db_update(Table *table, int64_t id, const char *column, const Value *value) {
    unsigned index;
    for (index = 0; index < table->schema.count; index++) {
        if (!strcmp(table->schema.columns[index].name, column)) {
            break;
        }
    }
    if (!index || index == table->schema.count ||
        table->schema.columns[index].type != value->type) {
        return DB_INVALID;
    }
    Value values[MAX_COLUMNS];
    DbStatus status = db_get(table, id, values);
    if (status != DB_OK) {
        return status;
    }
    values[index] = *value;
    uint8_t buffer[MAX_RECORD_SIZE];
    uint16_t length;
    status =
        record_encode(&table->schema, values, table->schema.count, buffer, sizeof(buffer), &length);
    return status == DB_OK ? btree_update(&table->tree, id, buffer, length) : status;
}

DbStatus db_delete(Table *table, int64_t id) {
    DbStatus status = btree_delete(&table->tree, id);
    return status == DB_OK ? save_root(table) : status;
}

typedef struct {
    Table *table;
    DbVisitor visitor;
    void *context;
} ScanContext;

static DbStatus scan_record(int64_t key, const uint8_t *record, uint16_t length, void *p) {
    ScanContext *scan = p;
    Value values[MAX_COLUMNS];
    DbStatus status = record_decode(&scan->table->schema, record, length, values);
    if (status != DB_OK) {
        return status;
    }
    if (values[0].as.integer != key) {
        return DB_CORRUPT;
    }
    return scan->visitor(values, scan->table->schema.count, scan->context);
}

DbStatus db_scan(Table *table, DbVisitor visitor, void *context) {
    if (!visitor) {
        return DB_INVALID;
    }
    ScanContext scan = {table, visitor, context};
    return btree_scan(&table->tree, scan_record, &scan);
}

typedef struct {
    const Schema *schema;
    FILE *output;
} PrintContext;

static DbStatus print_row(const Value *values, unsigned count, void *context) {
    (void)count;
    PrintContext *print = context;
    return record_print(print->schema, values, print->output);
}

DbStatus db_execute(Database *db, const Statement *statement, FILE *output) {
    DbStatus status;
    if (statement->type == STMT_CREATE) {
        status = db_create_table(db, statement->table, &statement->schema);
        if (status == DB_OK) {
            fputs("OK\n", output);
        }
        return status;
    }
    Table *table = db_table(db, statement->table);
    if (!table) {
        return DB_NOT_FOUND;
    }
    if (statement->type == STMT_INSERT) {
        status = db_insert(table, statement->values, statement->value_count);
        if (status == DB_OK) {
            fputs("1 row inserted\n", output);
        }
        return status;
    }
    if (statement->type == STMT_SELECT) {
        if (statement->has_where) {
            Value values[MAX_COLUMNS];
            status = db_get(table, statement->id, values);
            if (status == DB_NOT_FOUND) {
                return DB_OK;
            }
            return status == DB_OK ? record_print(&table->schema, values, output) : status;
        }
        PrintContext print = {&table->schema, output};
        return db_scan(table, print_row, &print);
    }
    return DB_INVALID;
}
