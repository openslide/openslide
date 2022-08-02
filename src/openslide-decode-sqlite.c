/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2013 Carnegie Mellon University
 *  Copyright (c) 2022 Benjamin Gilbert
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

/* Can only use API supported in SQLite 3.26.0 for RHEL 8 compatibility */

typedef char sqlite_char;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(sqlite_char, sqlite3_free)

static int profile_callback(unsigned trace_type G_GNUC_UNUSED,
                            void *ctx G_GNUC_UNUSED,
                            void *stmt,
                            void *ns) {
  sqlite3_stmt *stmtp = stmt;
  sqlite3_int64 *nsp = ns;
  uint64_t ms = *nsp / 1e6;
  g_autoptr(sqlite_char) sql = sqlite3_expanded_sql(stmtp);
  g_debug("%s --> %"PRIu64" ms", sql, ms);
  return 0;
}

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

  if (_openslide_debug(OPENSLIDE_DEBUG_SQL)) {
    sqlite3_trace_v2(db, SQLITE_TRACE_PROFILE, profile_callback, NULL);
  }

  return db;
}
#define sqlite3_open_v2 _OPENSLIDE_POISON(_openslide_sqlite_open)

sqlite3 *_openslide_sqlite_open(const char *filename, GError **err) {
  // ":" filename prefix is reserved.
  // "file:" prefix invokes URI filename interpretation if enabled, which
  // might have been done globally.
  g_autofree char *path = NULL;
  if (g_str_has_prefix(filename, ":") || g_str_has_prefix(filename, "file:")) {
    path = g_strdup_printf("./%s", filename);
  } else {
    path = g_strdup(filename);
  }
  sqlite3 *db = do_open(path, SQLITE_OPEN_READONLY, err);
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

// wrapper that returns void for g_autoptr
void _openslide_sqlite_finalize(sqlite3_stmt *stmt) {
  sqlite3_finalize(stmt);
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
