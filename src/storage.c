#include "storage.h"

#include <errno.h>
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

#define STORAGE_HEADER_SIZE 24u
static const uint8_t storage_magic[8] = {'M', 'I', 'N', 'I', 'R', 'O', 'W', 0};

TableStatus storage_load(MemoryTable *table, const char *database_path) {
    FILE *database_file = fopen(database_path, "rb");
    if (!database_file) {
        return errno == ENOENT ? TABLE_OK : TABLE_IO;
    }
    uint8_t header[STORAGE_HEADER_SIZE];
    TableStatus status = TABLE_INVALID;
    if (fread(header, sizeof(header), 1, database_file) != 1 ||
        memcmp(header, storage_magic, sizeof(storage_magic)) || decode_integer(header + 8) != 1) {
        goto close_file;
    }
    uint64_t row_count = decode_integer(header + 16);
    if (row_count > TABLE_CAPACITY) {
        goto close_file;
    }
    for (size_t row_index = 0; row_index < row_count; row_index++) {
        uint8_t bytes[STORED_ROW_SIZE];
        if (fread(bytes, sizeof(bytes), 1, database_file) != 1 ||
            decode_row(&table->rows[row_index], bytes) != TABLE_OK) {
            goto close_file;
        }
    }
    table->row_count = (size_t)row_count;
    status = TABLE_OK;
close_file:
    if (fclose(database_file) != 0) {
        status = TABLE_IO;
    }
    return status;
}

TableStatus storage_save(const MemoryTable *table, const char *database_path) {
    FILE *database_file = fopen(database_path, "wb");
    if (!database_file) {
        return TABLE_IO;
    }
    uint8_t header[STORAGE_HEADER_SIZE] = {0};
    memcpy(header, storage_magic, sizeof(storage_magic));
    encode_integer(header + 8, 1);
    encode_integer(header + 16, table->row_count);
    TableStatus status = TABLE_OK;
    if (fwrite(header, sizeof(header), 1, database_file) != 1) {
        status = TABLE_IO;
    }
    for (size_t row_index = 0; status == TABLE_OK && row_index < table->row_count; row_index++) {
        uint8_t bytes[STORED_ROW_SIZE];
        encode_row(&table->rows[row_index], bytes);
        if (fwrite(bytes, sizeof(bytes), 1, database_file) != 1) {
            status = TABLE_IO;
        }
    }
    if (fclose(database_file) != 0) {
        status = TABLE_IO;
    }
    return status;
}
