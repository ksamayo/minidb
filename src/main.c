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

static char *command_argument(char *command) {
    char *argument = strpbrk(command, " \t");

    if (argument) {
        *argument++ = '\0';
        argument = trim(argument);

        if (!*argument) {
            argument = NULL;
        }
    }

    return argument;
}

static void print_schema(const Table *table) {
    for (unsigned i = 0; i < table->schema.count; i++) {
        const Column *column = &table->schema.columns[i];

        printf("%s %s\n", column->name, column->type == TYPE_INT ? "INT" : "TEXT");
    }
}

static DbStatus show_table_details(Database *db, char *argument, bool schema) {
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
            print_schema(table);
        } else {
            DbStatus status = btree_dump(&table->tree, stdout);

            if (status != DB_OK) {
                return status;
            }
        }
    }

    return ferror(stdout) ? DB_IO : DB_OK;
}

static DbStatus meta_command(Database *db, char *command) {
    char *argument = command_argument(command);

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

    bool schema = strcmp(command, ".schema") == 0;

    if (!schema && strcmp(command, ".btree") != 0) {
        return DB_INVALID;
    }

    return show_table_details(db, argument, schema);
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

typedef struct {
    Database *db;
    bool interactive;
    char *sql;
    size_t pending;
    int result;
} Repl;

typedef enum {
    LINE_SQL,
    LINE_HANDLED,
    LINE_EXIT
} LineAction;

static LineAction process_command(Repl *repl, char *line) {
    while (isspace((unsigned char)*line)) {
        line++;
    }

    if (*line == '\0') {
        return LINE_HANDLED;
    }

    if (*line != '.') {
        return LINE_SQL;
    }

    char *command = trim(line);

    if (strcmp(command, ".exit") == 0) {
        return LINE_EXIT;
    }

    DbStatus status = meta_command(repl->db, command);

    if (status != DB_OK) {
        fprintf(stderr, "Error: %s (use .help)\n", db_status_string(status));

        if (!repl->interactive) {
            repl->result = 1;
        }
    }

    return LINE_HANDLED;
}

static bool append_sql(Repl *repl, const char *line, size_t length) {
    if (length > SQL_LIMIT - repl->pending) {
        fputs("Error: SQL input exceeds 65536 bytes\n", stderr);
        repl->pending = 0;

        if (!repl->interactive) {
            repl->result = 1;
        }

        return false;
    }

    memcpy(repl->sql + repl->pending, line, length);
    repl->pending += length;
    repl->sql[repl->pending] = '\0';

    return true;
}

static void discard_processed_sql(Repl *repl, size_t consumed) {
    memmove(repl->sql, repl->sql + consumed, repl->pending - consumed);
    repl->pending -= consumed;
    repl->sql[repl->pending] = '\0';

    size_t first = 0;

    while (first < repl->pending && isspace((unsigned char)repl->sql[first])) {
        first++;
    }

    if (first == repl->pending) {
        repl->pending = 0;
    }
}

static void execute_pending_sql(Repl *repl) {
    bool quoted = false;
    size_t consumed = 0;

    for (size_t i = 0; i < repl->pending; i++) {
        if (repl->sql[i] == '\'') {
            if (quoted && i + 1 < repl->pending && repl->sql[i + 1] == '\'') {
                i++;
            } else {
                quoted = !quoted;
            }
        } else if (repl->sql[i] == ';' && !quoted) {
            repl->sql[i] = '\0';

            int failed = run_sql(repl->db, repl->sql + consumed);

            if (failed && !repl->interactive) {
                repl->result = 1;
            }

            consumed = i + 1;
        }
    }

    discard_processed_sql(repl, consumed);
}

static bool process_line(Repl *repl, char *line, size_t length) {
    if (memchr(line, 0, length)) {
        fputs("Error: NUL byte in input\n", stderr);
        repl->pending = 0;
        repl->result = 1;

        return true;
    }

    if (repl->pending == 0) {
        LineAction action = process_command(repl, line);

        if (action == LINE_EXIT) {
            return false;
        }

        if (action == LINE_HANDLED) {
            return true;
        }
    }

    if (append_sql(repl, line, length)) {
        execute_pending_sql(repl);
    }

    return true;
}

static int run_repl(Database *db, char *sql, bool interactive) {
    Repl repl = {.db = db, .interactive = interactive, .sql = sql};
    char *line = NULL;
    size_t capacity = 0;

    for (;;) {
        if (interactive) {
            fputs(repl.pending ? "   ...> " : "minidb> ", stdout);
            fflush(stdout);
        }

        ssize_t length = getline(&line, &capacity, stdin);

        if (length < 0) {
            if (!feof(stdin)) {
                perror("stdin");
                repl.result = 1;
            }

            break;
        }

        if (!process_line(&repl, line, (size_t)length)) {
            break;
        }
    }

    if (repl.pending && feof(stdin) && run_sql(db, sql) && !interactive) {
        repl.result = 1;
    }

    free(line);

    return repl.result;
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

    bool interactive = isatty(STDIN_FILENO);

    if (interactive) {
        puts("MiniDB v0.1\nType .help for commands");
    }

    char *sql = malloc(SQL_LIMIT + 1);

    if (!sql) {
        fputs("Error: out of memory\n", stderr);
        db_close(&db);

        return 1;
    }

    int result = run_repl(&db, sql, interactive);

    free(sql);

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
