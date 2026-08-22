#include "storage.h"

#include <errno.h>

TableStatus storage_load(MemoryTable *table, const char *database_path) {
    FILE *database_file = fopen(database_path, "rb");
    if (!database_file) {
        return errno == ENOENT ? TABLE_OK : TABLE_IO;
    }
    size_t row_count;
    TableStatus status = TABLE_OK;
    if (fread(&row_count, sizeof(row_count), 1, database_file) != 1 || row_count > TABLE_CAPACITY ||
        fread(table->rows, sizeof(Row), row_count, database_file) != row_count) {
        status = TABLE_INVALID;
    } else {
        table->row_count = row_count;
    }
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
    TableStatus status = TABLE_OK;
    if (fwrite(&table->row_count, sizeof(table->row_count), 1, database_file) != 1 ||
        fwrite(table->rows, sizeof(Row), table->row_count, database_file) != table->row_count) {
        status = TABLE_IO;
    }
    if (fclose(database_file) != 0) {
        status = TABLE_IO;
    }
    return status;
}
