#include "storage.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char database_path[] = "/tmp/minidb-storage-XXXXXX";
    int file_descriptor = mkstemp(database_path);
    assert(file_descriptor >= 0 && close(file_descriptor) == 0 && unlink(database_path) == 0);
    MemoryTable table;
    table_init(&table);
    assert(storage_load(&table, database_path) == TABLE_OK);
    Row alice = {.id = 1, .name = "Alice", .age = 22};
    Row bob = {.id = 2, .name = "Bob", .age = 25};
    assert(table_insert(&table, &alice) == TABLE_OK);
    assert(table_insert(&table, &bob) == TABLE_OK);
    assert(table_delete(&table, 1) == TABLE_OK);
    assert(storage_save(&table, database_path) == TABLE_OK);
    table_init(&table);
    assert(storage_load(&table, database_path) == TABLE_OK);
    assert(table.row_count == 1 && table.rows[0].id == 2);
    assert(!strcmp(table.rows[0].name, "Bob"));
    file_descriptor = open(database_path, O_RDWR);
    assert(file_descriptor >= 0);
    uint8_t invalid_magic = 0;
    assert(pwrite(file_descriptor, &invalid_magic, 1, 0) == 1);
    assert(close(file_descriptor) == 0);
    assert(storage_load(&table, database_path) == TABLE_INVALID);
    assert(unlink(database_path) == 0);
    puts("storage: OK");
    return 0;
}
