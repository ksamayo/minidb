#ifndef MINIDB_STORAGE_H
#define MINIDB_STORAGE_H

#include "table.h"

TableStatus storage_load(MemoryTable *table, const char *database_path);
TableStatus storage_save(const MemoryTable *table, const char *database_path);
#endif
