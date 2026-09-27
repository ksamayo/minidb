#include "db.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DbStatus count_rows(const Value *values, unsigned column_count, void *context) {
    unsigned *row_count = context;

    assert(column_count == 3 && values[0].type == TYPE_INT);
    (*row_count)++;
    return DB_OK;
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

    puts("db: OK");
    return 0;
}
