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

DbStatus schema_validate(const Schema *schema) {
    if (!schema || !schema->count || schema->count > MAX_COLUMNS ||
        strcmp(schema->columns[0].name, "id") || schema->columns[0].type != TYPE_INT) {
        return DB_INVALID;
    }
    for (unsigned i = 0; i < schema->count; i++) {
        const Column *col = &schema->columns[i];
        if (!record_valid_name(col->name) || (col->type != TYPE_INT && col->type != TYPE_TEXT)) {
            return DB_INVALID;
        }
        for (unsigned j = 0; j < i; j++) {
            if (!strcmp(col->name, schema->columns[j].name)) {
                return DB_INVALID;
            }
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

DbStatus record_encode(const Schema *schema, const Value *values, unsigned count, uint8_t *buffer,
                       size_t capacity, uint16_t *length) {
    if (schema_validate(schema) != DB_OK || !values || !buffer || !length ||
        count != schema->count) {
        return DB_INVALID;
    }
    size_t sizes[MAX_COLUMNS], total = 0;
    for (unsigned i = 0; i < count; i++) {
        if (values[i].type != schema->columns[i].type) {
            return DB_INVALID;
        }
        sizes[i] = 8;
        if (values[i].type == TYPE_TEXT) {
            size_t n = strnlen(values[i].as.text, MAX_TEXT + 1);
            if (n > MAX_TEXT) {
                return DB_INVALID;
            }
            sizes[i] = n + 1;
        }
        total += sizes[i];
    }
    if (total > capacity) {
        return DB_INVALID;
    }
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        if (values[i].type == TYPE_INT) {
            write_u64(buffer + offset, (uint64_t)values[i].as.integer);
        } else {
            buffer[offset] = (uint8_t)(sizes[i] - 1);
            memcpy(buffer + offset + 1, values[i].as.text, sizes[i] - 1);
        }
        offset += sizes[i];
    }
    *length = (uint16_t)total;
    return DB_OK;
}

DbStatus record_decode(const Schema *schema, const uint8_t *buffer, size_t length, Value *values) {
    if (schema_validate(schema) != DB_OK || !buffer || !values) {
        return DB_INVALID;
    }
    size_t offset = 0;
    for (unsigned i = 0; i < schema->count; i++) {
        values[i].type = schema->columns[i].type;
        if (values[i].type == TYPE_INT) {
            if (length - offset < 8) {
                return DB_CORRUPT;
            }
            values[i].as.integer = read_i64(buffer + offset);
            offset += 8;
        } else {
            if (offset == length) {
                return DB_CORRUPT;
            }
            size_t size = buffer[offset++];
            if (size > length - offset || memchr(buffer + offset, 0, size)) {
                return DB_CORRUPT;
            }
            memcpy(values[i].as.text, buffer + offset, size);
            values[i].as.text[size] = '\0';
            offset += size;
        }
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
