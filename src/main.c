#include "table.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *trim(char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }
    size_t length = strlen(line);
    while (length && isspace((unsigned char)line[length - 1])) {
        line[--length] = '\0';
    }
    return line;
}

static int execute_command(MemoryTable *table, const char *command) {
    TableStatus status;
    char trailing_character;
    Row row = {0};
    if (sscanf(command, "insert %" SCNd64 " %63s %" SCNd64 " %c", &row.id, row.name, &row.age,
               &trailing_character) == 3) {
        status = table_insert(table, &row);
        if (status == TABLE_OK) {
            puts("1 row inserted");
        }
    } else if (!strcmp(command, "select")) {
        status = table_select(table, stdout);

    } else {
        fputs("Error: expected insert <id> <name> <age> or select\n", stderr);
        return 1;
    }
    if (status != TABLE_OK) {
        fprintf(stderr, "Error: table operation failed (%d)\n", status);
        return 1;
    }
    return 0;
}

static int run_repl(MemoryTable *table) {
    char *line = NULL;
    size_t line_capacity = 0;
    int interactive = isatty(STDIN_FILENO);
    int result = 0;
    for (;;) {
        if (interactive) {
            fputs("minidb> ", stdout);
            fflush(stdout);
        }
        if (getline(&line, &line_capacity, stdin) < 0) {
            break;
        }
        char *command = trim(line);
        if (!*command) {
            continue;
        }
        if (!strcmp(command, ".exit")) {
            break;
        }
        if (!strcmp(command, ".help")) {
            puts("insert <id> <name> <age>\nselect\n.exit");
        } else {
            result |= execute_command(table, command);
        }
    }
    free(line);
    return ferror(stdin) ? 1 : result;
}

int main(void) {
    MemoryTable table;
    table_init(&table);
    return run_repl(&table);
}
