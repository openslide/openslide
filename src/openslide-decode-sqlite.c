/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2013 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>
#include <string.h>

#include "openslide-decode-sqlite.h"
#include "openslide-private.h"

#define BUSY_TIMEOUT 500  // ms
#define PROFILE 0

/* Can only use API supported in SQLite 3.6.20 for RHEL 6 compatibility */

#if PROFILE
// We would like to put this behind a debug flag, but sqlite3_profile() is
// marked experimental, so we would be risking future build breakage.
static void profile_callback(void *arg G_GNUC_UNUSED, const char *sql,
                             sqlite3_uint64 ns) {
  uint64_t ms = ns / 1e6;
  g_debug("%s --> %"PRIu64" ms", sql, ms);
}
#endif

#undef sqlite3_open_v2
static sqlite3 *do_open(const char *filename, int flags, GError **err) {
  sqlite3 *db;

  int ret = sqlite3_initialize();
  if (ret) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't initialize SQLite: %d", ret);
    return NULL;
  }

  ret = sqlite3_open_v2(filename, &db, flags, NULL);

  if (ret) {
    if (db) {
      _openslide_sqlite_propagate_error(db, err);
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't open %s: %d", filename, ret);
    }
    _openslide_sqlite_close(db);
    return NULL;
  }

  sqlite3_busy_timeout(db, BUSY_TIMEOUT);

#if PROFILE
  sqlite3_profile(db, profile_callback, NULL);
#endif

  return db;
}
#define sqlite3_open_v2 _OPENSLIDE_POISON(_openslide_sqlite_open)

sqlite3 *_openslide_sqlite_open(const char *filename, GError **err) {
  // ":" filename prefix is reserved.
  // "file:" prefix invokes URI filename interpretation if enabled, which
  // might have been done globally.
  char *path;
  if (g_str_has_prefix(filename, ":") || g_str_has_prefix(filename, "file:")) {
    path = g_strdup_printf("./%s", filename);
  } else {
    path = g_strdup(filename);
  }
  sqlite3 *db = do_open(path, SQLITE_OPEN_READONLY, err);
  g_free(path);
  return db;
}

sqlite3_stmt *_openslide_sqlite_prepare(sqlite3 *db, const char *sql,
                                        GError **err) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, strlen(sql) + 1, &stmt, NULL)) {
    _openslide_sqlite_propagate_error(db, err);
  }
  return stmt;
}

bool _openslide_sqlite_step(sqlite3_stmt *stmt, GError **err) {
  switch (sqlite3_step(stmt)) {
  case SQLITE_ROW:
    return true;
  case SQLITE_DONE:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE,
                "Query returned no value: %s", sqlite3_sql(stmt));
    return false;
  default:
    _openslide_sqlite_propagate_stmt_error(stmt, err);
    return false;
  }
}

// only legal if an error occurred
void _openslide_sqlite_propagate_error(sqlite3 *db, GError **err) {
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "SQLite error: %s", sqlite3_errmsg(db));
}

// only legal if an error occurred
void _openslide_sqlite_propagate_stmt_error(sqlite3_stmt *stmt, GError **err) {
  _openslide_sqlite_propagate_error(sqlite3_db_handle(stmt), err);
}

#undef sqlite3_close
void _openslide_sqlite_close(sqlite3 *db) {
  // sqlite3_close() failures indicate a leaked resource, probably a
  // prepared statement.
  if (sqlite3_close(db)) {
    g_warning("SQLite error: %s", sqlite3_errmsg(db));
  }
}
#define sqlite3_close _OPENSLIDE_POISON(_openslide_sqlite_close)
