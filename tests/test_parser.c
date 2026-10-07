#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_create_statement(Statement *statement, char *error_message,
                                  size_t error_capacity) {
    assert(parser_parse("CrEaTe TABLE Users (id INT, name TEXT, age INT);", statement,
                        error_message, error_capacity) == DB_OK);
    assert(statement->type == STMT_CREATE && statement->schema.count == 3);
    assert(!strcmp(statement->table, "users"));
}

static void test_insert_statement(Statement *statement, char *error_message,
                                  size_t error_capacity) {
    assert(parser_parse("INSERT INTO users VALUES (-9223372036854775808, 'O''Brien; hi', +22)",
                        statement, error_message, error_capacity) == DB_OK);
    assert(statement->values[0].as.integer == INT64_MIN);
    assert(!strcmp(statement->values[1].as.text, "O'Brien; hi"));
}

static void test_select_statement(Statement *statement, char *error_message,
                                  size_t error_capacity) {
    assert(parser_parse("SELECT * FROM users WHERE id = 9223372036854775807;", statement,
                        error_message, error_capacity) == DB_OK);
    assert(statement->has_where && statement->id == INT64_MAX);
}

static void test_update_statement(Statement *statement, char *error_message,
                                  size_t error_capacity) {
    assert(parser_parse("UPDATE users SET name = 'Alice' WHERE id = 1;", statement, error_message,
                        error_capacity) == DB_OK);
    assert(statement->type == STMT_UPDATE && !strcmp(statement->column, "name"));
}

static void test_delete_statement(Statement *statement, char *error_message,
                                  size_t error_capacity) {
    assert(parser_parse("DELETE FROM users WHERE id = -1;", statement, error_message,
                        error_capacity) == DB_OK);
    assert(statement->type == STMT_DELETE && statement->id == -1);
}

static void test_valid_statements(void) {
    Statement statement;
    char error_message[160];

    test_create_statement(&statement, error_message, sizeof(error_message));
    test_insert_statement(&statement, error_message, sizeof(error_message));
    test_select_statement(&statement, error_message, sizeof(error_message));
    test_update_statement(&statement, error_message, sizeof(error_message));
    test_delete_statement(&statement, error_message, sizeof(error_message));
}

static void test_invalid_statements(void) {
    Statement statement;
    char error_message[160];
    const char *invalid_statements[] = {
        "",
        "SELECT name FROM users",
        "SELECT * users",
        "SELECT * FROM users WHERE age = 1",
        "SELECT * FROM users WHERE id = '1'",
        "INSERT INTO users VALUES (1, 'oops)",
        "INSERT INTO users VALUES (9223372036854775808)",
        "INSERT INTO users VALUES (-9223372036854775809)",
        "INSERT INTO users VALUES (1,)",
        "INSERT INTO users VALUES ()",
        "CREATE TABLE users ()",
        "CREATE TABLE users (name TEXT)",
        "CREATE TABLE users (id TEXT)",
        "CREATE TABLE users (id INT, id TEXT)",
        "CREATE TABLE users (id INT,)",
        "CREATE TABLE users (id FLOAT)",
        "SELECT * FROM users; SELECT * FROM users",
        "SELECT * FROM users #",
        "UPDATE users SET age = 2",
        "UPDATE users SET age = 2, name = 'x' WHERE id = 1",
        "UPDATE users age = 2 WHERE id = 1",
        "DELETE FROM users",
        "DELETE users WHERE id = 1",
        "DELETE FROM users WHERE name = 'Bob'",
        "UPDATE users SET age = 2 WHERE id > 1",
    };
    for (size_t statement_index = 0;
         statement_index < sizeof(invalid_statements) / sizeof(invalid_statements[0]);
         statement_index++) {
        assert(parser_parse(invalid_statements[statement_index], &statement, error_message,
                            sizeof(error_message)) == DB_INVALID);
        assert(*error_message);
    }
}

static void test_text_limit(void) {
    Statement statement;
    char error_message[160];
    char sql_with_long_text[400];
    const char prefix[] = "INSERT INTO t VALUES (1, '";
    size_t prefix_length = strlen(prefix);

    memset(sql_with_long_text, 'a', sizeof(sql_with_long_text));
    memcpy(sql_with_long_text, prefix, prefix_length);
    strcpy(sql_with_long_text + prefix_length + 256, "');");
    assert(parser_parse(sql_with_long_text, &statement, error_message, sizeof(error_message)) ==
           DB_INVALID);
    assert(strstr(error_message, "TEXT exceeds"));

    strcpy(sql_with_long_text + prefix_length + 255, "');");
    assert(parser_parse(sql_with_long_text, &statement, error_message, sizeof(error_message)) ==
           DB_OK);
    assert(strlen(statement.values[1].as.text) == MAX_TEXT);
}

static void test_column_limit(void) {
    Statement statement;
    char error_message[160];
    assert(
        parser_parse("CREATE TABLE t (id INT, a INT, b TEXT, c INT, d TEXT, e INT, f TEXT, g INT)",
                     &statement, error_message, sizeof(error_message)) == DB_OK);
    assert(parser_parse(
               "CREATE TABLE t (id INT, a INT, b INT, c INT, d INT, e INT, f INT, g INT, h INT)",
               &statement, error_message, sizeof(error_message)) == DB_INVALID);
}

int main(void) {
    test_valid_statements();
    test_invalid_statements();
    test_text_limit();
    test_column_limit();

    puts("parser: OK");
    return 0;
}
