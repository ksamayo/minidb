#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static DbStatus verify_scanned_row(int64_t key, const uint8_t *value, uint16_t value_length,
                                   void *context) {
    int64_t *expected_key = context;

    assert(key == *expected_key);
    assert(value_length == 8 && read_i64(value) == key);
    (*expected_key)++;
    return DB_OK;
}

static void test_insertions(BTree *tree, int record_count) {
    uint8_t value[32];

    for (int insertion_index = 0; insertion_index < record_count; insertion_index++) {
        int64_t key = ((int64_t)insertion_index * 7919) % record_count - record_count / 2;
        write_u64(value, (uint64_t)key);
        assert(btree_insert(tree, key, value, 8) == DB_OK);
    }

    assert(btree_insert(tree, 0, value, 8) == DB_DUPLICATE);
    assert(btree_find(tree, record_count, value, sizeof(value), NULL) == DB_NOT_FOUND);

    uint8_t *root_page;
    uint8_t *child_page;
    assert(pager_get(tree->pager, tree->root, &root_page) == DB_OK &&
           root_page[0] == PAGE_INTERNAL);
    assert(pager_get(tree->pager, read_u32(root_page + BTREE_HEADER_SIZE), &child_page) == DB_OK);
    assert(child_page[0] == PAGE_INTERNAL);

}

static void verify_reopened_rows(BTree *tree, int record_count) {
    uint8_t value[32];

    for (int64_t key = -record_count / 2; key < record_count / 2; key++) {
        uint16_t value_length;
        assert(btree_find(tree, key, value, sizeof(value), &value_length) == DB_OK);
        assert(value_length == 8 && read_i64(value) == key);
    }

    int64_t expected_key = -record_count / 2;
    assert(btree_scan(tree, verify_scanned_row, &expected_key) == DB_OK &&
           expected_key == record_count / 2);
}

static void test_tree_persistence(void) {
    char database_path[] = "/tmp/minidb-btree-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0 && close(file_descriptor) == 0);

    Pager pager;
    BTree tree;
    assert(pager_open(&pager, database_path) == DB_OK);
    assert(btree_create(&tree, &pager, 512) == DB_OK);

    const int record_count = 2400;
    test_insertions(&tree, record_count);
    assert(pager_set_catalog(&pager, tree.root) == DB_OK);
    assert(pager_close(&pager) == DB_OK);

    assert(pager_open(&pager, database_path) == DB_OK);
    assert(btree_open(&tree, &pager, read_u32(pager.header + HEADER_CATALOG), 512) == DB_OK);
    verify_reopened_rows(&tree, record_count);

    assert(pager_close(&pager) == DB_OK);
    assert(unlink(database_path) == 0);
}

int main(void) {
    test_tree_persistence();

    puts("btree: OK");
    return 0;
}
