#include "table.h"

#include <inttypes.h>
#include <string.h>

void table_init(MemoryTable *table) {
    table->row_count = 0;
}

TableStatus table_insert(MemoryTable *table, const Row *row) {
    for (size_t row_index = 0; row_index < table->row_count; row_index++) {
        if (table->rows[row_index].id == row->id) {
            return TABLE_DUPLICATE;
        }
    }
    if (table->row_count == TABLE_CAPACITY) {
        return TABLE_FULL;
    }
    table->rows[table->row_count++] = *row;
    return TABLE_OK;
}

TableStatus table_select(const MemoryTable *table, FILE *output) {
    for (size_t row_index = 0; row_index < table->row_count; row_index++) {
        const Row *row = &table->rows[row_index];
        fprintf(output, "%" PRId64 " | %s | %" PRId64 "\n", row->id, row->name, row->age);
    }
    return ferror(output) ? TABLE_IO : TABLE_OK;
}
