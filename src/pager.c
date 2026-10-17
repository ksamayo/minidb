#include "pager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t magic[8] = {'M', 'I', 'N', 'I', 'D', 'B', 0, 0};
enum {
    DB_FORMAT_VERSION = 1,
    FREE_PAGE_NEXT = 4,
    INITIAL_CACHE_CAPACITY = 16
};

const char *db_status_string(DbStatus status) {
    switch (status) {
        case DB_OK: {
            return "success";
        }

        case DB_IO: {
            return "file I/O error";
        }

        case DB_NOMEM: {
            return "out of memory";
        }

        case DB_INVALID: {
            return "invalid argument";
        }

        case DB_CORRUPT: {
            return "invalid or corrupt database";
        }

        case DB_NOT_FOUND: {
            return "not found";
        }

        case DB_DUPLICATE: {
            return "duplicate key or name";
        }

        case DB_FULL: {
            return "capacity exceeded";
        }
    }

    return "unknown error";
}

static DbStatus read_at(int fd, void *buffer, size_t size, off_t offset) {
    uint8_t *cursor = buffer;

    while (size > 0) {
        ssize_t bytes_read = pread(fd, cursor, size, offset);

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return DB_IO;
        }

        if (bytes_read == 0) {
            return DB_CORRUPT;
        }

        cursor += bytes_read;
        size -= (size_t)bytes_read;
        offset += bytes_read;
    }

    return DB_OK;
}

static DbStatus write_at(int fd, const void *buffer, size_t size, off_t offset) {
    const uint8_t *cursor = buffer;

    while (size > 0) {
        ssize_t bytes_written = pwrite(fd, cursor, size, offset);

        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }

            return DB_IO;
        }

        if (bytes_written == 0) {
            return DB_IO;
        }

        cursor += bytes_written;
        size -= (size_t)bytes_written;
        offset += bytes_written;
    }

    return DB_OK;
}

static bool pager_init(Pager *pager, const char *path) {
    memset(pager, 0, sizeof(*pager));
    pager->fd = open(path, O_RDWR | O_CREAT, 0600);

    return pager->fd >= 0;
}

static DbStatus pager_init_file(Pager *pager) {
    memcpy(pager->header, magic, sizeof(magic));

    write_u32(pager->header + HEADER_VERSION, DB_FORMAT_VERSION);
    write_u32(pager->header + HEADER_PAGES, 1);

    pager->page_count = 1;
    pager->header_dirty = true;

    return pager_sync(pager);
}

static DbStatus pager_count_pages(off_t file_size, uint32_t *page_count) {
    if (file_size <= 0 || file_size % PAGE_SIZE != 0) {
        return DB_CORRUPT;
    }

    uint64_t count = (uint64_t)file_size / PAGE_SIZE;

    if (count > UINT32_MAX) {
        return DB_CORRUPT;
    }

    *page_count = (uint32_t)count;

    return DB_OK;
}

static DbStatus pager_validate_header(const uint8_t *header, uint32_t page_count) {
    if (memcmp(header, magic, sizeof(magic)) != 0) {
        return DB_CORRUPT;
    }

    if (read_u32(header + HEADER_VERSION) != DB_FORMAT_VERSION) {
        return DB_CORRUPT;
    }

    if (read_u32(header + HEADER_PAGES) != page_count) {
        return DB_CORRUPT;
    }

    uint32_t catalog_page = read_u32(header + HEADER_CATALOG);
    uint32_t first_free_page = read_u32(header + HEADER_FREE);

    if (catalog_page >= page_count || first_free_page >= page_count) {
        return DB_CORRUPT;
    }

    return DB_OK;
}

static DbStatus pager_load_file(Pager *pager, off_t file_size) {
    uint32_t page_count;
    DbStatus status = pager_count_pages(file_size, &page_count);

    if (status != DB_OK) {
        return status;
    }

    status = read_at(pager->fd, pager->header, PAGE_SIZE, 0);

    if (status != DB_OK) {
        return status;
    }

    status = pager_validate_header(pager->header, page_count);

    if (status != DB_OK) {
        return status;
    }

    pager->page_count = page_count;

    return DB_OK;
}

DbStatus pager_open(Pager *pager, const char *path) {
    if (!pager || !path) {
        return DB_INVALID;
    }

    if (!pager_init(pager, path)) {
        return DB_IO;
    }

    struct stat file_info;
    DbStatus status = DB_IO;

    if (flock(pager->fd, LOCK_EX | LOCK_NB) < 0) {
        goto fail;
    }

    if (fstat(pager->fd, &file_info) < 0) {
        goto fail;
    }

    if (file_info.st_size == 0) {
        status = pager_init_file(pager);
    } else {
        status = pager_load_file(pager, file_info.st_size);
    }

    if (status != DB_OK) {
        goto fail;
    }

    return DB_OK;

fail:
    close(pager->fd);
    pager->fd = -1;

    return status;
}

static DbStatus cache_capacity_for(size_t current_capacity, size_t needed, size_t *capacity) {
    const size_t max_capacity = SIZE_MAX / sizeof(CachedPage);

    if (needed > max_capacity) {
        return DB_NOMEM;
    }

    size_t new_capacity = current_capacity;

    if (new_capacity == 0) {
        new_capacity = INITIAL_CACHE_CAPACITY;
    }

    while (new_capacity < needed) {
        if (new_capacity > max_capacity / 2) {
            new_capacity = needed;

            break;
        }

        new_capacity *= 2;
    }

    *capacity = new_capacity;

    return DB_OK;
}

static DbStatus grow_cache(Pager *pager, size_t needed) {
    const size_t old_capacity = pager->cache_capacity;

    if (needed <= old_capacity) {
        return DB_OK;
    }

    size_t new_capacity;
    DbStatus status = cache_capacity_for(old_capacity, needed, &new_capacity);

    if (status != DB_OK) {
        return status;
    }

    CachedPage *new_cache = realloc(pager->cache, new_capacity * sizeof(*new_cache));

    if (!new_cache) {
        return DB_NOMEM;
    }

    const size_t added_capacity = new_capacity - old_capacity;

    memset(new_cache + old_capacity, 0, added_capacity * sizeof(*new_cache));

    pager->cache = new_cache;
    pager->cache_capacity = new_capacity;

    return DB_OK;
}

static DbStatus pager_cache_page(Pager *pager, uint32_t number) {
    DbStatus status = grow_cache(pager, (size_t)number + 1);

    if (status != DB_OK) {
        return status;
    }

    CachedPage *page = &pager->cache[number];

    if (page->data) {
        return DB_OK;
    }

    uint8_t *data = malloc(PAGE_SIZE);

    if (!data) {
        return DB_NOMEM;
    }

    status = read_at(pager->fd, data, PAGE_SIZE, (off_t)number * PAGE_SIZE);

    if (status != DB_OK) {
        free(data);

        return status;
    }

    page->data = data;

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

    DbStatus status = pager_cache_page(pager, number);

    if (status != DB_OK) {
        return status;
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

static DbStatus pager_reuse_page(Pager *pager, uint32_t *number, uint8_t **data) {
    uint32_t first_free_page = read_u32(pager->header + HEADER_FREE);
    DbStatus status = pager_get(pager, first_free_page, data);

    if (status != DB_OK) {
        return status;
    }

    uint32_t next_free_page = read_u32(*data + FREE_PAGE_NEXT);

    if ((*data)[0] != PAGE_FREE || next_free_page >= pager->page_count ||
        next_free_page == first_free_page) {
        return DB_CORRUPT;
    }

    write_u32(pager->header + HEADER_FREE, next_free_page);
    memset(*data, 0, PAGE_SIZE);
    *number = first_free_page;

    return DB_OK;
}

static DbStatus pager_append_page(Pager *pager, uint32_t *number, uint8_t **data) {
    if (pager->page_count == UINT32_MAX) {
        return DB_FULL;
    }

    uint32_t new_page_number = pager->page_count;
    DbStatus status = grow_cache(pager, (size_t)new_page_number + 1);

    if (status != DB_OK) {
        return status;
    }

    *data = calloc(1, PAGE_SIZE);

    if (!*data) {
        return DB_NOMEM;
    }

    pager->cache[new_page_number].data = *data;
    pager->page_count++;
    write_u32(pager->header + HEADER_PAGES, pager->page_count);

    *number = new_page_number;

    return DB_OK;
}

DbStatus pager_allocate(Pager *pager, uint32_t *number, uint8_t **data) {
    if (!pager || pager->fd < 0 || !number || !data) {
        return DB_INVALID;
    }

    DbStatus status;

    if (read_u32(pager->header + HEADER_FREE) != 0) {
        status = pager_reuse_page(pager, number, data);
    } else {
        status = pager_append_page(pager, number, data);
    }

    if (status != DB_OK) {
        return status;
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
    write_u32(page + FREE_PAGE_NEXT, read_u32(pager->header + HEADER_FREE));
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

static DbStatus pager_write_dirty_pages(Pager *pager) {
    for (size_t i = 1; i < pager->cache_capacity; i++) {
        const CachedPage *page = &pager->cache[i];

        if (!page->dirty) {
            continue;
        }

        DbStatus status = write_at(pager->fd, page->data, PAGE_SIZE, (off_t)i * PAGE_SIZE);

        if (status != DB_OK) {
            return status;
        }
    }

    if (pager->header_dirty) {
        return write_at(pager->fd, pager->header, PAGE_SIZE, 0);
    }

    return DB_OK;
}

static void pager_mark_clean(Pager *pager) {
    pager->header_dirty = false;

    for (size_t i = 1; i < pager->cache_capacity; i++) {
        pager->cache[i].dirty = false;
    }
}

DbStatus pager_sync(Pager *pager) {
    if (!pager || pager->fd < 0) {
        return DB_INVALID;
    }

    DbStatus status = pager_write_dirty_pages(pager);

    if (status != DB_OK) {
        return status;
    }

    if (fsync(pager->fd) < 0) {
        return DB_IO;
    }

    pager_mark_clean(pager);

    return DB_OK;
}

static void pager_free_cache(Pager *pager) {
    for (size_t i = 0; i < pager->cache_capacity; i++) {
        free(pager->cache[i].data);
    }

    free(pager->cache);
    pager->cache = NULL;
    pager->cache_capacity = 0;
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
    pager_free_cache(pager);

    return status;
}
