/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
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
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-tiffdump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <glib.h>

#include <tiffio.h>

#ifndef TIFF_VERSION
// renamed in libtiff 4
#define TIFF_VERSION TIFF_VERSION_CLASSIC
#endif


static void fix_byte_order(void *data, int32_t size, int64_t count,
                           bool big_endian) {
  switch (size) {
  case 1: {
    break;
  }
  case 2: {
    uint16_t *arr = data;
    for (int64_t i = 0; i < count; i++) {
      arr[i] = big_endian ? GUINT16_FROM_BE(arr[i]) : GUINT16_FROM_LE(arr[i]);
    }
    break;
  }
  case 4: {
    uint32_t *arr = data;
    for (int64_t i = 0; i < count; i++) {
      arr[i] = big_endian ? GUINT32_FROM_BE(arr[i]) : GUINT32_FROM_LE(arr[i]);
    }
    break;
  }
  case 8: {
    uint64_t *arr = data;
    for (int64_t i = 0; i < count; i++) {
      arr[i] = big_endian ? GUINT64_FROM_BE(arr[i]) : GUINT64_FROM_LE(arr[i]);
    }
    break;
  }
  default:
    g_assert_not_reached();
    break;
  }
}

// only sets *ok on failure
static uint64_t read_uint(FILE *f, int32_t size, bool big_endian, bool *ok) {
  g_assert(ok != NULL);

  uint8_t buf[size];
  if (fread(buf, size, 1, f) != 1) {
    *ok = false;
    return 0;
  }
  fix_byte_order(buf, sizeof(buf), 1, big_endian);
  switch (size) {
  case 1: {
    uint8_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  case 2: {
    uint16_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  case 4: {
    uint32_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  case 8: {
    uint64_t result;
    memcpy(&result, buf, sizeof(result));
    return result;
  }
  default:
    g_assert_not_reached();
  }
}

static void *read_tiff_tag(FILE *f, int32_t size, int64_t count,
                           int64_t offset, uint8_t value[],
                           bool big_endian) {
  if (size <= 0 || count <= 0 || count > SSIZE_MAX / size) {
    return NULL;
  }
  ssize_t len = size * count;

  void *result = g_try_malloc(len);
  if (result == NULL) {
    return NULL;
  }

  //g_debug("reading tiff tag: len: %"G_GINT64_FORMAT", value/offset %u", len, (unsigned) offset);
  if (len <= 4) {
    // inline
    memcpy(result, value, len);
  } else {
    int64_t old_off = ftello(f);
    if (fseeko(f, offset, SEEK_SET) != 0) {
      goto FAIL;
    }
    if (fread(result, len, 1, f) != 1) {
      goto FAIL;
    }
    fseeko(f, old_off, SEEK_SET);
  }

  fix_byte_order(result, size, count, big_endian);

  return result;

 FAIL:
  g_free(result);
  return NULL;
}

static void tiffdump_item_destroy(gpointer data) {
  struct _openslide_tiffdump_item *td = data;

  g_free(td->value);
  g_slice_free(struct _openslide_tiffdump_item, td);
}

static GHashTable *read_directory(FILE *f, uint32_t *diroff,
				  GHashTable *loop_detector,
				  bool big_endian,
				  GError **err) {
  uint32_t off = *diroff;
  *diroff = 0;
  GHashTable *result = NULL;
  int64_t *key = NULL;
  bool ok = true;

  //  g_debug("diroff: %" PRId64, off);

  if (off <= 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Bad offset");
    goto FAIL;
  }

  // loop detection
  if (g_hash_table_lookup_extended(loop_detector, &off, NULL, NULL)) {
    // loop
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Loop detected");
    goto FAIL;
  }
  key = g_slice_new(int64_t);
  *key = off;
  g_hash_table_insert(loop_detector, key, NULL);

  // no loop, let's seek
  if (fseeko(f, off, SEEK_SET) != 0) {
    _openslide_io_error(err, "Cannot seek to offset");
    goto FAIL;
  }

  // read directory count
  uint16_t dircount = read_uint(f, 2, big_endian, &ok);
  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot read dircount");
    goto FAIL;
  }

  //  g_debug("dircount: %d", dircount);


  // initial checks passed, initialized the hashtable
  result = g_hash_table_new_full(g_int_hash, g_int_equal,
				 g_free, tiffdump_item_destroy);

  // read all directory entries
  for (int i = 0; i < dircount; i++) {
    uint16_t tag = read_uint(f, 2, big_endian, &ok);
    uint16_t type = read_uint(f, 2, big_endian, &ok);
    uint32_t count = read_uint(f, 4, big_endian, &ok);

    if (!ok) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot read tag, type, and count");
      goto FAIL;
    }

    //    g_debug(" tag: %d, type: %d, count: %" PRId64, tag, type, count);

    // read in the value/offset
    uint8_t value[4];
    if (fread(value, 1, 4, f) != 4) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot read value/offset");
      goto FAIL;
    }

    uint32_t offset;
    memcpy(&offset, value, 4);
    fix_byte_order(&offset, sizeof(offset), 1, big_endian);

    // allocate the item
    struct _openslide_tiffdump_item *data =
      g_slice_new(struct _openslide_tiffdump_item);
    data->type = (TIFFDataType) type;
    data->count = count;

    // load the value
    switch (type) {
    case TIFF_BYTE:
    case TIFF_ASCII:
    case TIFF_SBYTE:
    case TIFF_UNDEFINED:
      data->value = read_tiff_tag(f, 1, count, offset, value, big_endian);
      break;

    case TIFF_SHORT:
    case TIFF_SSHORT:
      data->value = read_tiff_tag(f, 2, count, offset, value, big_endian);
      break;

    case TIFF_LONG:
    case TIFF_SLONG:
    case TIFF_FLOAT:
    case TIFF_IFD:
      data->value = read_tiff_tag(f, 4, count, offset, value, big_endian);
      break;

    case TIFF_RATIONAL:
    case TIFF_SRATIONAL:
      data->value = read_tiff_tag(f, 4, count * 2, offset, value, big_endian);
      break;

    case TIFF_DOUBLE:
      data->value = read_tiff_tag(f, 8, count, offset, value, big_endian);
      break;

    default:
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unknown type encountered: %d", type);
      goto FAIL;
    }

    if (data->value == NULL) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot read value");
      goto FAIL;
    }

    // add this tag to the hashtable
    int *key = g_new(int, 1);
    *key = tag;
    g_hash_table_insert(result, key, data);
  }

  // read the next dir offset
  uint32_t nextdiroff = read_uint(f, 4, big_endian, &ok);
  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot read next directory offset");
    goto FAIL;
  }
  *diroff = nextdiroff;

  // success
  return result;


 FAIL:
  if (result != NULL) {
    g_hash_table_unref(result);
  }
  return NULL;
}

// returns list of hashtables of (int -> struct _openslide_tiffdump_item)
GSList *_openslide_tiffdump_create(FILE *f, GError **err) {
  // read and check magic
  uint16_t magic;
  fseeko(f, 0, SEEK_SET);
  if (fread(&magic, sizeof magic, 1, f) != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Can't read TIFF magic number");
    return NULL;
  }
  if (magic != TIFF_BIGENDIAN && magic != TIFF_LITTLEENDIAN) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unrecognized TIFF magic number");
    return NULL;
  }
  bool big_endian = (magic == TIFF_BIGENDIAN);

  //  g_debug("magic: %d", magic);

  bool ok = true;
  uint16_t version = read_uint(f, 2, big_endian, &ok);
  uint32_t diroff = read_uint(f, 4, big_endian, &ok);

  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Can't read TIFF header");
    return NULL;
  }

  //  g_debug("version: %d", version);

  if (version != TIFF_VERSION) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unrecognized TIFF version");
    return NULL;
  }

  // initialize loop detector
  GHashTable *loop_detector = g_hash_table_new_full(_openslide_int64_hash,
						    _openslide_int64_equal,
						    _openslide_int64_free,
						    NULL);
  // read all the directories
  GSList *result = NULL;
  while (diroff != 0) {
    // read a directory
    GHashTable *ht = read_directory(f, &diroff, loop_detector, big_endian, err);

    // was the directory successfully read?
    if (ht == NULL) {
      // no, so destroy everything
      _openslide_tiffdump_destroy(result);
      result = NULL;
      break;
    }

    // add result to list
    result = g_slist_prepend(result, ht);
  }

  g_hash_table_unref(loop_detector);
  return g_slist_reverse(result);
}


void _openslide_tiffdump_destroy(GSList *tiffdump) {
  while (tiffdump != NULL) {
    GHashTable *ht = tiffdump->data;
    g_hash_table_unref(ht);

    tiffdump = g_slist_delete_link(tiffdump, tiffdump);
  }
}

static void print_tag(int tag, struct _openslide_tiffdump_item *item) {
  printf(" %d: type: %d, count: %" G_GINT64_FORMAT "\n ", tag, item->type, item->count);

  if (item->type == TIFF_ASCII) {
    // will only print first string if there are multiple
    const char *str = _openslide_tiffdump_get_buffer(item);
    if (str[item->count - 1] != '\0') {
      str = "<not null-terminated>";
    }
    printf(" %s", str);
  } else if (item->type == TIFF_UNDEFINED) {
    const uint8_t *data = _openslide_tiffdump_get_buffer(item);
    for (int64_t i = 0; i < item->count; i++) {
      printf(" %u", data[i]);
    }
  } else {
    for (int64_t i = 0; i < item->count; i++) {
      switch (item->type) {
      case TIFF_BYTE:
      case TIFF_SHORT:
      case TIFF_LONG:
	printf(" %" G_GUINT64_FORMAT, _openslide_tiffdump_get_uint(item, i));
	break;

      case TIFF_IFD:
	printf(" %.16" G_GINT64_MODIFIER "x",
	       _openslide_tiffdump_get_uint(item, i));
	break;

      case TIFF_SBYTE:
      case TIFF_SSHORT:
      case TIFF_SLONG:
	printf(" %" G_GINT64_FORMAT, _openslide_tiffdump_get_sint(item, i));
	break;

      case TIFF_FLOAT:
      case TIFF_DOUBLE:
      case TIFF_RATIONAL:
      case TIFF_SRATIONAL:
	printf(" %g", _openslide_tiffdump_get_float(item, i));
	break;

      default:
	g_return_if_reached();
      }
    }
  }
  printf("\n");
}

struct hash_key_helper {
  int i;
  int *tags;
};

static int int_compare(const void *a, const void *b) {
  int aa = *((int *) a);
  int bb = *((int *) b);

  if (aa < bb) {
    return -1;
  } else if (aa > bb) {
    return 1;
  } else {
    return 0;
  }
}

static void save_key(gpointer key, gpointer value G_GNUC_UNUSED,
		     gpointer user_data) {
  int tag = *((int *) key);
  struct hash_key_helper *h = user_data;

  h->tags[h->i++] = tag;
}

static void print_directory(GHashTable *dir) {
  int count = g_hash_table_size(dir);
  struct hash_key_helper h = { 0, g_new(int, count) };
  g_hash_table_foreach(dir, save_key, &h);

  qsort(h.tags, count, sizeof (int), int_compare);
  for (int i = 0; i < count; i++) {
    int tag = h.tags[i];
    print_tag(tag, g_hash_table_lookup(dir, &tag));
  }
  g_free(h.tags);

  printf("\n");
}

void _openslide_tiffdump_print(GSList *tiffdump) {
  int i = 0;

  while (tiffdump != NULL) {
    printf("Directory %d\n", i);

    print_directory(tiffdump->data);

    i++;
    tiffdump = tiffdump->next;
  }
}

uint64_t _openslide_tiffdump_get_uint(struct _openslide_tiffdump_item *item,
                                      int64_t i) {
  g_assert(i >= 0 && i < item->count);
  switch (item->type) {
  case TIFF_BYTE:
    return ((uint8_t *) item->value)[i];
  case TIFF_SHORT:
    return ((uint16_t *) item->value)[i];
  case TIFF_LONG:
  case TIFF_IFD:
    return ((uint32_t *) item->value)[i];
  default:
    g_assert_not_reached();
  }
}

int64_t _openslide_tiffdump_get_sint(struct _openslide_tiffdump_item *item,
                                     int64_t i) {
  g_assert(i >= 0 && i < item->count);
  switch (item->type) {
  case TIFF_SBYTE:
    return ((int8_t *) item->value)[i];
  case TIFF_SSHORT:
    return ((int16_t *) item->value)[i];
  case TIFF_SLONG:
    return ((int32_t *) item->value)[i];
  default:
    g_assert_not_reached();
  }
}

double _openslide_tiffdump_get_float(struct _openslide_tiffdump_item *item,
                                     int64_t i) {
  g_assert(i >= 0 && i < item->count);
  switch (item->type) {
  case TIFF_FLOAT: {
    float val;
    memcpy(&val, ((uint32_t *) item->value) + i, sizeof(val));
    return val;
  }
  case TIFF_DOUBLE: {
    double val;
    memcpy(&val, ((uint64_t *) item->value) + i, sizeof(val));
    return val;
  }
  case TIFF_RATIONAL: {
    // convert 2 longs into rational
    uint32_t *val = item->value;
    return (double) val[i * 2] / (double) val[i * 2 + 1];
  }
  case TIFF_SRATIONAL: {
    // convert 2 slongs into rational
    int32_t *val = item->value;
    return (double) val[i * 2] / (double) val[i * 2 + 1];
  }
  default:
    g_assert_not_reached();
  }
}

const void *_openslide_tiffdump_get_buffer(struct _openslide_tiffdump_item *item) {
  switch (item->type) {
  case TIFF_ASCII:
  case TIFF_UNDEFINED:
    return item->value;
  default:
    g_assert_not_reached();
  }
}
