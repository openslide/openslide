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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_SQLITE_H_
#define OPENSLIDE_OPENSLIDE_DECODE_SQLITE_H_

#include <stdbool.h>
#include <glib.h>
#include <sqlite3.h>

/* SQLite support code */

sqlite3 *_openslide_sqlite_open(const char *filename, GError **err);
sqlite3_stmt *_openslide_sqlite_prepare(sqlite3 *db, const char *sql,
                                        GError **err);
bool _openslide_sqlite_step(sqlite3_stmt *stmt, GError **err);
void _openslide_sqlite_finalize(sqlite3_stmt *stmt);
void _openslide_sqlite_propagate_error(sqlite3 *db, GError **err);
void _openslide_sqlite_propagate_stmt_error(sqlite3_stmt *stmt, GError **err);
void _openslide_sqlite_close(sqlite3 *db);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(sqlite3, _openslide_sqlite_close)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(sqlite3_stmt, _openslide_sqlite_finalize)

#endif
