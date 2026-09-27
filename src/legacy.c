#include "legacy.h"

#include <inttypes.h>
#include <string.h>

static DbStatus print_row(const Value *values, unsigned column_count, void *context) {
    const Schema *schema = context;
    (void)column_count;
    return record_print(schema, values, stdout);
}

DbStatus legacy_execute(Database *database, const char *command) {
    char table_name[MAX_NAME + 1];
    char trailing_character;
    int64_t id;
    int64_t age;
    char name[MAX_TEXT + 1];
    if (sscanf(command, ".create %31s %c", table_name, &trailing_character) == 1) {
        Schema schema = {3, {{"id", TYPE_INT}, {"name", TYPE_TEXT}, {"age", TYPE_INT}}};
        return db_create_table(database, table_name, &schema);
    }
    if (sscanf(command, "insert %31s %" SCNd64 " %255s %" SCNd64 " %c", table_name, &id, name, &age,
               &trailing_character) == 4) {
        Table *table = db_table(database, table_name);
        if (!table) {
            return DB_NOT_FOUND;
        }
        Value values[MAX_COLUMNS] = {{.type = TYPE_INT, .as.integer = id},
                                     {.type = TYPE_TEXT},
                                     {.type = TYPE_INT, .as.integer = age}};
        strcpy(values[1].as.text, name);
        return db_insert(table, values, 3);
    }
    if (sscanf(command, "select %31s %c", table_name, &trailing_character) == 1) {
        Table *table = db_table(database, table_name);
        return table ? db_scan(table, print_row, &table->schema) : DB_NOT_FOUND;
    }
    if (sscanf(command, "delete %31s %" SCNd64 " %c", table_name, &id, &trailing_character) == 2) {
        Table *table = db_table(database, table_name);
        return table ? db_delete(table, id) : DB_NOT_FOUND;
    }
    return DB_INVALID;
}
