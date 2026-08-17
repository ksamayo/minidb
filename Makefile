CC = cc
CFLAGS = -std=c11 -Wall -D_POSIX_C_SOURCE=200809L

CORE = 
SOURCES = src/main.c $(CORE)
TESTS = 

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
