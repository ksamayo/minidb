#include "parser.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    TOK_END,
    TOK_WORD,
    TOK_INTEGER,
    TOK_STRING,
    TOK_LEFT,
    TOK_RIGHT,
    TOK_COMMA,
    TOK_STAR,
    TOK_EQUAL,
    TOK_SEMICOLON,
    TOK_BAD
} TokenType;

typedef struct {
    TokenType type;
    char text[MAX_TEXT + 1];
    int64_t integer;
    size_t offset;
} Token;

typedef struct {
    const char *input;
    size_t position;
    Token token;
    char *error;
    size_t error_size;
    bool failed;
} Parser;

static void fail(Parser *parser, const char *message) {
    if (!parser->failed && parser->error && parser->error_size) {
        snprintf(parser->error, parser->error_size, "%s at byte %zu", message,
                 parser->token.offset);
    }

    parser->failed = true;
}

static bool read_symbol_token(Parser *parser, unsigned char character) {
    switch (character) {
        case '(': {
            parser->token.type = TOK_LEFT;

            return true;
        }

        case ')': {
            parser->token.type = TOK_RIGHT;

            return true;
        }

        case ',': {
            parser->token.type = TOK_COMMA;

            return true;
        }

        case '*': {
            parser->token.type = TOK_STAR;

            return true;
        }

        case '=': {
            parser->token.type = TOK_EQUAL;

            return true;
        }

        case ';': {
            parser->token.type = TOK_SEMICOLON;

            return true;
        }

        default: {
            return false;
        }
    }
}

static void read_string_token(Parser *parser) {
    const char *input = parser->input;
    size_t length = 0;

    parser->token.type = TOK_STRING;

    for (;;) {
        unsigned char character = (unsigned char)input[parser->position++];

        if (character == '\0') {
            fail(parser, "unterminated string");

            return;
        }

        if (character == '\'') {
            if (input[parser->position] != '\'') {
                parser->token.text[length] = '\0';

                return;
            }

            parser->position++;
        }

        if (length == MAX_TEXT) {
            fail(parser, "TEXT exceeds 255 bytes");

            return;
        }

        parser->token.text[length++] = (char)character;
    }
}

static void read_integer_token(Parser *parser) {
    const char *start = parser->input + parser->token.offset;
    char *end;

    errno = 0;

    intmax_t integer = strtoimax(start, &end, 10);

    if (end == start || errno == ERANGE || integer < INT64_MIN || integer > INT64_MAX) {
        fail(parser, "invalid 64-bit integer");

        return;
    }

    parser->token.type = TOK_INTEGER;
    parser->token.integer = (int64_t)integer;
    parser->position = (size_t)(end - parser->input);
}

static void read_word_token(Parser *parser) {
    const char *input = parser->input;
    size_t start = parser->token.offset;

    while (isalnum((unsigned char)input[parser->position]) || input[parser->position] == '_') {
        parser->position++;
    }

    size_t length = parser->position - start;

    if (length > MAX_NAME) {
        fail(parser, "identifier exceeds 31 bytes");

        return;
    }

    parser->token.type = TOK_WORD;

    for (size_t i = 0; i < length; i++) {
        parser->token.text[i] = (char)tolower((unsigned char)input[start + i]);
    }

    parser->token.text[length] = '\0';
}

static bool is_integer_start(unsigned char character) {
    return (character >= '0' && character <= '9') || character == '-' || character == '+';
}

static bool is_word_start(unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           character == '_';
}

static void next_token(Parser *parser) {
    if (parser->failed) {
        return;
    }

    const char *input = parser->input;

    while (isspace((unsigned char)input[parser->position])) {
        parser->position++;
    }

    parser->token = (Token){.offset = parser->position};

    unsigned char character = (unsigned char)input[parser->position];

    if (character == '\0') {
        return;
    }

    parser->position++;

    if (read_symbol_token(parser, character)) {
        return;
    }

    if (character == '\'') {
        read_string_token(parser);
    } else if (is_integer_start(character)) {
        read_integer_token(parser);
    } else if (is_word_start(character)) {
        read_word_token(parser);
    } else {
        parser->token.type = TOK_BAD;
        fail(parser, "unexpected character");
    }
}

static bool token_is_keyword(const Parser *parser, const char *text) {
    return !parser->failed && parser->token.type == TOK_WORD &&
           strcmp(parser->token.text, text) == 0;
}

static bool expect_token(Parser *parser, TokenType type, const char *message) {
    if (parser->failed) {
        return false;
    }

    if (parser->token.type != type) {
        fail(parser, message);

        return false;
    }

    next_token(parser);

    return !parser->failed;
}

static bool expect_keyword(Parser *parser, const char *text) {
    if (!token_is_keyword(parser, text)) {
        fail(parser, text);

        return false;
    }

    next_token(parser);

    return !parser->failed;
}

static bool parse_name(Parser *parser, char *target) {
    if (parser->failed) {
        return false;
    }

    if (parser->token.type != TOK_WORD) {
        fail(parser, "expected identifier");

        return false;
    }

    strcpy(target, parser->token.text);
    next_token(parser);

    return !parser->failed;
}

static bool parse_value(Parser *parser, Value *target) {
    if (parser->token.type == TOK_INTEGER) {
        target->type = TYPE_INT;
        target->as.integer = parser->token.integer;

    } else if (parser->token.type == TOK_STRING) {
        target->type = TYPE_TEXT;
        strcpy(target->as.text, parser->token.text);

    } else {
        fail(parser, "expected integer or quoted text");

        return false;
    }

    next_token(parser);

    return !parser->failed;
}

static bool parse_column(Parser *parser, Column *column) {
    if (!parse_name(parser, column->name)) {
        return false;
    }

    if (token_is_keyword(parser, "int")) {
        column->type = TYPE_INT;
    } else if (token_is_keyword(parser, "text")) {
        column->type = TYPE_TEXT;
    } else {
        fail(parser, "expected INT or TEXT");

        return false;
    }

    next_token(parser);

    return !parser->failed;
}

static bool parse_schema(Parser *parser, Schema *schema) {
    if (!expect_token(parser, TOK_LEFT, "expected (")) {
        return false;
    }

    for (;;) {
        if (schema->count == MAX_COLUMNS) {
            fail(parser, "at most 8 columns are supported");

            return false;
        }

        Column *column = &schema->columns[schema->count++];

        if (!parse_column(parser, column)) {
            return false;
        }

        if (parser->token.type != TOK_COMMA) {
            break;
        }

        next_token(parser);
    }

    if (!expect_token(parser, TOK_RIGHT, "expected )")) {
        return false;
    }

    if (schema_validate(schema) != DB_OK) {
        fail(parser, "schema needs id INT first and unique column names");

        return false;
    }

    return true;
}

static bool parse_values(Parser *parser, Statement *statement) {
    if (!expect_token(parser, TOK_LEFT, "expected (")) {
        return false;
    }

    for (;;) {
        if (statement->value_count == MAX_COLUMNS) {
            fail(parser, "too many values");

            return false;
        }

        if (!parse_value(parser, &statement->values[statement->value_count++])) {
            return false;
        }

        if (parser->token.type != TOK_COMMA) {
            break;
        }

        next_token(parser);
    }

    return expect_token(parser, TOK_RIGHT, "expected )");
}

static bool parse_assignment(Parser *parser, Statement *statement) {
    if (!parse_name(parser, statement->column) || !expect_token(parser, TOK_EQUAL, "expected =") ||
        !parse_value(parser, &statement->values[0])) {
        return false;
    }

    statement->value_count = 1;

    return true;
}

static void parse_where(Parser *parser, Statement *statement) {
    statement->has_where = true;

    if (!expect_keyword(parser, "where") || !expect_keyword(parser, "id") ||
        !expect_token(parser, TOK_EQUAL, "expected =")) {
        return;
    }

    if (parser->token.type != TOK_INTEGER) {
        fail(parser, "WHERE id requires an integer");

        return;
    }

    statement->id = parser->token.integer;
    next_token(parser);
}

static void parse_create_table(Parser *parser, Statement *statement) {
    statement->type = STMT_CREATE;
    next_token(parser);

    if (!expect_keyword(parser, "table") || !parse_name(parser, statement->table)) {
        return;
    }

    parse_schema(parser, &statement->schema);
}

static void parse_insert(Parser *parser, Statement *statement) {
    statement->type = STMT_INSERT;
    next_token(parser);

    if (!expect_keyword(parser, "into") || !parse_name(parser, statement->table) ||
        !expect_keyword(parser, "values")) {
        return;
    }

    parse_values(parser, statement);
}

static void parse_select(Parser *parser, Statement *statement) {
    statement->type = STMT_SELECT;
    next_token(parser);

    if (!expect_token(parser, TOK_STAR, "only SELECT * is supported") ||
        !expect_keyword(parser, "from") || !parse_name(parser, statement->table)) {
        return;
    }

    if (token_is_keyword(parser, "where")) {
        parse_where(parser, statement);
    }
}

static void parse_update(Parser *parser, Statement *statement) {
    statement->type = STMT_UPDATE;
    next_token(parser);

    if (!parse_name(parser, statement->table) || !expect_keyword(parser, "set") ||
        !parse_assignment(parser, statement)) {
        return;
    }

    parse_where(parser, statement);
}

static void parse_delete(Parser *parser, Statement *statement) {
    statement->type = STMT_DELETE;
    next_token(parser);

    if (!expect_keyword(parser, "from") || !parse_name(parser, statement->table)) {
        return;
    }

    parse_where(parser, statement);
}

static void parse_statement(Parser *parser, Statement *statement) {
    if (token_is_keyword(parser, "create")) {
        parse_create_table(parser, statement);
    } else if (token_is_keyword(parser, "insert")) {
        parse_insert(parser, statement);
    } else if (token_is_keyword(parser, "select")) {
        parse_select(parser, statement);
    } else if (token_is_keyword(parser, "update")) {
        parse_update(parser, statement);
    } else if (token_is_keyword(parser, "delete")) {
        parse_delete(parser, statement);
    } else {
        fail(parser, "expected CREATE, INSERT, SELECT, UPDATE or DELETE");
    }
}

static void finish_statement(Parser *parser) {
    if (!parser->failed && parser->token.type == TOK_SEMICOLON) {
        next_token(parser);
    }

    if (!parser->failed && parser->token.type != TOK_END) {
        fail(parser, "unexpected token after statement");
    }
}

DbStatus parser_parse(const char *sql, Statement *statement, char *error, size_t error_size) {
    if (!sql || !statement) {
        return DB_INVALID;
    }

    memset(statement, 0, sizeof(*statement));

    if (error && error_size) {
        error[0] = '\0';
    }

    Parser parser = {.input = sql, .error = error, .error_size = error_size};

    next_token(&parser);
    parse_statement(&parser, statement);
    finish_statement(&parser);

    return parser.failed ? DB_INVALID : DB_OK;
}
