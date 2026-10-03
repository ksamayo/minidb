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

static void next(Parser *parser) {
    if (parser->failed) {
        return;
    }
    const char *input = parser->input;
    while (isspace((unsigned char)input[parser->position])) {
        parser->position++;
    }
    size_t start = parser->position;
    parser->token = (Token){.offset = start};
    unsigned char c = (unsigned char)input[start];
    if (!c) {
        return;
    }
    parser->position++;
    switch (c) {
        case '(':
            parser->token.type = TOK_LEFT;
            return;
        case ')':
            parser->token.type = TOK_RIGHT;
            return;
        case ',':
            parser->token.type = TOK_COMMA;
            return;
        case '*':
            parser->token.type = TOK_STAR;
            return;
        case '=':
            parser->token.type = TOK_EQUAL;
            return;
        case ';':
            parser->token.type = TOK_SEMICOLON;
            return;
        case '\'': {
            parser->token.type = TOK_STRING;
            size_t length = 0;
            for (;;) {
                c = (unsigned char)input[parser->position++];
                if (!c) {
                    fail(parser, "unterminated string");
                    return;
                }
                if (c == '\'') {
                    if (input[parser->position] != '\'') {
                        break;
                    }
                    parser->position++;
                }
                if (length == MAX_TEXT) {
                    fail(parser, "TEXT exceeds 255 bytes");
                    return;
                }
                parser->token.text[length++] = (char)c;
            }
            return;
        }
        default:
            break;
    }
    if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
        char *end;
        errno = 0;
        intmax_t value = strtoimax(input + start, &end, 10);
        if (end == input + start || errno == ERANGE || value < INT64_MIN || value > INT64_MAX) {
            fail(parser, "invalid 64-bit integer");
            return;
        }
        parser->token.type = TOK_INTEGER;
        parser->token.integer = (int64_t)value;
        parser->position = (size_t)(end - input);
        return;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
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
        return;
    }
    parser->token.type = TOK_BAD;
    fail(parser, "unexpected character");
}

static bool word(const Parser *parser, const char *text) {
    return !parser->failed && parser->token.type == TOK_WORD && !strcmp(parser->token.text, text);
}

static bool expect(Parser *parser, TokenType type, const char *message) {
    if (parser->failed) {
        return false;
    }
    if (parser->token.type != type) {
        fail(parser, message);
        return false;
    }
    next(parser);
    return !parser->failed;
}

static bool keyword(Parser *parser, const char *text) {
    if (!word(parser, text)) {
        fail(parser, text);
        return false;
    }
    next(parser);
    return !parser->failed;
}

static bool name(Parser *parser, char *target) {
    if (parser->failed) {
        return false;
    }
    if (parser->token.type != TOK_WORD) {
        fail(parser, "expected identifier");
        return false;
    }
    strcpy(target, parser->token.text);
    next(parser);
    return !parser->failed;
}

static bool value(Parser *parser, Value *target) {
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
    next(parser);
    return !parser->failed;
}

static void create_table(Parser *parser, Statement *statement) {
    statement->type = STMT_CREATE;
    next(parser);
    if (!keyword(parser, "table") || !name(parser, statement->table) ||
        !expect(parser, TOK_LEFT, "expected (")) {
        return;
    }
    for (;;) {
        if (statement->schema.count == MAX_COLUMNS) {
            fail(parser, "at most 8 columns are supported");
            return;
        }
        Column *column = &statement->schema.columns[statement->schema.count++];
        if (!name(parser, column->name)) {
            return;
        }
        if (word(parser, "int")) {
            column->type = TYPE_INT;
        } else if (word(parser, "text")) {
            column->type = TYPE_TEXT;
        } else {
            fail(parser, "expected INT or TEXT");
            return;
        }
        next(parser);
        if (parser->token.type != TOK_COMMA) {
            break;
        }
        next(parser);
    }
    if (!expect(parser, TOK_RIGHT, "expected )")) {
        return;
    }
    if (schema_validate(&statement->schema) != DB_OK) {
        fail(parser, "schema needs id INT first and unique column names");
    }
}

static void insert(Parser *parser, Statement *statement) {
    statement->type = STMT_INSERT;
    next(parser);
    if (!keyword(parser, "into") || !name(parser, statement->table) || !keyword(parser, "values") ||
        !expect(parser, TOK_LEFT, "expected (")) {
        return;
    }
    for (;;) {
        if (statement->value_count == MAX_COLUMNS) {
            fail(parser, "too many values");
            return;
        }
        if (!value(parser, &statement->values[statement->value_count++])) {
            return;
        }
        if (parser->token.type != TOK_COMMA) {
            break;
        }
        next(parser);
    }
    expect(parser, TOK_RIGHT, "expected )");
}

static void where(Parser *parser, Statement *statement) {
    statement->has_where = true;
    if (!keyword(parser, "where") || !keyword(parser, "id") ||
        !expect(parser, TOK_EQUAL, "expected =")) {
        return;
    }
    if (parser->token.type != TOK_INTEGER) {
        fail(parser, "WHERE id requires an integer");
        return;
    }
    statement->id = parser->token.integer;
    next(parser);
}

static void select_rows(Parser *parser, Statement *statement) {
    statement->type = STMT_SELECT;
    next(parser);
    if (!expect(parser, TOK_STAR, "only SELECT * is supported") || !keyword(parser, "from") ||
        !name(parser, statement->table)) {
        return;
    }
    if (word(parser, "where")) {
        where(parser, statement);
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
    next(&parser);
    if (word(&parser, "create")) {
        create_table(&parser, statement);
    } else if (word(&parser, "insert")) {
        insert(&parser, statement);
    } else if (word(&parser, "select")) {
        select_rows(&parser, statement);
    } else {
        fail(&parser, "expected CREATE, INSERT or SELECT");
    }
    if (!parser.failed && parser.token.type == TOK_SEMICOLON) {
        next(&parser);
    }
    if (!parser.failed && parser.token.type != TOK_END) {
        fail(&parser, "unexpected token after statement");
    }
    return parser.failed ? DB_INVALID : DB_OK;
}
