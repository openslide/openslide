/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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
#include <stdbool.h>
#include <glib.h>
#include <inttypes.h>


#include <tiffio.h>


static int64_t read_uint16(FILE *f, uint16_t endian) {
  uint16_t result;
  if (fread(&result, sizeof result, 1, f) != 1) {
    return -1;
  }

  switch (endian) {
  case TIFF_BIGENDIAN:
    return GUINT16_FROM_BE(result);

  case TIFF_LITTLEENDIAN:
    return GUINT16_FROM_LE(result);

  default:
    g_return_val_if_reached(-1);
  }
}

static int64_t read_uint32(FILE *f, uint16_t endian) {
  uint32_t result;
  if (fread(&result, sizeof result, 1, f) != 1) {
    return -1;
  }

  switch (endian) {
  case TIFF_BIGENDIAN:
    return GUINT32_FROM_BE(result);

  case TIFF_LITTLEENDIAN:
    return GUINT32_FROM_LE(result);

  default:
    g_return_val_if_reached(-1);
  }
}

static bool read_tiff_tag(FILE *f, int64_t size, void *dest,
			  int64_t offset, uint8_t value[]) {
  //  g_debug(" reading tiff tag: size: %d, value/offset %u", (int) size, (int) offset);

  if (size <= 4) {
    // inline
    memcpy(dest, value, size);
  } else {
    off_t old_off = ftello(f);
    if (fseeko(f, offset, SEEK_SET) != 0) {
      return false;
    }
    if (fread(dest, size, 1, f) != 1) {
      return false;
    }
    fseeko(f, old_off, SEEK_SET);
  }

  return true;
}

static void *read_tiff_tag_1(FILE *f,
			     int64_t count, int64_t offset,
			     uint8_t value[]) {
  uint8_t *result = g_try_new(uint8_t, count);
  if (result == NULL) {
    goto FAIL;
  }

  if (!read_tiff_tag(f, count * sizeof *result, result, offset, value)) {
    goto FAIL;
  }

  /*
  g_debug("  count %" PRId64, count);
  for (int i = 0; i < count; i++) {
    if (i > 50) {
      g_debug("    ...");
      break;
    }
    g_debug("   %u", result[i]);
  }
  g_debug(" ");
  */

  return result;

 FAIL:
  g_free(result);
  return NULL;
}

static void *read_tiff_tag_2(FILE *f,
			     int64_t count, int64_t offset,
			     uint8_t value[], uint16_t endian) {
  uint16_t *result = g_try_new(uint16_t, count);
  if (result == NULL) {
    goto FAIL;
  }

  if (!read_tiff_tag(f, count * sizeof *result, result, offset, value)) {
    goto FAIL;
  }

  // swap?
  for (int64_t i = 0; i < count; i++) {
    if (endian == TIFF_BIGENDIAN) {
      result[i] = GUINT16_FROM_BE(result[i]);
    } else {
      result[i] = GUINT16_FROM_LE(result[i]);
    }
  }

  /*
  g_debug("  count %" PRId64, count);
  for (int i = 0; i < count; i++) {
    if (i > 50) {
      g_debug("    ...");
      break;
    }
    g_debug("   %u", result[i]);
  }
  g_debug(" ");
  */

  return result;

 FAIL:
  g_free(result);
  return NULL;
}

static void *read_tiff_tag_4(FILE *f,
			     int64_t count, int64_t offset,
			     uint8_t value[], uint16_t endian) {
  uint32_t *result = g_try_new(uint32_t, count);
  if (result == NULL) {
    goto FAIL;
  }

  if (!read_tiff_tag(f, count * sizeof *result, result, offset, value)) {
    goto FAIL;
  }

  // swap?
  for (int64_t i = 0; i < count; i++) {
    if (endian == TIFF_BIGENDIAN) {
      result[i] = GUINT32_FROM_BE(result[i]);
    } else {
      result[i] = GUINT32_FROM_LE(result[i]);
    }
  }

  /*
  g_debug("  count %" PRId64, count);
  for (int i = 0; i < count; i++) {
    if (i > 50) {
      g_debug("    ...");
      break;
    }
    g_debug("   %u", result[i]);
  }
  g_debug(" ");
  */

  return result;

 FAIL:
  g_free(result);
  return NULL;
}

static void *read_tiff_tag_8(FILE *f,
			     int64_t count, int64_t offset,
			     uint16_t endian) {
  uint64_t *result = g_try_new(uint64_t, count);
  if (result == NULL) {
    goto FAIL;
  }

  if (!read_tiff_tag(f, count * sizeof *result, result, offset, NULL)) {
    goto FAIL;
  }

  // swap?
  for (int64_t i = 0; i < count; i++) {
    if (endian == TIFF_BIGENDIAN) {
      result[i] = GUINT64_FROM_BE(result[i]);
    } else {
      result[i] = GUINT64_FROM_LE(result[i]);
    }
  }

  /*
  g_debug("  count %" PRId64, count);
  for (int i = 0; i < count; i++) {
    if (i > 50) {
      g_debug("    ...");
      break;
    }
    g_debug("   %" PRIu64, result[i]);
  }
  g_debug(" ");
  */

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

static GHashTable *read_directory(FILE *f, int64_t *diroff,
				  GHashTable *loop_detector,
				  uint16_t endian) {
  int64_t off = *diroff;
  *diroff = 0;
  GHashTable *result = NULL;

  //  g_debug("diroff: %" PRId64, off);

  if (off <= 0) {
    g_warning("Bad offset");
    goto FAIL;
  }

  // loop detection
  if (g_hash_table_lookup_extended(loop_detector, &off, NULL, NULL)) {
    // loop
    g_warning("Loop detected");
    goto FAIL;
  }
  int64_t *key = g_slice_new(int64_t);
  *key = off;
  g_hash_table_insert(loop_detector, key, NULL);

  // no loop, let's seek
  if (fseeko(f, off, SEEK_SET) != 0) {
    g_warning("Cannot seek to offset");
    goto FAIL;
  }

  // read directory count
  int dircount = read_uint16(f, endian);
  if (dircount == -1) {
    g_warning("Cannot read dircount");
    goto FAIL;
  }

  //  g_debug("dircount: %d", dircount);


  // initial checks passed, initialized the hashtable
  result = g_hash_table_new_full(g_int_hash, g_int_equal,
				 g_free, tiffdump_item_destroy);

  // read all directory entries
  for (int i = 0; i < dircount; i++) {
    int32_t tag = read_uint16(f, endian);
    int32_t type = read_uint16(f, endian);
    int64_t count = read_uint32(f, endian);

    if ((tag == -1) || (type == -1) || (count == -1)) {
      g_warning("Cannot read tag, type, and count");
      goto FAIL;
    }

    //    g_debug(" tag: %d, type: %d, count: %" PRId64, tag, type, count);

    // read in the value/offset
    uint8_t value[4];
    if (fread(value, 1, 4, f) != 4) {
      g_warning("Cannot read value/offset");
      goto FAIL;
    }

    uint32_t offset;
    memcpy(&offset, value, 4);
    if (endian == TIFF_BIGENDIAN) {
      offset = GUINT32_FROM_BE(offset);
    } else {
      offset = GUINT32_FROM_LE(offset);
    }

    // allocate the item
    struct _openslide_tiffdump_item *data =
      g_slice_new(struct _openslide_tiffdump_item);
    data->type = type;
    data->count = count;

    // load the value
    switch (type) {
    case TIFF_BYTE:
    case TIFF_ASCII:
    case TIFF_SBYTE:
    case TIFF_UNDEFINED:
      data->value = read_tiff_tag_1(f, count, offset, value);
      break;

    case TIFF_SHORT:
    case TIFF_SSHORT:
      data->value = read_tiff_tag_2(f, count, offset, value, endian);
      break;

    case TIFF_LONG:
    case TIFF_SLONG:
    case TIFF_FLOAT:
    case TIFF_IFD:
      data->value = read_tiff_tag_4(f, count, offset, value, endian);
      break;

    case TIFF_RATIONAL:
    case TIFF_SRATIONAL:
      data->value = read_tiff_tag_4(f, count * 2, offset, value, endian);
      break;

    case TIFF_DOUBLE:
      data->value = read_tiff_tag_8(f, count, offset, endian);
      break;

    default:
      g_warning("Unknown type encountered: %d", type);
      goto FAIL;
    }

    if (data->value == NULL) {
      g_warning("Cannot read value");
      goto FAIL;
    }

    // add this tag to the hashtable
    int *key = g_new(int, 1);
    *key = tag;
    g_hash_table_insert(result, key, data);
  }

  // read the next dir offset
  int64_t nextdiroff = read_uint32(f, endian);
  if (nextdiroff == -1) {
    g_warning("Cannot read next directory offset");
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
GSList *_openslide_tiffdump_create(FILE *f) {
  // read and check magic
  uint16_t magic;
  fseeko(f, 0, SEEK_SET);
  if (fread(&magic, sizeof magic, 1, f) != 1) {
    return NULL;
  }
  if (magic != TIFF_BIGENDIAN && magic != TIFF_LITTLEENDIAN) {
    return NULL;
  }

  //  g_debug("magic: %d", magic);

  int32_t version = read_uint16(f, magic);
  int64_t diroff = read_uint32(f, magic);

  //  g_debug("version: %d", version);

  if (version != TIFF_VERSION) {
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
    GHashTable *ht = read_directory(f, &diroff, loop_detector, magic);

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
  printf(" %d: type: %d, count: %" PRId64 "\n ", tag, item->type, item->count);

  if (item->type == TIFF_ASCII) {
    // will only print first string if there are multiple
    const char *str = _openslide_tiffdump_get_ascii(item);
    if (str[item->count - 1] == '\0') {
      printf(" %s", str);
    } else {
      g_warning("ASCII value not null-terminated");
    }
  } else {
    for (int64_t i = 0; i < item->count; i++) {
      switch (item->type) {
      case TIFF_BYTE:
	printf(" %" PRIu8, _openslide_tiffdump_get_byte(item, i));
	break;

      case TIFF_SBYTE:
	printf(" %" PRId8, _openslide_tiffdump_get_sbyte(item, i));
	break;

      case TIFF_UNDEFINED:
	printf(" %.2" PRIx8, _openslide_tiffdump_get_undefined(item, i));
	break;

      case TIFF_SHORT:
	printf(" %" PRIu16, _openslide_tiffdump_get_short(item, i));
	break;

      case TIFF_SSHORT:
	printf(" %" PRId16, _openslide_tiffdump_get_sshort(item, i));
	break;

      case TIFF_LONG:
	printf(" %" PRIu32, _openslide_tiffdump_get_long(item, i));
	break;

      case TIFF_SLONG:
	printf(" %" PRId32, _openslide_tiffdump_get_slong(item, i));
	break;

      case TIFF_FLOAT:
	printf(" %g", _openslide_tiffdump_get_float(item, i));
	break;

      case TIFF_IFD:
	printf(" %.8" PRIx64, _openslide_tiffdump_get_ifd(item, i));
	break;

      case TIFF_RATIONAL:
	printf(" %g", _openslide_tiffdump_get_rational(item, i));
	break;

      case TIFF_SRATIONAL:
	printf(" %g", _openslide_tiffdump_get_srational(item, i));
	break;

      case TIFF_DOUBLE:
	printf(" %g", _openslide_tiffdump_get_double(item, i));
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

static void save_key(gpointer key, gpointer _OPENSLIDE_UNUSED(value),
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


static void check_assertions(struct _openslide_tiffdump_item *item,
			     TIFFDataType type, int64_t i) {
  g_assert(item->type == type);
  g_assert(i >= 0);
  g_assert(i < item->count);
}

uint8_t _openslide_tiffdump_get_byte(struct _openslide_tiffdump_item *item,
				     int64_t i) {
  check_assertions(item, TIFF_BYTE, i);
  return ((uint8_t *) item->value)[i];
}

const char *_openslide_tiffdump_get_ascii(struct _openslide_tiffdump_item *item) {
  check_assertions(item, TIFF_ASCII, 0);
  return item->value;
}

uint16_t _openslide_tiffdump_get_short(struct _openslide_tiffdump_item *item,
				       int64_t i) {
  check_assertions(item, TIFF_SHORT, i);
  return ((uint16_t *) item->value)[i];
}

uint32_t _openslide_tiffdump_get_long(struct _openslide_tiffdump_item *item,
				      int64_t i) {
  check_assertions(item, TIFF_LONG, i);
  return ((uint32_t *) item->value)[i];
}

double _openslide_tiffdump_get_rational(struct _openslide_tiffdump_item *item,
					int64_t i) {
  check_assertions(item, TIFF_RATIONAL, i);

  // convert 2 longs into rational
  uint32_t *value = item->value;
  return (double) value[i * 2] / (double) value[i * 2 + 1];
}

int8_t _openslide_tiffdump_get_sbyte(struct _openslide_tiffdump_item *item,
				     int64_t i) {
  check_assertions(item, TIFF_SBYTE, i);
  return ((uint8_t *) item->value)[i];
}

uint8_t _openslide_tiffdump_get_undefined(struct _openslide_tiffdump_item *item,
					  int64_t i) {
  check_assertions(item, TIFF_UNDEFINED, i);
  return ((uint8_t *) item->value)[i];
}

int16_t _openslide_tiffdump_get_sshort(struct _openslide_tiffdump_item *item,
				       int64_t i) {
  check_assertions(item, TIFF_SSHORT, i);
  return ((uint16_t *) item->value)[i];
}

int32_t _openslide_tiffdump_get_slong(struct _openslide_tiffdump_item *item,
				      int64_t i) {
  check_assertions(item, TIFF_SLONG, i);
  return ((uint32_t *) item->value)[i];
}

double _openslide_tiffdump_get_srational(struct _openslide_tiffdump_item *item,
					 int64_t i) {
  check_assertions(item, TIFF_SRATIONAL, i);

  // convert 2 slongs into rational
  uint32_t *value = item->value;
  return (double) ((int32_t) value[i * 2]) /
    (double) ((int32_t) value[i * 2 + 1]);
}

float _openslide_tiffdump_get_float(struct _openslide_tiffdump_item *item,
				    int64_t i) {
  check_assertions(item, TIFF_FLOAT, i);

  float val;
  memcpy(&val, ((uint32_t *) item->value) + i, sizeof val);
  return val;
}

double _openslide_tiffdump_get_double(struct _openslide_tiffdump_item *item,
				      int64_t i) {
  check_assertions(item, TIFF_DOUBLE, i);

  double val;
  memcpy(&val, ((uint64_t *) item->value) + i, sizeof val);
  return val;
}

int64_t _openslide_tiffdump_get_ifd(struct _openslide_tiffdump_item *item,
				    int64_t i) {
  check_assertions(item, TIFF_IFD, i);
  return ((uint32_t *) item->value)[i];
}
