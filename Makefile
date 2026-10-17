CC = cc
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS =
BUILD_DIR ?= build
TARGET ?= minidb

CORE = src/pager.c src/btree.c src/record.c src/db.c src/parser.c
SOURCES = src/main.c $(CORE)
CORE_OBJECTS = $(CORE:src/%.c=$(BUILD_DIR)/%.o)
OBJECTS = $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
TESTS = $(addprefix $(BUILD_DIR)/test_,pager btree record db parser)

.PHONY: all test clean sanitize
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

$(BUILD_DIR)/test_%: tests/test_%.c $(CORE_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE_OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/test_btree: tests/test_btree.c $(CORE_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE_OBJECTS) $(LDFLAGS) -Wl,--wrap=calloc -Wl,--wrap=malloc -o $@

test: $(TARGET) $(TESTS)
	@for test in $(TESTS); do ./$$test || exit 1; done

sanitize:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(MAKE) BUILD_DIR=build/sanitize TARGET=build/sanitize/minidb CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined' test

clean:
	rm -rf build minidb
