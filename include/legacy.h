#ifndef MINIDB_LEGACY_H
#define MINIDB_LEGACY_H
#include "db.h"
DbStatus legacy_execute(Database *database, const char *command);
#endif
