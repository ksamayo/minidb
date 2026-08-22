CC = cc
CFLAGS = -std=c11 -Wall -g -Iinclude -D_POSIX_C_SOURCE=200809L

CORE = src/table.c src/storage.c
SOURCES = src/main.c $(CORE)
TESTS = build/test_table

.PHONY: all test clean
all: minidb

minidb: $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

build/test_%: tests/test_%.c $(CORE)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test: minidb $(TESTS)
	@for test in $(TESTS); do ./$$test || exit 1; done

clean:
	rm -rf build minidb
