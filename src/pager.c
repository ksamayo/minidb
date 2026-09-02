#include "pager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t magic[8] = {'M', 'I', 'N', 'I', 'D', 'B', 0, 0};

const char *db_status_string(DbStatus status) {
    switch (status) {
        case DB_OK:
            return "success";
        case DB_IO:
            return "file I/O error (or database already open)";
        case DB_NOMEM:
            return "out of memory";
        case DB_INVALID:
            return "invalid argument";
        case DB_CORRUPT:
            return "invalid or corrupt database";
        case DB_NOT_FOUND:
            return "not found";
        case DB_DUPLICATE:
            return "duplicate key or name";
        case DB_FULL:
            return "capacity exceeded";
    }
    return "unknown error";
}

static DbStatus read_at(int fd, void *buffer, size_t size, off_t offset) {
    uint8_t *p = buffer;
    while (size) {
        ssize_t n = pread(fd, p, size, offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return n == 0 ? DB_CORRUPT : DB_IO;
        }
        p += n;
        size -= (size_t)n;
        offset += n;
    }
    return DB_OK;
}

static DbStatus write_at(int fd, const void *buffer, size_t size, off_t offset) {
    const uint8_t *p = buffer;
    while (size) {
        ssize_t n = pwrite(fd, p, size, offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return DB_IO;
        }
        p += n;
        size -= (size_t)n;
        offset += n;
    }
    return DB_OK;
}

DbStatus pager_open(Pager *pager, const char *path) {
    if (!pager || !path) {
        return DB_INVALID;
    }
    memset(pager, 0, sizeof(*pager));
    pager->fd = open(path, O_RDWR | O_CREAT, 0600);
    if (pager->fd < 0) {
        return DB_IO;
    }
    struct stat st;
    DbStatus status = DB_IO;
    if (flock(pager->fd, LOCK_EX | LOCK_NB) < 0 || fstat(pager->fd, &st) < 0) {
        goto fail;
    }
    if (st.st_size == 0) {
        memcpy(pager->header, magic, sizeof(magic));
        write_u32(pager->header + HEADER_VERSION, 1);
        write_u32(pager->header + HEADER_PAGES, 1);
        pager->page_count = 1;
        pager->header_dirty = true;
        status = pager_sync(pager);
        if (status != DB_OK) {
            goto fail;
        }
        return DB_OK;
    }
    status = DB_CORRUPT;
    if (st.st_size % PAGE_SIZE || (uint64_t)st.st_size / PAGE_SIZE > UINT32_MAX) {
        goto fail;
    }
    status = read_at(pager->fd, pager->header, PAGE_SIZE, 0);
    if (status != DB_OK) {
        goto fail;
    }
    pager->page_count = (uint32_t)(st.st_size / PAGE_SIZE);
    status = DB_CORRUPT;
    if (memcmp(pager->header, magic, sizeof(magic)) ||
        read_u32(pager->header + HEADER_VERSION) != 1 ||
        read_u32(pager->header + HEADER_PAGES) != pager->page_count ||
        read_u32(pager->header + HEADER_CATALOG) >= pager->page_count ||
        read_u32(pager->header + HEADER_FREE) >= pager->page_count) {
        goto fail;
    }
    return DB_OK;

fail:
    close(pager->fd);
    pager->fd = -1;
    return status;
}

static DbStatus grow_cache(Pager *pager, size_t needed) {
    if (needed <= pager->cache_capacity) {
        return DB_OK;
    }
    size_t capacity = pager->cache_capacity ? pager->cache_capacity : 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2 / sizeof(CachedPage)) {
            return DB_NOMEM;
        }
        capacity *= 2;
    }
    CachedPage *cache = realloc(pager->cache, capacity * sizeof(*cache));
    if (!cache) {
        return DB_NOMEM;
    }
    memset(cache + pager->cache_capacity, 0, (capacity - pager->cache_capacity) * sizeof(*cache));
    pager->cache = cache;
    pager->cache_capacity = capacity;
    return DB_OK;
}

DbStatus pager_get(Pager *pager, uint32_t number, uint8_t **data) {
    if (!pager || pager->fd < 0 || !data || number >= pager->page_count) {
        return DB_INVALID;
    }
    if (number == 0) {
        *data = pager->header;
        return DB_OK;
    }
    DbStatus status = grow_cache(pager, (size_t)number + 1);
    if (status != DB_OK) {
        return status;
    }
    if (!pager->cache[number].data) {
        uint8_t *buffer = malloc(PAGE_SIZE);
        if (!buffer) {
            return DB_NOMEM;
        }
        status = read_at(pager->fd, buffer, PAGE_SIZE, (off_t)number * PAGE_SIZE);
        if (status != DB_OK) {
            free(buffer);
            return status;
        }
        pager->cache[number].data = buffer;
    }
    *data = pager->cache[number].data;
    return DB_OK;
}

void pager_dirty(Pager *pager, uint32_t number) {
    if (number == 0) {
        pager->header_dirty = true;
    } else {
        pager->cache[number].dirty = true;
    }
}

DbStatus pager_allocate(Pager *pager, uint32_t *number, uint8_t **data) {
    if (!pager || pager->fd < 0 || !number || !data) {
        return DB_INVALID;
    }
    uint32_t head = read_u32(pager->header + HEADER_FREE);
    if (head) {
        DbStatus status = pager_get(pager, head, data);
        if (status != DB_OK) {
            return status;
        }
        uint32_t next = read_u32(*data + 4);
        if ((*data)[0] != PAGE_FREE || next >= pager->page_count || next == head) {
            return DB_CORRUPT;
        }
        write_u32(pager->header + HEADER_FREE, next);
        *number = head;
        memset(*data, 0, PAGE_SIZE);
    } else {
        if (pager->page_count == UINT32_MAX) {
            return DB_FULL;
        }
        uint32_t id = pager->page_count;
        DbStatus status = grow_cache(pager, (size_t)id + 1);
        if (status != DB_OK) {
            return status;
        }
        *data = calloc(1, PAGE_SIZE);
        if (!*data) {
            return DB_NOMEM;
        }
        pager->cache[id].data = *data;
        *number = id;
        pager->page_count++;
        write_u32(pager->header + HEADER_PAGES, pager->page_count);
    }
    pager_dirty(pager, *number);
    pager->header_dirty = true;
    return DB_OK;
}

DbStatus pager_release(Pager *pager, uint32_t number) {
    if (!number) {
        return DB_INVALID;
    }
    uint8_t *page;
    DbStatus status = pager_get(pager, number, &page);
    if (status != DB_OK) {
        return status;
    }
    if (page[0] == PAGE_FREE) {
        return DB_INVALID;
    }
    memset(page, 0, PAGE_SIZE);
    page[0] = PAGE_FREE;
    write_u32(page + 4, read_u32(pager->header + HEADER_FREE));
    write_u32(pager->header + HEADER_FREE, number);
    pager_dirty(pager, number);
    pager->header_dirty = true;
    return DB_OK;
}

DbStatus pager_set_catalog(Pager *pager, uint32_t number) {
    if (!pager || pager->fd < 0 || number >= pager->page_count) {
        return DB_INVALID;
    }
    write_u32(pager->header + HEADER_CATALOG, number);
    pager->header_dirty = true;
    return DB_OK;
}

DbStatus pager_sync(Pager *pager) {
    if (!pager || pager->fd < 0) {
        return DB_INVALID;
    }
    for (size_t i = 1; i < pager->cache_capacity; i++) {
        if (pager->cache[i].dirty) {
            DbStatus status =
                write_at(pager->fd, pager->cache[i].data, PAGE_SIZE, (off_t)i * PAGE_SIZE);
            if (status != DB_OK) {
                return status;
            }
        }
    }
    if (pager->header_dirty) {
        DbStatus status = write_at(pager->fd, pager->header, PAGE_SIZE, 0);
        if (status != DB_OK) {
            return status;
        }
    }
    if (fsync(pager->fd) < 0) {
        return DB_IO;
    }
    pager->header_dirty = false;
    for (size_t i = 1; i < pager->cache_capacity; i++) {
        pager->cache[i].dirty = false;
    }
    return DB_OK;
}

DbStatus pager_close(Pager *pager) {
    if (!pager || pager->fd < 0) {
        return DB_INVALID;
    }
    DbStatus status = pager_sync(pager);
    if (close(pager->fd) < 0 && status == DB_OK) {
        status = DB_IO;
    }
    pager->fd = -1;
    for (size_t i = 0; i < pager->cache_capacity; i++) {
        free(pager->cache[i].data);
    }
    free(pager->cache);
    pager->cache = NULL;
    pager->cache_capacity = 0;
    return status;
}
