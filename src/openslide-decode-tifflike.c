/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
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
#include "openslide-decode-tifflike.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>

#include <tiff.h>

#define NO_OFFSET UINT64_MAX

#define NDPI_TAG 65420


struct _openslide_tifflike {
  char *filename;
  bool big_endian;
  bool ndpi;
  GPtrArray *directories;
  GMutex value_lock;
};

struct tiff_directory {
  GHashTable *items;
  uint64_t offset;  // only for NDPI fixups
};

struct tiff_item {
  uint16_t type;
  int64_t count;
  uint64_t offset;

  // data format variants
  uint64_t *uints;
  int64_t *sints;
  double *floats;
  void *buffer;
};


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
static uint64_t read_uint(struct _openslide_file *f, int32_t size,
                          bool big_endian, bool *ok) {
  g_assert(ok != NULL);

  uint8_t buf[size];
  if (_openslide_fread(f, buf, size) != (size_t) size) {
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

static uint32_t get_value_size(uint16_t type, uint64_t *count) {
  switch (type) {
  case TIFF_BYTE:
  case TIFF_ASCII:
  case TIFF_SBYTE:
  case TIFF_UNDEFINED:
    return 1;

  case TIFF_SHORT:
  case TIFF_SSHORT:
    return 2;

  case TIFF_LONG:
  case TIFF_SLONG:
  case TIFF_FLOAT:
  case TIFF_IFD:
    return 4;

  case TIFF_RATIONAL:
  case TIFF_SRATIONAL:
    *count *= 2;
    return 4;

  case TIFF_DOUBLE:
  case TIFF_LONG8:
  case TIFF_SLONG8:
  case TIFF_IFD8:
    return 8;

  default:
    return 0;
  }
}

// Re-add implied high-order bits to a 32-bit offset.
// Heuristic: maximize high-order bits while keeping the offset below diroff.
static uint64_t fix_offset_ndpi(uint64_t diroff, uint64_t offset) {
  uint64_t result = (diroff & ~(uint64_t) UINT32_MAX) | (offset & UINT32_MAX);
  if (result >= diroff) {
    // ensure result doesn't wrap around
    result = MIN(result - UINT32_MAX - 1, result);
  }
  //g_debug("diroff %"PRIx64": %"PRIx64" -> %"PRIx64, diroff, offset, result);
  return result;
}

#define ALLOC_VALUES_OR_FAIL(OUT, TYPE, COUNT) do {			\
    OUT = g_try_new(TYPE, COUNT);					\
    if (!OUT) {								\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,		\
                  "Cannot allocate TIFF value array");			\
      return false;							\
    }									\
  } while (0)

#define CONVERT_VALUES_EXTEND(TO, FROM_TYPE, FROM, COUNT) do {		\
    const FROM_TYPE *from = (const FROM_TYPE *) FROM;			\
    for (int64_t i = 0; i < COUNT; i++) {				\
      TO[i] = from[i];							\
    }									\
  } while (0)

#define CONVERT_VALUES_RATIONAL(TO, FROM_TYPE, FROM, COUNT) do {	\
    const FROM_TYPE *from = (const FROM_TYPE *) FROM;			\
    for (int64_t i = 0; i < COUNT; i++) {				\
      TO[i] = (double) from[i * 2] / (double) from[i * 2 + 1];		\
    }									\
  } while (0)

// value_lock must be held
static bool set_item_values(struct tiff_item *item,
                            const void *buf,
                            GError **err) {
  //g_debug("setting values for item type %d", item->type);

  switch (item->type) {
  // uints
  case TIFF_BYTE:
    if (!item->uints) {
      ALLOC_VALUES_OR_FAIL(item->uints, uint64_t, item->count);
      CONVERT_VALUES_EXTEND(item->uints, uint8_t, buf, item->count);
    }
    // for TIFFTAG_XMLPACKET
    if (!item->buffer) {
      ALLOC_VALUES_OR_FAIL(item->buffer, char, item->count + 1);
      memcpy(item->buffer, buf, item->count);
      ((char *) item->buffer)[item->count] = 0;
    }
    break;
  case TIFF_SHORT:
    if (!item->uints) {
      ALLOC_VALUES_OR_FAIL(item->uints, uint64_t, item->count);
      CONVERT_VALUES_EXTEND(item->uints, uint16_t, buf, item->count);
    }
    break;
  case TIFF_LONG:
  case TIFF_IFD:
    if (!item->uints) {
      ALLOC_VALUES_OR_FAIL(item->uints, uint64_t, item->count);
      CONVERT_VALUES_EXTEND(item->uints, uint32_t, buf, item->count);
    }
    break;
  case TIFF_LONG8:
  case TIFF_IFD8:
    if (!item->uints) {
      ALLOC_VALUES_OR_FAIL(item->uints, uint64_t, item->count);
      memcpy(item->uints, buf, sizeof(uint64_t) * item->count);
    }
    break;

  // sints
  case TIFF_SBYTE:
    if (!item->sints) {
      ALLOC_VALUES_OR_FAIL(item->sints, int64_t, item->count);
      CONVERT_VALUES_EXTEND(item->sints, int8_t, buf, item->count);
    }
    break;
  case TIFF_SSHORT:
    if (!item->sints) {
      ALLOC_VALUES_OR_FAIL(item->sints, int64_t, item->count);
      CONVERT_VALUES_EXTEND(item->sints, int16_t, buf, item->count);
    }
    break;
  case TIFF_SLONG:
    if (!item->sints) {
      ALLOC_VALUES_OR_FAIL(item->sints, int64_t, item->count);
      CONVERT_VALUES_EXTEND(item->sints, int32_t, buf, item->count);
    }
    break;
  case TIFF_SLONG8:
    if (!item->sints) {
      ALLOC_VALUES_OR_FAIL(item->sints, int64_t, item->count);
      memcpy(item->sints, buf, sizeof(int64_t) * item->count);
    }
    break;

  // floats
  case TIFF_FLOAT:
    if (!item->floats) {
      ALLOC_VALUES_OR_FAIL(item->floats, double, item->count);
      CONVERT_VALUES_EXTEND(item->floats, float, buf, item->count);
    }
    break;
  case TIFF_DOUBLE:
    if (!item->floats) {
      ALLOC_VALUES_OR_FAIL(item->floats, double, item->count);
      memcpy(item->floats, buf, sizeof(double) * item->count);
    }
    break;
  case TIFF_RATIONAL:
    // convert 2 longs into rational
    if (!item->floats) {
      ALLOC_VALUES_OR_FAIL(item->floats, double, item->count);
      CONVERT_VALUES_RATIONAL(item->floats, uint32_t, buf, item->count);
    }
    break;
  case TIFF_SRATIONAL:
    // convert 2 slongs into rational
    if (!item->floats) {
      ALLOC_VALUES_OR_FAIL(item->floats, double, item->count);
      CONVERT_VALUES_RATIONAL(item->floats, int32_t, buf, item->count);
    }
    break;

  // buffer
  case TIFF_ASCII:
  case TIFF_UNDEFINED:
    if (!item->buffer) {
      ALLOC_VALUES_OR_FAIL(item->buffer, char, item->count + 1);
      memcpy(item->buffer, buf, item->count);
      ((char *) item->buffer)[item->count] = 0;
    }
    break;

  // default
  default:
    g_assert_not_reached();
  }

  // record that we've set all values
  item->offset = NO_OFFSET;
  return true;
}

static bool populate_item(struct _openslide_tifflike *tl,
                          struct tiff_item *item,
                          GError **err) {
  g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
    g_mutex_locker_new(&tl->value_lock);
  if (item->offset == NO_OFFSET) {
    return true;
  }

  g_autoptr(_openslide_file) f = _openslide_fopen(tl->filename, err);
  if (!f) {
    return false;
  }

  uint64_t count = item->count;
  int32_t value_size = get_value_size(item->type, &count);
  g_assert(value_size);
  ssize_t len = value_size * count;

  g_autofree void *buf = g_try_malloc(len);
  if (buf == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot allocate TIFF value");
    return false;
  }

  //g_debug("reading tiff value: len: %"PRId64", offset %"PRIu64, len, item->offset);
  if (!_openslide_fseek(f, item->offset, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to read TIFF value: ");
    return false;
  }
  if (_openslide_fread(f, buf, len) != (size_t) len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read TIFF value");
    return false;
  }

  fix_byte_order(buf, value_size, count, tl->big_endian);
  if (!set_item_values(item, buf, err)) {
    return false;
  }

  return true;
}

static void tiff_directory_destroy(struct tiff_directory *d) {
  if (d == NULL) {
    return;
  }
  g_hash_table_unref(d->items);
  g_slice_free(struct tiff_directory, d);
}

typedef struct tiff_directory tiff_directory;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(tiff_directory, tiff_directory_destroy)

static void tiff_item_destroy(gpointer data) {
  struct tiff_item *item = data;

  g_free(item->uints);
  g_free(item->sints);
  g_free(item->floats);
  g_free(item->buffer);
  g_slice_free(struct tiff_item, item);
}

static struct tiff_directory *read_directory(struct _openslide_file *f,
                                             uint64_t *diroff,
                                             struct tiff_directory *first_dir,
                                             GHashTable *loop_detector,
                                             bool bigtiff,
                                             bool ndpi,
                                             bool big_endian,
                                             GError **err) {
  uint64_t off = *diroff;
  *diroff = 0;

  //  g_debug("diroff: %"PRIu64, off);

  if ((int64_t) off <= 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad offset");
    return NULL;
  }

  // loop detection
  if (g_hash_table_lookup_extended(loop_detector, &off, NULL, NULL)) {
    // loop
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Loop detected");
    return NULL;
  }
  uint64_t *key = g_slice_new(uint64_t);
  *key = off;
  g_hash_table_insert(loop_detector, key, NULL);

  // no loop, let's seek
  if (!_openslide_fseek(f, off, SEEK_SET, err)) {
    g_prefix_error(err, "Cannot seek to offset: ");
    return NULL;
  }

  // read directory count
  bool ok = true;
  uint64_t dircount = read_uint(f, bigtiff ? 8 : 2, big_endian, &ok);
  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read dircount");
    return NULL;
  }

  //  g_debug("dircount: %"PRIu64, dircount);


  // initial checks passed, initialize the directory
  g_autoptr(tiff_directory) d = g_slice_new0(struct tiff_directory);
  d->items = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                   NULL, tiff_item_destroy);
  d->offset = off;

  // read all directory entries
  for (uint64_t n = 0; n < dircount; n++) {
    uint16_t tag = read_uint(f, 2, big_endian, &ok);
    uint16_t type = read_uint(f, 2, big_endian, &ok);
    uint64_t count = read_uint(f, bigtiff ? 8 : 4, big_endian, &ok);

    if (!ok) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot read tag, type, and count");
      return NULL;
    }

    //    g_debug(" tag: %d, type: %d, count: %"PRId64, tag, type, count);

    // allocate the item
    struct tiff_item *item = g_slice_new0(struct tiff_item);
    item->type = type;
    item->count = count;
    g_hash_table_insert(d->items, GINT_TO_POINTER(tag), item);

    // compute value size
    uint32_t value_size = get_value_size(type, &count);
    if (!value_size) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unknown type encountered: %d", type);
      return NULL;
    }

    // check for overflow
    if (count > SSIZE_MAX / value_size) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Value count too large");
      return NULL;
    }

    // read in the value/offset
    uint8_t value[bigtiff ? 8 : 4];
    if (_openslide_fread(f, value, sizeof(value)) != sizeof(value)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot read value/offset");
      return NULL;
    }

    // does value/offset contain the value?
    if (value_size * count <= sizeof(value)) {
      // yes
      fix_byte_order(value, value_size, count, big_endian);
      if (!set_item_values(item, value, err)) {
        return NULL;
      }

    } else {
      // no; store offset
      if (bigtiff) {
        memcpy(&item->offset, value, 8);
        fix_byte_order(&item->offset, sizeof(item->offset), 1, big_endian);
      } else {
        uint32_t off32;
        memcpy(&off32, value, 4);
        fix_byte_order(&off32, sizeof(off32), 1, big_endian);
        item->offset = off32;
      }

      if (ndpi) {
        // heuristically set high-order bits of offset
        // if this tag has the same offset in the first IFD, reuse that value
        struct tiff_item *first_dir_item = NULL;
        if (first_dir) {
          first_dir_item = g_hash_table_lookup(first_dir->items,
                                               GINT_TO_POINTER(tag));
        }
        if (!first_dir_item || first_dir_item->offset != item->offset) {
          item->offset = fix_offset_ndpi(off, item->offset);
        }
      }
    }
  }

  // read the next dir offset
  uint64_t nextdiroff = read_uint(f, (bigtiff || ndpi) ? 8 : 4,
                                  big_endian, &ok);
  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read next directory offset");
    return NULL;
  }
  *diroff = nextdiroff;

  // success
  return g_steal_pointer(&d);
}

struct _openslide_tifflike *_openslide_tifflike_create(const char *filename,
                                                       GError **err) {
  // open file
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return NULL;
  }

  // read and check magic
  uint16_t magic;
  if (_openslide_fread(f, &magic, sizeof magic) != sizeof magic) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read TIFF magic number");
    return NULL;
  }
  if (magic != TIFF_BIGENDIAN && magic != TIFF_LITTLEENDIAN) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unrecognized TIFF magic number");
    return NULL;
  }
  bool big_endian = (magic == TIFF_BIGENDIAN);

  //  g_debug("magic: %d", magic);

  // read rest of header
  bool ok = true;
  uint16_t version = read_uint(f, 2, big_endian, &ok);
  bool bigtiff = (version == TIFF_VERSION_BIG);
  uint16_t offset_size = 0;
  uint16_t pad = 0;
  if (bigtiff) {
    offset_size = read_uint(f, 2, big_endian, &ok);
    pad = read_uint(f, 2, big_endian, &ok);
  }
  // for classic TIFF, will mask off the high bytes after NDPI detection
  uint64_t diroff = read_uint(f, 8, big_endian, &ok);

  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read TIFF header");
    return NULL;
  }

  //  g_debug("version: %d", version);

  // validate
  if (version == TIFF_VERSION_BIG) {
    if (offset_size != 8 || pad != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unexpected value in BigTIFF header");
      return NULL;
    }
  } else if (version != TIFF_VERSION_CLASSIC) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unrecognized TIFF version");
    return NULL;
  }

  // allocate struct
  g_autoptr(_openslide_tifflike) tl = g_slice_new0(struct _openslide_tifflike);
  tl->filename = g_strdup(filename);
  tl->big_endian = big_endian;
  tl->directories = g_ptr_array_new();
  g_mutex_init(&tl->value_lock);

  // initialize directory reading
  g_autoptr(GHashTable) loop_detector =
    g_hash_table_new_full(g_int64_hash, g_int64_equal,
                          _openslide_int64_free, NULL);
  struct tiff_directory *first_dir = NULL;

  // NDPI needs special quirks, since it is classic TIFF pretending to be
  // BigTIFF.  Enable NDPI mode if this is classic TIFF but the offset to
  // the first directory -- when treated as a 64-bit value -- points to a
  // valid directory containing the NDPI_TAG.
  if (!bigtiff && diroff != 0) {
    uint64_t trial_diroff = diroff;
    struct tiff_directory *d = read_directory(f, &trial_diroff,
                                              NULL,
                                              loop_detector,
                                              bigtiff, true, big_endian,
                                              NULL);
    if (d) {
      struct tiff_item *item =
        g_hash_table_lookup(d->items, GINT_TO_POINTER(NDPI_TAG));
      if (item && item->count) {
        // NDPI
        //g_debug("NDPI detected");
        tl->ndpi = true;
        // save the parsed directory rather than reparsing it below
        g_ptr_array_add(tl->directories, d);
        first_dir = d;
        diroff = trial_diroff;
      } else {
        // correctly parsed the directory in NDPI mode, but didn't find
        // NDPI_TAG
        tiff_directory_destroy(d);
      }
    }
    if (!tl->ndpi) {
      // This is classic TIFF, so diroff is 32 bits.  Mask off the high bits
      // and reset.
      //g_debug("not NDPI");
      diroff &= 0xffffffff;
      g_hash_table_remove_all(loop_detector);
    }
  }

  // read all the directories
  while (diroff != 0) {
    // read a directory
    struct tiff_directory *d = read_directory(f, &diroff,
                                              first_dir,
                                              loop_detector,
                                              bigtiff, tl->ndpi, big_endian,
                                              err);

    // was the directory successfully read?
    if (d == NULL) {
      return NULL;
    }

    // store result
    g_ptr_array_add(tl->directories, d);
    if (!first_dir) {
      first_dir = d;
    }
  }

  // ensure there are directories
  if (tl->directories->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF contains no directories");
    return NULL;
  }

  return g_steal_pointer(&tl);
}


void _openslide_tifflike_destroy(struct _openslide_tifflike *tl) {
  if (tl == NULL) {
    return;
  }
  g_mutex_lock(&tl->value_lock);
  for (uint32_t n = 0; n < tl->directories->len; n++) {
    tiff_directory_destroy(tl->directories->pdata[n]);
  }
  g_mutex_unlock(&tl->value_lock);
  g_ptr_array_free(tl->directories, true);
  g_free(tl->filename);
  g_mutex_clear(&tl->value_lock);
  g_slice_free(struct _openslide_tifflike, tl);
}

static struct tiff_item *get_item(struct _openslide_tifflike *tl,
                                  int64_t dir, int32_t tag) {
  if (dir < 0 || dir >= tl->directories->len) {
    return NULL;
  }
  struct tiff_directory *d = tl->directories->pdata[dir];
  return g_hash_table_lookup(d->items, GINT_TO_POINTER(tag));
}

static void print_tag(struct _openslide_tifflike *tl,
                      int64_t dir, int32_t tag) {
  struct tiff_item *item = get_item(tl, dir, tag);
  g_assert(item != NULL);

  printf(" %d: type: %d, count: %"PRId64"\n ", tag, item->type, item->count);

  switch (item->type) {
  case TIFF_ASCII: {
    // will only print first string if there are multiple
    const char *str = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
    printf(" %s", str);
    break;
  }

  case TIFF_UNDEFINED: {
    const uint8_t *data = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
    for (int64_t i = 0; i < item->count; i++) {
      printf(" %u", data[i]);
    }
    break;
  }

  case TIFF_BYTE:
  case TIFF_SHORT:
  case TIFF_LONG:
  case TIFF_LONG8: {
    const uint64_t *uints = _openslide_tifflike_get_uints(tl, dir, tag, NULL);
    for (int64_t i = 0; i < item->count; i++) {
      printf(" %"PRIu64, uints[i]);
    }
    break;
  }

  case TIFF_IFD:
  case TIFF_IFD8: {
    const uint64_t *uints = _openslide_tifflike_get_uints(tl, dir, tag, NULL);
    for (int64_t i = 0; i < item->count; i++) {
      printf(" %.16"PRIx64, uints[i]);
    }
    break;
  }

  case TIFF_SBYTE:
  case TIFF_SSHORT:
  case TIFF_SLONG:
  case TIFF_SLONG8: {
    const int64_t *sints = _openslide_tifflike_get_sints(tl, dir, tag, NULL);
    for (int64_t i = 0; i < item->count; i++) {
      printf(" %"PRId64, sints[i]);
    }
    break;
  }

  case TIFF_FLOAT:
  case TIFF_DOUBLE:
  case TIFF_RATIONAL:
  case TIFF_SRATIONAL: {
    const double *floats = _openslide_tifflike_get_floats(tl, dir, tag, NULL);
    for (int64_t i = 0; i < item->count; i++) {
      printf(" %g", floats[i]);
    }
    break;
  }

  default:
    g_return_if_reached();
  }
  printf("\n");
}

static int tag_compare(gconstpointer a, gconstpointer b) {
  int32_t aa = GPOINTER_TO_INT(a);
  int32_t bb = GPOINTER_TO_INT(b);

  if (aa < bb) {
    return -1;
  } else if (aa > bb) {
    return 1;
  } else {
    return 0;
  }
}

static void print_directory(struct _openslide_tifflike *tl,
                            int64_t dir) {
  struct tiff_directory *d = tl->directories->pdata[dir];
  g_autoptr(GList) keys = g_hash_table_get_keys(d->items);
  keys = g_list_sort(keys, tag_compare);
  for (GList *el = keys; el; el = el->next) {
    print_tag(tl, dir, GPOINTER_TO_INT(el->data));
  }

  printf("\n");
}

void _openslide_tifflike_print(struct _openslide_tifflike *tl) {
  for (uint32_t n = 0; n < tl->directories->len; n++) {
    printf("Directory %u\n", n);
    print_directory(tl, n);
  }
}

int64_t _openslide_tifflike_get_directory_count(struct _openslide_tifflike *tl) {
  return tl->directories->len;
}

int64_t _openslide_tifflike_get_value_count(struct _openslide_tifflike *tl,
                                            int64_t dir, int32_t tag) {
  struct tiff_item *item = get_item(tl, dir, tag);
  if (item == NULL) {
    return 0;
  }
  return item->count;
}

static struct tiff_item *get_and_check_item(struct _openslide_tifflike *tl,
                                            int64_t dir, int32_t tag,
                                            GError **err) {
  struct tiff_item *item = get_item(tl, dir, tag);
  if (item == NULL || item->count == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE,
                "No such value: directory %"PRId64", tag %d", dir, tag);
    return NULL;
  }
  return item;
}

uint64_t _openslide_tifflike_get_uint(struct _openslide_tifflike *tl,
                                      int64_t dir, int32_t tag,
                                      GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return 0;
  }
  if (!item->uints) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected value type: directory %"PRId64", "
                "tag %d, type %d", dir, tag, item->type);
    return 0;
  }
  return item->uints[0];
}

int64_t _openslide_tifflike_get_sint(struct _openslide_tifflike *tl,
                                     int64_t dir, int32_t tag,
                                     GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return 0;
  }
  if (!item->sints) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected value type: directory %"PRId64", "
                "tag %d, type %d", dir, tag, item->type);
    return 0;
  }
  return item->sints[0];
}

double _openslide_tifflike_get_float(struct _openslide_tifflike *tl,
                                     int64_t dir, int32_t tag,
                                     GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return NAN;
  }
  if (!item->floats) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected value type: directory %"PRId64", "
                "tag %d, type %d", dir, tag, item->type);
    return NAN;
  }
  return item->floats[0];
}

const uint64_t *_openslide_tifflike_get_uints(struct _openslide_tifflike *tl,
                                              int64_t dir, int32_t tag,
                                              GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return NULL;
  }
  if (!item->uints) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected value type: directory %"PRId64", "
                "tag %d, type %d", dir, tag, item->type);
    return NULL;
  }
  return item->uints;
}

const int64_t *_openslide_tifflike_get_sints(struct _openslide_tifflike *tl,
                                             int64_t dir, int32_t tag,
                                             GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return NULL;
  }
  if (!item->sints) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected value type: directory %"PRId64", "
                "tag %d, type %d", dir, tag, item->type);
    return NULL;
  }
  return item->sints;
}

const double *_openslide_tifflike_get_floats(struct _openslide_tifflike *tl,
                                             int64_t dir, int32_t tag,
                                             GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return NULL;
  }
  if (!item->floats) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected value type: directory %"PRId64", "
                "tag %d, type %d", dir, tag, item->type);
    return NULL;
  }
  return item->floats;
}

const void *_openslide_tifflike_get_buffer(struct _openslide_tifflike *tl,
                                           int64_t dir, int32_t tag,
                                           GError **err) {
  struct tiff_item *item = get_and_check_item(tl, dir, tag, err);
  if (item == NULL || !populate_item(tl, item, err)) {
    return NULL;
  }
  if (!item->buffer) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected value type: directory %"PRId64", "
                "tag %d, type %d", dir, tag, item->type);
    return NULL;
  }
  return item->buffer;
}

bool _openslide_tifflike_is_tiled(struct _openslide_tifflike *tl,
                                  int64_t dir) {
  return _openslide_tifflike_get_value_count(tl, dir, TIFFTAG_TILEWIDTH) &&
         _openslide_tifflike_get_value_count(tl, dir, TIFFTAG_TILELENGTH);
}

uint64_t _openslide_tifflike_uint_fix_offset_ndpi(struct _openslide_tifflike *tl,
                                                  int64_t dir, uint64_t offset) {
  g_assert(dir >= 0 && dir < tl->directories->len);
  if (!tl->ndpi) {
    return offset;
  }
  struct tiff_directory *d = tl->directories->pdata[dir];
  return fix_offset_ndpi(d->offset, offset);
}

static const char *store_string_property(struct _openslide_tifflike *tl,
                                         int64_t dir,
                                         openslide_t *osr,
                                         const char *name,
                                         int32_t tag) {
  const char *buf = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
  if (!buf) {
    return NULL;
  }
  char *value = g_strdup(buf);
  g_hash_table_insert(osr->properties, g_strdup(name), value);
  return value;
}

static void store_and_hash_string_property(struct _openslide_tifflike *tl,
                                           int64_t dir,
                                           openslide_t *osr,
                                           struct _openslide_hash *quickhash1,
                                           const char *name,
                                           int32_t tag) {
  _openslide_hash_string(quickhash1, name);
  _openslide_hash_string(quickhash1,
                         store_string_property(tl, dir, osr, name, tag));
}

static void store_float_property(struct _openslide_tifflike *tl,
                                 int64_t dir,
                                 openslide_t *osr,
                                 const char *name,
                                 int32_t tag) {
  GError *tmp_err = NULL;
  double value = _openslide_tifflike_get_float(tl, dir, tag, &tmp_err);
  if (!tmp_err) {
    g_hash_table_insert(osr->properties,
                        g_strdup(name),
                        _openslide_format_double(value));
  }
  g_clear_error(&tmp_err);
}

static void store_and_hash_properties(struct _openslide_tifflike *tl,
                                      int64_t dir,
                                      openslide_t *osr,
                                      struct _openslide_hash *quickhash1) {
  GError *tmp_err = NULL;

  // strings
  store_string_property(tl, dir, osr, OPENSLIDE_PROPERTY_NAME_COMMENT,
                        TIFFTAG_IMAGEDESCRIPTION);

  // strings to store and hash
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.ImageDescription",
                                 TIFFTAG_IMAGEDESCRIPTION);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Make", TIFFTAG_MAKE);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Model", TIFFTAG_MODEL);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Software", TIFFTAG_SOFTWARE);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.DateTime", TIFFTAG_DATETIME);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Artist", TIFFTAG_ARTIST);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.HostComputer", TIFFTAG_HOSTCOMPUTER);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Copyright", TIFFTAG_COPYRIGHT);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.DocumentName", TIFFTAG_DOCUMENTNAME);

  // don't hash floats, they might be unstable over time
  store_float_property(tl, dir, osr, "tiff.XResolution", TIFFTAG_XRESOLUTION);
  store_float_property(tl, dir, osr, "tiff.YResolution", TIFFTAG_YRESOLUTION);
  store_float_property(tl, dir, osr, "tiff.XPosition", TIFFTAG_XPOSITION);
  store_float_property(tl, dir, osr, "tiff.YPosition", TIFFTAG_YPOSITION);

  // special
  int64_t resolution_unit =
    _openslide_tifflike_get_uint(tl, dir, TIFFTAG_RESOLUTIONUNIT, &tmp_err);
  if (tmp_err) {
    resolution_unit = RESUNIT_INCH;  // default
    g_clear_error(&tmp_err);
  }
  const char *result;
  switch(resolution_unit) {
  case RESUNIT_NONE:
    result = "none";
    break;
  case RESUNIT_INCH:
    result = "inch";
    break;
  case RESUNIT_CENTIMETER:
    result = "centimeter";
    break;
  default:
    result = "unknown";
  }
  g_hash_table_insert(osr->properties,
                      g_strdup("tiff.ResolutionUnit"),
                      g_strdup(result));
}

static bool hash_tiff_level(struct _openslide_hash *hash,
                            struct _openslide_tifflike *tl,
                            int32_t dir,
                            GError **err) {
  int32_t offset_tag;
  int32_t length_tag;

  // determine layout
  if (_openslide_tifflike_get_value_count(tl, dir, TIFFTAG_TILEOFFSETS)) {
    // tiled
    offset_tag = TIFFTAG_TILEOFFSETS;
    length_tag = TIFFTAG_TILEBYTECOUNTS;
  } else if (_openslide_tifflike_get_value_count(tl, dir,
                                                 TIFFTAG_STRIPOFFSETS)) {
    // stripped
    offset_tag = TIFFTAG_STRIPOFFSETS;
    length_tag = TIFFTAG_STRIPBYTECOUNTS;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Directory %d is neither tiled nor stripped", dir);
    return false;
  }

  // get tile/strip count
  int64_t count = _openslide_tifflike_get_value_count(tl, dir, offset_tag);
  if (!count ||
      count != _openslide_tifflike_get_value_count(tl, dir, length_tag)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid tile/strip counts for directory %d", dir);
    return false;
  }

  // get offset/length arrays
  const uint64_t *offsets = _openslide_tifflike_get_uints(tl, dir, offset_tag,
                                                          err);
  if (!offsets) {
    return false;
  }
  const uint64_t *lengths = _openslide_tifflike_get_uints(tl, dir, length_tag,
                                                          err);
  if (!lengths) {
    return false;
  }

  // check total size
  int64_t total = 0;
  for (int64_t i = 0; i < count; i++) {
    total += lengths[i];
    if (total > (5 << 20)) {
      // This is a non-pyramidal image or one with a very large top level.
      // Refuse to calculate a quickhash for it to keep openslide_open()
      // from taking an arbitrary amount of time.  (#79)
      _openslide_hash_disable(hash);
      return true;
    }
  }

  // hash raw data of each tile/strip
  for (int64_t i = 0; i < count; i++) {
    if (!_openslide_hash_file_part(hash, tl->filename, offsets[i], lengths[i],
                                   err)) {
      return false;
    }
  }

  return true;
}

bool _openslide_tifflike_init_properties_and_hash(openslide_t *osr,
                                                  struct _openslide_tifflike *tl,
                                                  struct _openslide_hash *quickhash1,
                                                  int32_t lowest_resolution_level,
                                                  int32_t property_dir,
                                                  GError **err) {
  // generate hash of the smallest level
  if (!hash_tiff_level(quickhash1, tl, lowest_resolution_level, err)) {
    g_prefix_error(err, "Cannot hash TIFF tiles: ");
    return false;
  }

  // load TIFF properties
  store_and_hash_properties(tl, property_dir, osr, quickhash1);

  return true;
}
