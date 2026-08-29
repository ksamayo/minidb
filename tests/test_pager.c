#include "pager.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_header_creation_and_locking(Pager *pager, const char *database_path) {
    Pager second_pager;

    assert(pager_open(pager, database_path) == DB_OK);
    assert(pager->page_count == 1);
    assert(read_u32(pager->header + HEADER_VERSION) == 1);
    assert(pager_open(&second_pager, database_path) == DB_IO);
    assert(pager_close(pager) == DB_OK);

    assert(pager_open(pager, database_path) == DB_OK);
    assert(pager->page_count == 1);
    assert(pager_close(pager) == DB_OK);
}

static void test_corrupted_header_fields(Pager *pager, const char *database_path,
                                         int file_descriptor) {
    uint8_t original_header[PAGE_SIZE];
    uint8_t corrupted_header[PAGE_SIZE];
    const unsigned field_offsets[] = {0, HEADER_VERSION, HEADER_PAGES, HEADER_CATALOG, HEADER_FREE};

    assert(read(file_descriptor, original_header, PAGE_SIZE) == PAGE_SIZE);

    for (size_t field_index = 0; field_index < sizeof(field_offsets) / sizeof(field_offsets[0]);
         field_index++) {
        memcpy(corrupted_header, original_header, PAGE_SIZE);
        write_u32(corrupted_header + field_offsets[field_index], UINT32_MAX);
        assert(pwrite(file_descriptor, corrupted_header, PAGE_SIZE, 0) == PAGE_SIZE);
        assert(pager_open(pager, database_path) == DB_CORRUPT);
    }
}

static void test_truncated_file(Pager *pager, const char *database_path, int file_descriptor) {
    assert(ftruncate(file_descriptor, 1) == 0);
    assert(pager_open(pager, database_path) == DB_CORRUPT);

    assert(ftruncate(file_descriptor, 0) == 0);
    assert(pager_open(pager, database_path) == DB_OK);
    assert(pager_close(pager) == DB_OK);
}

static void test_file_header(void) {
    char database_path[] = "/tmp/minidb-pager-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0);
    assert(close(file_descriptor) == 0);

    Pager pager;
    test_header_creation_and_locking(&pager, database_path);

    file_descriptor = open(database_path, O_RDWR);
    assert(file_descriptor >= 0);
    test_corrupted_header_fields(&pager, database_path, file_descriptor);
    test_truncated_file(&pager, database_path, file_descriptor);

    assert(close(file_descriptor) == 0);
    assert(unlink(database_path) == 0);
}

int main(void) {
    test_file_header();

    puts("pager: OK");
    return 0;
}
