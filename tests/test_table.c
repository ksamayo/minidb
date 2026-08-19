#include "table.h"

#include <assert.h>
#include <string.h>

int main(void) {
    MemoryTable table;
    table_init(&table);
    assert(table.row_count == 0);
    Row alice = {.id = 1, .name = "Alice", .age = 22};
    Row bob = {.id = 2, .name = "Bob", .age = 25};
    assert(table_insert(&table, &alice) == TABLE_OK);
    assert(table_insert(&table, &bob) == TABLE_OK);
    assert(table_insert(&table, &alice) == TABLE_DUPLICATE);
    assert(table.row_count == 2);
    assert(!strcmp(table.rows[0].name, "Alice"));
    FILE *output = tmpfile();
    assert(output);
    assert(table_select(&table, output) == TABLE_OK);
    assert(fclose(output) == 0);
    puts("table: OK");
    return 0;
}
