#ifndef MINIDB_PAGER_H
#define MINIDB_PAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096u

typedef enum {
    DB_OK,
    DB_IO,
    DB_NOMEM,
    DB_INVALID,
    DB_CORRUPT,
    DB_NOT_FOUND,
    DB_DUPLICATE,
    DB_FULL
} DbStatus;

/* Disk integers are little-endian. */
static inline uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t read_u64(const uint8_t *p) {
    return (uint64_t)read_u32(p) | (uint64_t)read_u32(p + 4) << 32;
}

static inline int64_t read_i64(const uint8_t *p) {
    uint64_t n = read_u64(p);
    return n <= INT64_MAX ? (int64_t)n : -1 - (int64_t)(UINT64_MAX - n);
}

static inline void write_u16(uint8_t *p, uint16_t n) {
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

static inline void write_u32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(n >> (8 * i));
    }
}

static inline void write_u64(uint8_t *p, uint64_t n) {
    write_u32(p, (uint32_t)n);
    write_u32(p + 4, (uint32_t)(n >> 32));
}

enum {
    HEADER_VERSION = 8,
    HEADER_PAGES = 12,
    HEADER_CATALOG = 16,
    HEADER_FREE = 20
};

enum {
    PAGE_LEAF = 1,
    PAGE_INTERNAL = 2,
    PAGE_CATALOG = 3,
    PAGE_FREE = 4
};

typedef struct {
    uint8_t *data;
    bool dirty;
} CachedPage;

typedef struct {
    int fd;
    uint32_t page_count;
    uint8_t header[PAGE_SIZE];
    bool header_dirty;
    CachedPage *cache;
    size_t cache_capacity;
} Pager;

const char *db_status_string(DbStatus status);
DbStatus pager_open(Pager *pager, const char *path);
DbStatus pager_sync(Pager *pager);
DbStatus pager_close(Pager *pager);
DbStatus pager_get(Pager *pager, uint32_t number, uint8_t **data);
DbStatus pager_allocate(Pager *pager, uint32_t *number, uint8_t **data);
DbStatus pager_release(Pager *pager, uint32_t number);
void pager_dirty(Pager *pager, uint32_t number);
DbStatus pager_set_catalog(Pager *pager, uint32_t number);

#endif
