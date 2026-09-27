#include "db.h"
#include "legacy.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *trim(char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }
    size_t len = strlen(line);
    while (len && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    return line;
}

static void help(void) {
    puts(".help            Show available commands\n"
         ".tables          List tables\n"
         ".schema [table]  Show column names and types\n"
         ".btree [table]   Print tree pages and keys\n"
         ".exit            Save and close the database");
}

static DbStatus meta_command(Database *db, char *command) {
    char *argument = strpbrk(command, " \t");
    if (argument) {
        *argument++ = '\0';
        argument = trim(argument);
        if (!*argument) {
            argument = NULL;
        }
    }
    if (!strcmp(command, ".help") && !argument) {
        help();
        return DB_OK;
    }
    if (!strcmp(command, ".tables") && !argument) {
        for (size_t i = 0; i < db->table_count; i++) {
            puts(db->tables[i].name);
        }
        return DB_OK;
    }
    bool schema = !strcmp(command, ".schema");
    if (!schema && strcmp(command, ".btree")) {
        return DB_INVALID;
    }
    if (argument) {
        if (!record_valid_name(argument)) {
            return DB_INVALID;
        }
        for (char *p = argument; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        if (!db_table(db, argument)) {
            return DB_NOT_FOUND;
        }
    }
    for (size_t i = 0; i < db->table_count; i++) {
        Table *table = &db->tables[i];
        if (argument && strcmp(argument, table->name)) {
            continue;
        }
        if (!argument) {
            printf("%s:\n", table->name);
        }
        if (schema) {
            for (unsigned col = 0; col < table->schema.count; col++) {
                printf("%s %s\n", table->schema.columns[col].name,
                       table->schema.columns[col].type == TYPE_INT ? "INT" : "TEXT");
            }
        } else {
            DbStatus status = btree_dump(&table->tree, stdout);
            if (status != DB_OK) {
                return status;
            }
        }
    }
    return ferror(stdout) ? DB_IO : DB_OK;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s database.db\n", argv[0]);
        return 1;
    }
    Database db;
    DbStatus status = db_open(&db, argv[1]);
    if (status != DB_OK) {
        fprintf(stderr, "Error: %s\n", db_status_string(status));
        return 1;
    }
    int interactive = isatty(STDIN_FILENO);
    if (interactive) {
        puts("MiniDB v0.1\nType .help for commands");
    }
    char *line = NULL;
    size_t capacity = 0;
    for (;;) {
        if (interactive) {
            fputs("minidb> ", stdout);
            fflush(stdout);
        }
        if (getline(&line, &capacity, stdin) < 0) {
            break;
        }
        char *command = trim(line);
        if (!*command) {
            continue;
        }
        if (!strcmp(command, ".exit")) {
            break;
        }
        if (*command == '.' && strncmp(command, ".create ", 8)) {
            status = meta_command(&db, command);
            if (status != DB_OK) {
                fprintf(stderr, "Error: %s (use .help)\n", db_status_string(status));
            }
        } else {
            status = legacy_execute(&db, command);
            if (status != DB_OK) {
                fprintf(stderr, "Error: %s\n", db_status_string(status));
            }
        }
    }
    free(line);
    int result = 0;
    if (ferror(stdin)) {
        perror("stdin");
        result = 1;
    }
    status = db_close(&db);
    if (status != DB_OK) {
        fprintf(stderr, "Error: %s\n", db_status_string(status));
        result = 1;
    }
    return result;
}
