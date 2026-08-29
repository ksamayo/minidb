#include "pager.h"
#include "storage.h"

#include <string.h>

#define STORED_ROW_SIZE (8u + ROW_NAME_SIZE + 8u)

static uint64_t decode_integer(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned byte_index = 0; byte_index < 8; byte_index++) {
        value |= (uint64_t)bytes[byte_index] << (byte_index * 8);
    }
    return value;
}

static void encode_integer(uint8_t *bytes, uint64_t value) {
    for (unsigned byte_index = 0; byte_index < 8; byte_index++) {
        bytes[byte_index] = (uint8_t)(value >> (byte_index * 8));
    }
}

static void encode_row(const Row *row, uint8_t *bytes) {
    encode_integer(bytes, (uint64_t)row->id);
    memcpy(bytes + 8, row->name, ROW_NAME_SIZE);
    encode_integer(bytes + 8 + ROW_NAME_SIZE, (uint64_t)row->age);
}

static int64_t decode_signed_integer(const uint8_t *bytes) {
    uint64_t value = decode_integer(bytes);
    return value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
}

static TableStatus decode_row(Row *row, const uint8_t *bytes) {
    if (!memchr(bytes + 8, '\0', ROW_NAME_SIZE)) {
        return TABLE_INVALID;
    }
    row->id = decode_signed_integer(bytes);
    memcpy(row->name, bytes + 8, ROW_NAME_SIZE);
    row->age = decode_signed_integer(bytes + 8 + ROW_NAME_SIZE);
    return TABLE_OK;
}

#define ROWS_PER_PAGE (PAGE_SIZE / STORED_ROW_SIZE)

static TableStatus storage_status(DbStatus status) {
    if (status == DB_OK) {
        return TABLE_OK;
    }
    return status == DB_CORRUPT ? TABLE_INVALID : TABLE_IO;
}

TableStatus storage_load(MemoryTable *table, const char *database_path) {
    Pager pager;
    DbStatus status = pager_open(&pager, database_path);
    if (status != DB_OK) {
        return storage_status(status);
    }
    uint32_t catalog_page_number = read_u32(pager.header + HEADER_CATALOG);
    if (!catalog_page_number) {
        return storage_status(pager_close(&pager));
    }
    uint8_t *catalog_page;
    status = pager_get(&pager, catalog_page_number, &catalog_page);
    size_t row_count = status == DB_OK ? read_u32(catalog_page + 4) : 0;
    TableStatus result = storage_status(status);
    if (status == DB_OK && (catalog_page[0] != PAGE_CATALOG || row_count > TABLE_CAPACITY)) {
        result = TABLE_INVALID;
    }
    for (size_t row_index = 0; result == TABLE_OK && row_index < row_count; row_index++) {
        uint8_t *page;
        status = pager_get(&pager, 2 + (uint32_t)(row_index / ROWS_PER_PAGE), &page);
        if (status != DB_OK) {
            result = storage_status(status);
        } else {
            result = decode_row(&table->rows[row_index],
                                page + (row_index % ROWS_PER_PAGE) * STORED_ROW_SIZE);
        }
    }
    if (result == TABLE_OK) {
        table->row_count = row_count;
    }
    status = pager_close(&pager);
    return result == TABLE_OK ? storage_status(status) : result;
}

TableStatus storage_save(const MemoryTable *table, const char *database_path) {
    Pager pager;
    DbStatus status = pager_open(&pager, database_path);
    if (status != DB_OK) {
        return storage_status(status);
    }
    uint32_t catalog_page_number = read_u32(pager.header + HEADER_CATALOG);
    uint8_t *page;
    if (!catalog_page_number) {
        status = pager_allocate(&pager, &catalog_page_number, &page);
        if (status == DB_OK) {
            status = pager_set_catalog(&pager, catalog_page_number);
        }
    }
    size_t required_pages = 2 + (table->row_count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    while (status == DB_OK && pager.page_count < required_pages) {
        uint32_t page_number;
        status = pager_allocate(&pager, &page_number, &page);
    }
    if (status == DB_OK) {
        status = pager_get(&pager, catalog_page_number, &page);
    }
    if (status == DB_OK) {
        page[0] = PAGE_CATALOG;
        write_u32(page + 4, (uint32_t)table->row_count);
        pager_dirty(&pager, catalog_page_number);
    }
    for (size_t row_index = 0; status == DB_OK && row_index < table->row_count; row_index++) {
        uint32_t page_number = 2 + (uint32_t)(row_index / ROWS_PER_PAGE);
        status = pager_get(&pager, page_number, &page);
        if (status == DB_OK) {
            encode_row(&table->rows[row_index],
                       page + (row_index % ROWS_PER_PAGE) * STORED_ROW_SIZE);
            pager_dirty(&pager, page_number);
        }
    }
    DbStatus close_status = pager_close(&pager);
    return storage_status(status == DB_OK ? close_status : status);
}
