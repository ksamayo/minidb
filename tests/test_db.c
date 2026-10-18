#include "db.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    CATALOG_NEXT_PAGE_OFFSET = 4,
    CATALOG_ROOT_OFFSET = 8,
    CATALOG_COLUMN_COUNT_OFFSET = 12,
    CATALOG_TABLE_NAME_OFFSET = 16,
    CATALOG_COLUMNS_OFFSET = 48,
    CATALOG_COLUMN_TYPE_OFFSET = 32,
    CATALOG_COLUMN_SIZE = 33
};

enum {
    CATALOG_WRONG_PAGE_TYPE,
    CATALOG_CYCLE,
    CATALOG_INVALID_NEXT_PAGE,
    CATALOG_MISSING_ROOT,
    CATALOG_ROOT_IS_CATALOG,
    CATALOG_TOO_MANY_COLUMNS,
    CATALOG_UNTERMINATED_TABLE_NAME,
    CATALOG_UNTERMINATED_COLUMN_NAME,
    CATALOG_INVALID_PRIMARY_KEY_TYPE,
    CATALOG_UNKNOWN_COLUMN_TYPE,
    CATALOG_DUPLICATE_TABLE_NAME,
    CATALOG_SHARED_TREE_ROOT,
    CATALOG_CORRUPTION_COUNT
};

static DbStatus count_rows(const Value *values, unsigned column_count, void *context) {
    unsigned *row_count = context;

    assert(column_count == 3 && values[0].type == TYPE_INT);
    (*row_count)++;
    return DB_OK;
}

static void create_catalog_snapshot(Database *database, const char *database_path,
                                    uint32_t *catalog_page_numbers,
                                    uint8_t original_catalog_pages[2][PAGE_SIZE]) {
    Schema schema = {2, {{"id", TYPE_INT}, {"name", TYPE_TEXT}}};

    assert(db_open(database, database_path) == DB_OK);
    assert(db_create_table(database, "users", &schema) == DB_OK);
    assert(db_create_table(database, "products", &schema) == DB_OK);
    catalog_page_numbers[0] = database->tables[0].catalog_page;
    catalog_page_numbers[1] = database->tables[1].catalog_page;

    for (unsigned page_index = 0; page_index < 2; page_index++) {
        uint8_t *catalog_page;
        assert(pager_get(&database->pager, catalog_page_numbers[page_index], &catalog_page) ==
               DB_OK);
        memcpy(original_catalog_pages[page_index], catalog_page, PAGE_SIZE);
    }

    assert(db_close(database) == DB_OK);
}

static void corrupt_catalog_page(unsigned corruption_case, uint8_t *corrupted_page,
                                 const uint32_t *catalog_page_numbers,
                                 const uint8_t *first_catalog_page) {
    switch (corruption_case) {
        case CATALOG_WRONG_PAGE_TYPE: {
            corrupted_page[0] = PAGE_LEAF;
            break;
        }
        case CATALOG_CYCLE: {
            write_u32(corrupted_page + CATALOG_NEXT_PAGE_OFFSET, catalog_page_numbers[0]);
            break;
        }
        case CATALOG_INVALID_NEXT_PAGE: {
            write_u32(corrupted_page + CATALOG_NEXT_PAGE_OFFSET, UINT32_MAX);
            break;
        }
        case CATALOG_MISSING_ROOT: {
            write_u32(corrupted_page + CATALOG_ROOT_OFFSET, 0);
            break;
        }
        case CATALOG_ROOT_IS_CATALOG: {
            write_u32(corrupted_page + CATALOG_ROOT_OFFSET, catalog_page_numbers[0]);
            break;
        }
        case CATALOG_TOO_MANY_COLUMNS: {
            write_u16(corrupted_page + CATALOG_COLUMN_COUNT_OFFSET, MAX_COLUMNS + 1);
            break;
        }
        case CATALOG_UNTERMINATED_TABLE_NAME: {
            memset(corrupted_page + CATALOG_TABLE_NAME_OFFSET, 'x', MAX_NAME + 1);
            break;
        }
        case CATALOG_UNTERMINATED_COLUMN_NAME: {
            memset(corrupted_page + CATALOG_COLUMNS_OFFSET, 'x', MAX_NAME + 1);
            break;
        }
        case CATALOG_INVALID_PRIMARY_KEY_TYPE: {
            corrupted_page[CATALOG_COLUMNS_OFFSET + CATALOG_COLUMN_TYPE_OFFSET] = TYPE_TEXT;
            break;
        }
        case CATALOG_UNKNOWN_COLUMN_TYPE: {
            corrupted_page[CATALOG_COLUMNS_OFFSET + CATALOG_COLUMN_SIZE +
                           CATALOG_COLUMN_TYPE_OFFSET] = 99;
            break;
        }
        case CATALOG_DUPLICATE_TABLE_NAME: {
            memcpy(corrupted_page + CATALOG_TABLE_NAME_OFFSET,
                   first_catalog_page + CATALOG_TABLE_NAME_OFFSET, MAX_NAME + 1);
            break;
        }
        case CATALOG_SHARED_TREE_ROOT: {
            write_u32(corrupted_page + CATALOG_ROOT_OFFSET,
                      read_u32(first_catalog_page + CATALOG_ROOT_OFFSET));
            break;
        }
    }
}

static void verify_repaired_catalog(Database *database, const char *database_path) {

    assert(db_open(database, database_path) == DB_OK && database->table_count == 2);
    assert(db_table(database, "users") && db_table(database, "products"));
    assert(db_close(database) == DB_OK);
}

static void test_invalid_catalog(void) {
    char database_path[] = "/tmp/minidb-catalog-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0 && close(file_descriptor) == 0);

    Database database;
    uint32_t catalog_page_numbers[2];
    uint8_t original_catalog_pages[2][PAGE_SIZE];
    uint8_t corrupted_page[PAGE_SIZE];
    create_catalog_snapshot(&database, database_path, catalog_page_numbers, original_catalog_pages);

    file_descriptor = open(database_path, O_RDWR);
    assert(file_descriptor >= 0);

    for (unsigned corruption_case = 0; corruption_case < CATALOG_CORRUPTION_COUNT;
         corruption_case++) {
        unsigned page_index = corruption_case >= CATALOG_DUPLICATE_TABLE_NAME ? 1 : 0;
        memcpy(corrupted_page, original_catalog_pages[page_index], PAGE_SIZE);
        corrupt_catalog_page(corruption_case, corrupted_page, catalog_page_numbers,
                             original_catalog_pages[0]);

        off_t page_offset = (off_t)catalog_page_numbers[page_index] * PAGE_SIZE;
        assert(pwrite(file_descriptor, corrupted_page, PAGE_SIZE, page_offset) == PAGE_SIZE);
        assert(db_open(&database, database_path) == DB_CORRUPT);
        assert(pwrite(file_descriptor, original_catalog_pages[page_index], PAGE_SIZE,
                      page_offset) == PAGE_SIZE);
        verify_repaired_catalog(&database, database_path);
    }

    assert(close(file_descriptor) == 0 && unlink(database_path) == 0);
}

static void insert_test_rows(Table *users_table, Table *products_table) {
    Value row_values[MAX_COLUMNS] = {{.type = TYPE_INT}, {.type = TYPE_TEXT}, {.type = TYPE_INT}};

    for (int row_id = 0; row_id < 300; row_id++) {
        row_values[0].as.integer = row_id;
        snprintf(row_values[1].as.text, sizeof(row_values[1].as.text), "user%d", row_id);
        row_values[2].as.integer = 22;
        assert(db_insert(users_table, row_values, 3) == DB_OK);

        if (row_id < 30) {
            strcpy(row_values[1].as.text, "product");
            assert(db_insert(products_table, row_values, 3) == DB_OK);
        }
    }

    assert(db_insert(users_table, row_values, 3) == DB_DUPLICATE);
}

static void test_row_updates_and_deletions(Table *users_table) {
    Value updated_age = {.type = TYPE_INT, .as.integer = 23};

    assert(db_update(users_table, 1, "age", &updated_age) == DB_OK);
    assert(db_update(users_table, 1, "id", &updated_age) == DB_INVALID);

    for (int row_id = 0; row_id < 300; row_id += 3) {
        assert(db_delete(users_table, row_id) == DB_OK);
    }
}

static void populate_tables(Database *database) {
    Schema schema = {3, {{"id", TYPE_INT}, {"name", TYPE_TEXT}, {"age", TYPE_INT}}};

    assert(db_create_table(database, "users", &schema) == DB_OK);
    assert(db_create_table(database, "products", &schema) == DB_OK);
    assert(db_create_table(database, "users", &schema) == DB_DUPLICATE);

    Table *users_table = db_table(database, "users");
    Table *products_table = db_table(database, "products");
    assert(users_table && products_table && users_table->tree.root != products_table->tree.root);

    insert_test_rows(users_table, products_table);
    test_row_updates_and_deletions(users_table);
}

static void verify_persisted_rows(Table *users_table, Table *products_table) {
    Value row_values[MAX_COLUMNS];

    for (int row_id = 0; row_id < 300; row_id++) {
        DbStatus status = db_get(users_table, row_id, row_values);
        assert(status == (row_id % 3 ? DB_OK : DB_NOT_FOUND));

        if (status == DB_OK) {
            assert(row_values[2].as.integer == (row_id == 1 ? 23 : 22));
        }

        if (row_id < 30) {
            assert(db_get(products_table, row_id, row_values) == DB_OK);
            assert(!strcmp(row_values[1].as.text, "product") && row_values[2].as.integer == 22);
        }
    }
}

static void verify_scan_and_delete_remaining_rows(Table *users_table) {
    unsigned row_count = 0;
    assert(db_scan(users_table, count_rows, &row_count) == DB_OK && row_count == 200);

    for (int row_id = 0; row_id < 300; row_id++) {
        if (row_id % 3) {
            assert(db_delete(users_table, row_id) == DB_OK);
        }
    }
}

static void verify_and_delete_rows(Database *database) {
    Table *users_table = db_table(database, "users");
    Table *products_table = db_table(database, "products");

    assert(users_table && products_table && users_table->schema.count == 3);
    assert(btree_verify(&users_table->tree, NULL) == DB_OK);
    assert(btree_verify(&products_table->tree, NULL) == DB_OK);

    verify_persisted_rows(users_table, products_table);
    verify_scan_and_delete_remaining_rows(users_table);
}

static void test_table_persistence(void) {
    char database_path[] = "/tmp/minidb-db-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0 && close(file_descriptor) == 0);

    Database database;
    assert(db_open(&database, database_path) == DB_OK);
    populate_tables(&database);
    assert(db_close(&database) == DB_OK);

    assert(db_open(&database, database_path) == DB_OK && database.table_count == 2);
    verify_and_delete_rows(&database);
    assert(db_close(&database) == DB_OK);

    assert(unlink(database_path) == 0);
}

int main(void) {
    test_table_persistence();
    test_invalid_catalog();

    puts("db: OK");
    return 0;
}
