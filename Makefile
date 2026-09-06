CC = cc
CPPFLAGS = -Iinclude -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -O2 -g

CORE = src/table.c src/storage.c src/pager.c src/btree.c
SOURCES = src/main.c $(CORE)
CORE_OBJECTS = $(CORE:src/%.c=build/%.o)
OBJECTS = $(SOURCES:src/%.c=build/%.o)
TESTS = build/test_table build/test_storage build/test_pager build/test_btree

.PHONY: all test clean
all: minidb

minidb: $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test_%: tests/test_%.c $(CORE_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE_OBJECTS) $(LDFLAGS) -o $@

test: minidb $(TESTS)
	@for test in $(TESTS); do ./$$test || exit 1; done

clean:
	rm -rf build minidb
