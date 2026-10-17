#include "record.h"

#include <inttypes.h>
#include <string.h>

bool record_valid_name(const char *name) {
    if (!name) {
        return false;
    }

    size_t length = strnlen(name, MAX_NAME + 1);

    if (!length || length > MAX_NAME) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];

        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';

        if (!letter && !(i && c >= '0' && c <= '9')) {
            return false;
        }
    }

    return true;
}

static bool column_is_valid(const Schema *schema, unsigned index) {
    const Column *column = &schema->columns[index];

    if (!record_valid_name(column->name) ||
        (column->type != TYPE_INT && column->type != TYPE_TEXT)) {
        return false;
    }

    for (unsigned previous = 0; previous < index; previous++) {
        if (strcmp(column->name, schema->columns[previous].name) == 0) {
            return false;
        }
    }

    return true;
}

DbStatus schema_validate(const Schema *schema) {
    if (!schema || !schema->count || schema->count > MAX_COLUMNS ||
        strcmp(schema->columns[0].name, "id") || schema->columns[0].type != TYPE_INT) {
        return DB_INVALID;
    }

    for (unsigned i = 0; i < schema->count; i++) {
        if (!column_is_valid(schema, i)) {
            return DB_INVALID;
        }
    }

    return DB_OK;
}

uint16_t record_max_size(const Schema *schema) {
    if (schema_validate(schema) != DB_OK) {
        return 0;
    }

    unsigned size = 0;

    for (unsigned i = 0; i < schema->count; i++) {
        size += schema->columns[i].type == TYPE_INT ? 8u : MAX_TEXT + 1u;
    }

    return (uint16_t)size;
}

static DbStatus measure_values(const Schema *schema, const Value *values, size_t *sizes,
                               size_t *total) {
    *total = 0;

    for (unsigned i = 0; i < schema->count; i++) {
        if (values[i].type != schema->columns[i].type) {
            return DB_INVALID;
        }

        sizes[i] = 8;

        if (values[i].type == TYPE_TEXT) {
            size_t text_length = strnlen(values[i].as.text, MAX_TEXT + 1);

            if (text_length > MAX_TEXT) {
                return DB_INVALID;
            }

            sizes[i] = text_length + 1;
        }

        *total += sizes[i];
    }

    return DB_OK;
}

static void encode_value(const Value *value, uint8_t *buffer, size_t size) {
    if (value->type == TYPE_INT) {
        write_u64(buffer, (uint64_t)value->as.integer);
    } else {
        size_t text_length = size - 1;

        buffer[0] = (uint8_t)text_length;
        memcpy(buffer + 1, value->as.text, text_length);
    }
}

DbStatus record_encode(const Schema *schema, const Value *values, unsigned count, uint8_t *buffer,
                       size_t capacity, uint16_t *length) {
    if (schema_validate(schema) != DB_OK || !values || !buffer || !length ||
        count != schema->count) {
        return DB_INVALID;
    }

    size_t sizes[MAX_COLUMNS];
    size_t total;
    DbStatus status = measure_values(schema, values, sizes, &total);

    if (status != DB_OK) {
        return status;
    }

    if (total > capacity) {
        return DB_INVALID;
    }

    size_t offset = 0;

    for (unsigned i = 0; i < count; i++) {
        encode_value(&values[i], buffer + offset, sizes[i]);
        offset += sizes[i];
    }

    *length = (uint16_t)total;

    return DB_OK;
}

static DbStatus decode_value(ColumnType type, const uint8_t *buffer, size_t available, Value *value,
                             size_t *consumed) {
    value->type = type;

    if (type == TYPE_INT) {
        if (available < 8) {
            return DB_CORRUPT;
        }

        value->as.integer = read_i64(buffer);
        *consumed = 8;

        return DB_OK;
    }

    if (available == 0) {
        return DB_CORRUPT;
    }

    size_t text_length = buffer[0];

    if (text_length > available - 1 || memchr(buffer + 1, 0, text_length)) {
        return DB_CORRUPT;
    }

    memcpy(value->as.text, buffer + 1, text_length);
    value->as.text[text_length] = '\0';
    *consumed = text_length + 1;

    return DB_OK;
}

DbStatus record_decode(const Schema *schema, const uint8_t *buffer, size_t length, Value *values) {
    if (schema_validate(schema) != DB_OK || !buffer || !values) {
        return DB_INVALID;
    }

    size_t offset = 0;

    for (unsigned i = 0; i < schema->count; i++) {
        size_t consumed;
        DbStatus status = decode_value(schema->columns[i].type, buffer + offset, length - offset,
                                       &values[i], &consumed);

        if (status != DB_OK) {
            return status;
        }

        offset += consumed;
    }

    return offset == length ? DB_OK : DB_CORRUPT;
}

DbStatus record_print(const Schema *schema, const Value *values, FILE *output) {
    for (unsigned i = 0; i < schema->count; i++) {
        if (i) {
            fputs(" | ", output);
        }

        if (values[i].type == TYPE_INT) {
            fprintf(output, "%" PRId64, values[i].as.integer);
        } else {
            fputs(values[i].as.text, output);
        }
    }

    fputc('\n', output);

    return ferror(output) ? DB_IO : DB_OK;
}
