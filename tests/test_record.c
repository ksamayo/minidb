#include "record.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t test_record_round_trip(const Schema *schema, const Value *input_values,
                                       uint8_t *encoded_record, Value *decoded_values) {
    uint16_t encoded_length;

    assert(record_max_size(schema) == 272);
    assert(record_encode(schema, input_values, 3, encoded_record, MAX_RECORD_SIZE,
                         &encoded_length) == DB_OK);
    assert(encoded_length == 24);
    assert(encoded_record[7] == 0x80 && encoded_record[8] == 7);
    assert(record_decode(schema, encoded_record, encoded_length, decoded_values) == DB_OK);
    assert(decoded_values[0].as.integer == INT64_MIN && decoded_values[2].as.integer == INT64_MAX);
    assert(!strcmp(decoded_values[1].as.text, "O'Brien"));

    return encoded_length;
}

static void test_invalid_record_lengths(const Schema *schema, const uint8_t *encoded_record,
                                        uint16_t encoded_length, Value *decoded_values) {
    for (unsigned truncated_length = 0; truncated_length < encoded_length; truncated_length++) {
        assert(record_decode(schema, encoded_record, truncated_length, decoded_values) ==
               DB_CORRUPT);
    }

    assert(record_decode(schema, encoded_record, encoded_length + 1, decoded_values) == DB_CORRUPT);
}

static void test_text_length_limits(const Schema *schema, Value *input_values,
                                    uint8_t *encoded_record, Value *decoded_values) {
    uint16_t encoded_length;

    memset(input_values[1].as.text, 'x', MAX_TEXT);
    input_values[1].as.text[MAX_TEXT] = '\0';
    assert(record_encode(schema, input_values, 3, encoded_record, MAX_RECORD_SIZE,
                         &encoded_length) == DB_OK);
    assert(encoded_length == 272 &&
           record_decode(schema, encoded_record, encoded_length, decoded_values) == DB_OK);
    assert(strlen(decoded_values[1].as.text) == MAX_TEXT);
    assert(record_encode(schema, input_values, 3, encoded_record, 10, &encoded_length) ==
           DB_INVALID);

    input_values[1].as.text[MAX_TEXT] = 'x';
    assert(record_encode(schema, input_values, 3, encoded_record, MAX_RECORD_SIZE,
                         &encoded_length) == DB_INVALID);
}

static void test_invalid_value_type_and_count(const Schema *schema, Value *input_values,
                                              uint8_t *encoded_record) {
    uint16_t encoded_length;

    input_values[1].type = TYPE_INT;
    assert(record_encode(schema, input_values, 3, encoded_record, MAX_RECORD_SIZE,
                         &encoded_length) == DB_INVALID);
    assert(record_encode(schema, input_values, 2, encoded_record, MAX_RECORD_SIZE,
                         &encoded_length) == DB_INVALID);
}

static void test_invalid_schema_and_names(Schema *schema) {
    strcpy(schema->columns[2].name, "name");
    assert(schema_validate(schema) == DB_INVALID);

    strcpy(schema->columns[0].name, "other");
    assert(schema_validate(schema) == DB_INVALID);
    assert(!record_valid_name("bad-name") && !record_valid_name("1bad"));
}

int main(void) {
    Schema schema = {3, {{"id", TYPE_INT}, {"name", TYPE_TEXT}, {"age", TYPE_INT}}};
    Value input_values[MAX_COLUMNS] = {{.type = TYPE_INT, .as.integer = INT64_MIN},
                                       {.type = TYPE_TEXT, .as.text = "O'Brien"},
                                       {.type = TYPE_INT, .as.integer = INT64_MAX}};
    Value decoded_values[MAX_COLUMNS];
    uint8_t encoded_record[MAX_RECORD_SIZE];

    uint16_t encoded_length =
        test_record_round_trip(&schema, input_values, encoded_record, decoded_values);
    test_invalid_record_lengths(&schema, encoded_record, encoded_length, decoded_values);
    test_text_length_limits(&schema, input_values, encoded_record, decoded_values);
    test_invalid_value_type_and_count(&schema, input_values, encoded_record);
    test_invalid_schema_and_names(&schema);

    puts("record: OK");
    return 0;
}
