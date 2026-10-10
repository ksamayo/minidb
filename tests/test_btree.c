#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static size_t calloc_budget = SIZE_MAX;
static size_t malloc_budget = SIZE_MAX;
void *__real_calloc(size_t count, size_t size);
void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size) {
    if (!malloc_budget) {
        return NULL;
    }

    if (malloc_budget != SIZE_MAX) {
        malloc_budget--;
    }

    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
    if (!calloc_budget) {
        return NULL;
    }

    if (calloc_budget != SIZE_MAX) {
        calloc_budget--;
    }

    return __real_calloc(count, size);
}

static DbStatus verify_scanned_row(int64_t key, const uint8_t *value, uint16_t value_length,
                                   void *context) {
    int64_t *expected_key = context;

    assert(key == *expected_key);
    assert(value_length == 8 && read_i64(value) == key);
    (*expected_key)++;
    return DB_OK;
}

static void verify_page_ownership(BTree *tree) {
    BTreeStats tree_statistics;
    assert(btree_verify(tree, &tree_statistics) == DB_OK);

    Pager *pager = tree->pager;
    uint8_t *visited_pages = calloc(pager->page_count, 1);
    assert(visited_pages);

    uint32_t free_page_number = read_u32(pager->header + HEADER_FREE);
    uint32_t free_page_count = 0;

    while (free_page_number) {
        assert(free_page_number < pager->page_count && !visited_pages[free_page_number]);
        visited_pages[free_page_number] = 1;

        uint8_t *page;
        assert(pager_get(pager, free_page_number, &page) == DB_OK && page[0] == PAGE_FREE);
        free_page_count++;
        free_page_number = read_u32(page + 4);
    }

    assert(tree_statistics.nodes + free_page_count + 1 == pager->page_count);
    free(visited_pages);
}

static void insert_capacity_test_keys(BTree *tree, uint16_t value_capacity, const uint8_t *value) {
    for (int insertion_index = 0; insertion_index < 400; insertion_index++) {
        int key = (insertion_index * 113) % 400 - 200;
        assert(btree_insert(tree, key, value, value_capacity) == DB_OK);
    }

    assert(btree_insert(tree, INT64_MIN, value, 8) == DB_OK);
    assert(btree_insert(tree, INT64_MAX, value, 8) == DB_OK);
    assert(btree_verify(tree, NULL) == DB_OK);
}

static void delete_capacity_test_keys(BTree *tree) {
    assert(btree_delete(tree, INT64_MIN) == DB_OK);
    assert(btree_delete(tree, INT64_MAX) == DB_OK);

    for (int deletion_index = 0; deletion_index < 400; deletion_index++) {
        int key = (deletion_index * 113) % 400 - 200;
        assert(btree_delete(tree, key) == DB_OK);

        if (deletion_index % 17 == 0) {
            assert(btree_verify(tree, NULL) == DB_OK);
        }
    }
}

static void test_cyclic_leaf_link(BTree *tree) {
    uint8_t *root_page;
    assert(pager_get(tree->pager, tree->root, &root_page) == DB_OK);

    write_u32(root_page + 4, tree->root);
    assert(btree_verify(tree, NULL) == DB_CORRUPT);
    write_u32(root_page + 4, 0);
}

static void test_value_capacity(uint16_t value_capacity) {
    char database_path[] = "/tmp/minidb-width-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0 && close(file_descriptor) == 0);

    Pager pager;
    BTree tree;
    uint8_t value[BTREE_MAX_VALUE] = {0};
    assert(pager_open(&pager, database_path) == DB_OK);
    assert(btree_create(&tree, &pager, value_capacity) == DB_OK);

    insert_capacity_test_keys(&tree, value_capacity, value);
    delete_capacity_test_keys(&tree);

    BTreeStats tree_statistics;
    assert(btree_verify(&tree, &tree_statistics) == DB_OK && tree_statistics.records == 0);
    test_cyclic_leaf_link(&tree);
    verify_page_ownership(&tree);
    assert(pager_close(&pager) == DB_OK && unlink(database_path) == 0);
}

static void test_capacities(void) {
    const uint16_t value_capacities[] = {8, 256, 1800, BTREE_MAX_VALUE};

    for (unsigned capacity_index = 0;
         capacity_index < sizeof(value_capacities) / sizeof(value_capacities[0]);
         capacity_index++) {
        test_value_capacity(value_capacities[capacity_index]);
    }
}

static void test_root_allocation_failure(BTree *tree, uint8_t *value) {

    calloc_budget = 1;
    assert(btree_insert(tree, 2, value, 8) == DB_NOMEM);
    calloc_budget = SIZE_MAX;

    assert(btree_verify(tree, NULL) == DB_OK);
    assert(btree_find(tree, 0, value, BTREE_MAX_VALUE, NULL) == DB_OK);
    assert(btree_find(tree, 1, value, BTREE_MAX_VALUE, NULL) == DB_OK);
    assert(btree_find(tree, 2, value, BTREE_MAX_VALUE, NULL) == DB_NOT_FOUND);
}

static void test_root_split_retry(BTree *tree, const uint8_t *value) {
    assert(btree_verify(tree, NULL) == DB_OK);
    assert(btree_insert(tree, 2, value, 8) == DB_OK);
    assert(btree_verify(tree, NULL) == DB_OK);
    verify_page_ownership(tree);
}

static void test_sibling_allocation_failure(BTree *tree, uint8_t *value) {

    malloc_budget = 1;
    assert(btree_delete(tree, 0) == DB_NOMEM);
    malloc_budget = SIZE_MAX;

    assert(btree_verify(tree, NULL) == DB_OK);
    assert(btree_find(tree, 0, value, BTREE_MAX_VALUE, NULL) == DB_OK);
    assert(btree_delete(tree, 0) == DB_OK);
    verify_page_ownership(tree);
}

static void test_allocation_failure(void) {
    char database_path[] = "/tmp/minidb-oom-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0 && close(file_descriptor) == 0);

    Pager pager;
    BTree tree;
    uint8_t value[BTREE_MAX_VALUE] = {0};
    assert(pager_open(&pager, database_path) == DB_OK);
    assert(btree_create(&tree, &pager, sizeof(value)) == DB_OK);
    assert(btree_insert(&tree, 0, value, 8) == DB_OK);
    assert(btree_insert(&tree, 1, value, 8) == DB_OK);
    test_root_allocation_failure(&tree, value);

    assert(pager_set_catalog(&pager, tree.root) == DB_OK);
    assert(pager_close(&pager) == DB_OK);
    assert(pager_open(&pager, database_path) == DB_OK);
    assert(btree_open(&tree, &pager, read_u32(pager.header + HEADER_CATALOG), sizeof(value)) ==
           DB_OK);
    test_root_split_retry(&tree, value);

    assert(pager_set_catalog(&pager, tree.root) == DB_OK);
    assert(pager_close(&pager) == DB_OK && pager_open(&pager, database_path) == DB_OK);
    assert(btree_open(&tree, &pager, read_u32(pager.header + HEADER_CATALOG), sizeof(value)) ==
           DB_OK);
    test_sibling_allocation_failure(&tree, value);
    assert(pager_close(&pager) == DB_OK && unlink(database_path) == 0);
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

    BTreeStats tree_statistics;
    assert(btree_verify(tree, &tree_statistics) == DB_OK &&
           tree_statistics.records == (unsigned)record_count);
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
    assert(btree_delete(tree, record_count) == DB_NOT_FOUND);
}

static void delete_all_keys(BTree *tree, const char *database_path, int record_count) {
    uint8_t value[32];
    BTreeStats tree_statistics;

    for (int deletion_index = 0; deletion_index < record_count; deletion_index++) {
        int64_t key = ((int64_t)deletion_index * 7919) % record_count - record_count / 2;
        assert(btree_delete(tree, key) == DB_OK);
        assert(btree_find(tree, key, value, sizeof(value), NULL) == DB_NOT_FOUND);

        if (deletion_index < 100 || deletion_index % 4096 == 0 ||
            deletion_index == record_count - 1) {
            assert(btree_verify(tree, &tree_statistics) == DB_OK);
            assert(tree_statistics.records == (unsigned)(record_count - deletion_index - 1));
        }

        if (deletion_index == record_count / 2) {
            assert(pager_set_catalog(tree->pager, tree->root) == DB_OK);
            assert(pager_close(tree->pager) == DB_OK);
            assert(pager_open(tree->pager, database_path) == DB_OK);
            assert(btree_open(tree, tree->pager, read_u32(tree->pager->header + HEADER_CATALOG),
                              tree->value_size) == DB_OK);
            assert(btree_verify(tree, NULL) == DB_OK);
        }
    }

    assert(tree_statistics.records == 0 && tree_statistics.height == 1);
}

static void test_page_reuse(BTree *tree, int record_count) {
    uint8_t value[32];
    BTreeStats tree_statistics;
    uint32_t initial_page_count = tree->pager->page_count;

    for (int insertion_index = 0; insertion_index < record_count / 4; insertion_index++) {
        write_u64(value, (uint64_t)insertion_index);
        assert(btree_insert(tree, insertion_index, value, 8) == DB_OK);
    }

    assert(tree->pager->page_count == initial_page_count);

    for (int deletion_index = record_count / 4 - 1; deletion_index >= 0; deletion_index--) {
        assert(btree_delete(tree, deletion_index) == DB_OK);
    }

    assert(btree_verify(tree, &tree_statistics) == DB_OK && tree_statistics.records == 0);
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
    delete_all_keys(&tree, database_path, record_count);
    test_page_reuse(&tree, record_count);

    assert(pager_close(&pager) == DB_OK);
    assert(unlink(database_path) == 0);
}

int main(void) {
    test_tree_persistence();
    test_capacities();
    test_allocation_failure();

    puts("btree: OK");
    return 0;
}
