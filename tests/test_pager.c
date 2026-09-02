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

static void test_cache_growth(Pager *pager) {
    uint32_t allocated_page_number;
    uint8_t *page;
    uint8_t *first_page;

    assert(pager_get(pager, 1, &page) == DB_INVALID);
    assert(pager_get(pager, UINT32_MAX, &page) == DB_INVALID);
    assert(pager_allocate(pager, &allocated_page_number, &first_page) == DB_OK &&
           allocated_page_number == 1);
    strcpy((char *)first_page, "page contents");

    for (unsigned page_number = 2; page_number <= 130; page_number++) {
        assert(pager_allocate(pager, &allocated_page_number, &page) == DB_OK &&
               allocated_page_number == page_number);
        write_u32(page + 16, page_number);
    }

    assert(!strcmp((char *)first_page, "page contents"));
}

static void test_page_release(Pager *pager) {
    assert(pager_release(pager, 0) == DB_INVALID);
    assert(pager_release(pager, 999) == DB_INVALID);
    assert(pager_release(pager, 42) == DB_OK);
    assert(pager_release(pager, 42) == DB_INVALID);
}

static void test_persisted_pages_and_reuse(Pager *pager) {
    uint8_t *page;
    uint32_t allocated_page_number;

    assert(pager->page_count == 131);
    assert(pager_get(pager, 1, &page) == DB_OK);
    assert(!strcmp((char *)page, "page contents"));
    assert(pager_get(pager, 130, &page) == DB_OK);
    assert(read_u32(page + 16) == 130);
    assert(pager_allocate(pager, &allocated_page_number, &page) == DB_OK &&
           allocated_page_number == 42);

    for (size_t byte_index = 0; byte_index < PAGE_SIZE; byte_index++) {
        assert(page[byte_index] == 0);
    }
}

static void test_page_cache(void) {
    char database_path[] = "/tmp/minidb-pager-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0 && close(file_descriptor) == 0 && unlink(database_path) == 0);

    Pager pager;

    assert(pager_open(&pager, database_path) == DB_OK);
    test_cache_growth(&pager);
    test_page_release(&pager);
    assert(pager_close(&pager) == DB_OK);

    assert(pager_open(&pager, database_path) == DB_OK);
    test_persisted_pages_and_reuse(&pager);
    assert(pager_close(&pager) == DB_OK);

    assert(unlink(database_path) == 0);
}

int main(void) {
    test_file_header();
    test_page_cache();

    puts("pager: OK");
    return 0;
}
