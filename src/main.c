#include "db.h"
#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SQL_LIMIT 65536u

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
         ".exit            Save and close the database\n"
         "End SQL statements with ; (multiple lines are supported).");
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

static int run_sql(Database *db, const char *sql) {
    while (isspace((unsigned char)*sql)) {
        sql++;
    }
    if (!*sql) {
        return 0;
    }
    Statement statement;
    char error[160];
    DbStatus status = parser_parse(sql, &statement, error, sizeof(error));
    if (status != DB_OK) {
        fprintf(stderr, "Error: %s\n", error);
        return 1;
    }
    status = db_execute(db, &statement, stdout);
    if (status != DB_OK) {
        fprintf(stderr, "Error: %s\n", db_status_string(status));
        return 1;
    }
    return 0;
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
    char *sql = malloc(SQL_LIMIT + 1);
    if (!sql) {
        fputs("Error: out of memory\n", stderr);
        db_close(&db);
        return 1;
    }
    size_t pending = 0;
    int result = 0;
    for (;;) {
        if (interactive) {
            fputs(pending ? "   ...> " : "minidb> ", stdout);
            fflush(stdout);
        }
        ssize_t length = getline(&line, &capacity, stdin);
        if (length < 0) {
            if (!feof(stdin)) {
                perror("stdin");
                result = 1;
            }
            break;
        }
        if (memchr(line, 0, (size_t)length)) {
            fputs("Error: NUL byte in input\n", stderr);
            pending = 0;
            result = 1;
            continue;
        }
        if (!pending) {
            char *command = line;
            while (isspace((unsigned char)*command)) {
                command++;
            }
            if (!*command) {
                continue;
            }
            if (*command == '.') {
                command = trim(command);
                if (!strcmp(command, ".exit")) {
                    break;
                }
                status = meta_command(&db, command);
                if (status != DB_OK) {
                    fprintf(stderr, "Error: %s (use .help)\n", db_status_string(status));
                    if (!interactive) {
                        result = 1;
                    }
                }
                continue;
            }
        }
        if ((size_t)length > SQL_LIMIT - pending) {
            fputs("Error: SQL input exceeds 65536 bytes\n", stderr);
            pending = 0;
            if (!interactive) {
                result = 1;
            }
            continue;
        }
        memcpy(sql + pending, line, (size_t)length);
        pending += (size_t)length;
        sql[pending] = '\0';
        bool quoted = false;
        size_t consumed = 0;
        for (size_t i = 0; i < pending; i++) {
            if (sql[i] == '\'') {
                if (quoted && i + 1 < pending && sql[i + 1] == '\'') {
                    i++;
                } else {
                    quoted = !quoted;
                }
            } else if (sql[i] == ';' && !quoted) {
                sql[i] = '\0';
                int failed = run_sql(&db, sql + consumed);
                if (failed && !interactive) {
                    result = 1;
                }
                consumed = i + 1;
            }
        }
        memmove(sql, sql + consumed, pending - consumed);
        pending -= consumed;
        sql[pending] = '\0';
        size_t first = 0;
        while (first < pending && isspace((unsigned char)sql[first])) {
            first++;
        }
        if (first == pending) {
            pending = 0;
        }
    }
    if (pending && run_sql(&db, sql) && !interactive) {
        result = 1;
    }
    free(sql);
    free(line);
    if (fflush(stdout) == EOF) {
        result = 1;
    }
    status = db_close(&db);
    if (status != DB_OK) {
        fprintf(stderr, "Error: %s\n", db_status_string(status));
        result = 1;
    }
    return result;
}
