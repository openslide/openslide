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
 *
 * Add Zeiss czi support. Wei Chen <chenw1@uthscsa.edu>
 *
 */
#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-jp2k.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-xml.h"
#include "openslide-decode-jxr.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#define CZI_SEG_ID_LEN   16
#define MAX_CHANNEL       3

#define DIV_ROUND_CLOSEST(n, d)                                                \
  ((((n) < 0) != ((d) < 0)) ? (((n) - (d)/2)/(d)) : (((n) + (d)/2)/(d)))

/* zeiss uses little-endian */
struct zisraw_seg_hdr {
  char sid[CZI_SEG_ID_LEN];
  int64_t allocated_size;
  int64_t used_size;
};

// beginning of a CZI file, SID = ZISRAWFILE
struct __attribute__ ((__packed__)) zisraw_data_file_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t major;
  int32_t minor;
  int32_t _reserved1;
  int32_t _reserved2;
  char primary_file_guid[16];
  char file_guid[16];
  int32_t file_part;   // this causes off-align
  int64_t subblk_dir_pos;
  int64_t meta_pos;
  int32_t update_pending;
  int64_t att_dir_pos;
};

// SubBlockDirectorySegment, SID = ZISRAWDIRECTORY
struct zisraw_subblk_dir_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t entry_count;
  char _reserved[124];
  // followed by DirectoryEntryDV list
};

// Metadata segment, SID = ZISRAWMETADATA
struct zisraw_meta_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t xml_size;
  int32_t _attach_size;
  char _reserved[248];
};

// SubBlock segment, SID = ZISRAWSUBBLOCK
struct zisraw_subblk_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t meta_size;
  int32_t attach_size;
  int64_t data_size;
  // followed by DirectoryEntryDV of this subblock
};

// Directory Entry - Schema DV
struct __attribute__ ((__packed__)) zisraw_dir_entry_dv {
  char schema[2];
  int32_t pixel_type;
  int64_t file_pos;
  int32_t _file_part;
  int32_t compression;
  int8_t pyramid_type;
  char _reserved1;
  char _reserved2[4];
  int32_t ndimensions;
  // followed by variable length array of zisraw_dim_entry_dv
};

// DimensionEntryDV1
struct zisraw_dim_entry_dv {
  char dimension[4];
  int32_t start;
  int32_t size;
  float start_coordinate;
  int32_t stored_size;
};

// AttachmentEntry - Schema A1
struct __attribute__ ((__packed__)) zisraw_att_entry_a1 {
  char schema[2];
  char _reserved2[10];
  int64_t file_pos;
  int32_t _file_part;
  char guid[16];
  char file_type[8];  // ZIP, ZISRAW, JPG etc.
  char name[80];      // Thumbnail, Label, SlidePreview etc.
};


// Attachment Segment, SID = ZISRAWATTACH
struct __attribute__ ((__packed__)) zisraw_seg_att_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t data_size;
  char _reserved1[12];
  struct zisraw_att_entry_a1 att_entry;
  char _reserved2[112];
  // followed by data
};

// AttachmentDirectory Segment, SID = ZISRAWATTDIR
struct zisraw_att_dir_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t entry_count;
  char _reserved[252];
  // followed by AttachementEntryA1 list
};

struct czi_subblk {
  int64_t file_pos;
  int64_t downsample_i;
  int32_t pixel_type;
  int32_t compression;
  int32_t x1, x2, y1, y2;
  uint32_t w, h, tw, th;
  int32_t id;
  int32_t dir_entry_len;
  int8_t channel;
  int8_t scene;
};

struct associated_image {
  struct _openslide_associated_image base;
  char *filename;
  int64_t data_offset;
  int32_t w, h;
  struct czi_subblk *subblk;
  int file_type;
};

struct czi_att_info {
  int64_t data_offset;
  int32_t w, h;
  int file_type;
};

static struct associated_image_mapping {
  char *czi_name;
  char *osr_name;
} known_associated_images[] = {
  {"Label", "label"},
  {"SlidePreview", "macro"},
  {"Thumbnail", "thumbnail"},
  {NULL, NULL},
};

enum z_pyramid_type {
  PYR_NONE = 0,
  PYR_SINGLE,
  PYR_MULTIPLE,
};

enum z_compression {
  COMP_NONE = 0,
  COMP_JPEG,
  COMP_LZW,
  COMP_JXR = 4,
  COMP_OTHER,
};

enum z_pixel_type {
  PT_GRAY8 = 0,
  PT_GRAY16,
  PT_GRAY32FLOAT,
  PT_BGR24,
  PT_BGR48,
  PT_BGR96FLOAT = 8,
  PT_BGRA32,
  PT_GRAY64COMPLEX,
  PT_BGR192COMPLEX,
  PT_GRAY32,
  PT_GRAY64,
};

enum z_attach_content_file_type {
  ATT_UNKNOWN = 0,
  ATT_CZI,
  ATT_JPG,
};

struct zeiss_ops_data {
  // offset to ZISRAWFILE, one for each file, usually 0. CZI file is like
  // Russian doll, it can embed other CZI files. Non-zero value is the
  // offset to embedded CZI file
  int64_t zisraw_offset;

  char *filename;
  int64_t subblk_dir_pos;
  int64_t meta_pos;
  int64_t att_dir_pos;
  int32_t nsubblk;  // total number of subblocks
  int32_t w;
  int32_t h;
  int32_t scene;
  int32_t offset_x;
  int32_t offset_y;
  GPtrArray *subblks;
  GHashTable *grids;
  GHashTable *count_levels;
};

struct level {
  struct _openslide_level base;
  int64_t downsample_i;
  int32_t compression;
};

static void destroy(openslide_t *osr);
static bool paint_region(openslide_t *osr, cairo_t *cr, int64_t x, int64_t y,
                         struct _openslide_level *level, int32_t w, int32_t h,
                         GError **err);

static const struct _openslide_ops zeiss_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static void destroy_level(struct level *l) {
  g_slice_free(struct level, l);
}

static void destroy_subblk(struct czi_subblk *p)
{
  g_slice_free(struct czi_subblk, p);
}

static void destroy_ops_data(struct zeiss_ops_data *data)
{
  g_free(data->filename);
  if (data->count_levels)
    g_hash_table_destroy(data->count_levels);

  if (data->grids)
    g_hash_table_destroy(data->grids);

  g_ptr_array_free(data->subblks, TRUE);
  g_slice_free(struct zeiss_ops_data, data);
}

static void destroy(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
  }
  g_free(osr->levels);

  destroy_ops_data(osr->data);
}

/* locate a grid based on level and let grid_paint_region do the job */
static bool paint_region(openslide_t *osr, cairo_t *cr,
                          int64_t x, int64_t y,
                          struct _openslide_level *level,
                          int32_t w, int32_t h,
                          GError **err)
{
  struct zeiss_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  int64_t ds = (int64_t) level->downsample;
  struct _openslide_grid *grid = g_hash_table_lookup(data->grids, &ds);
  void *unused_args = NULL;

  // need convert level 0 x,y to x,y on current level
  if (!_openslide_grid_paint_region(grid, cr, &unused_args,
                                    x / l->base.downsample,
                                    y / l->base.downsample,
                                    level, w, h, err)) {
      return false;
  }

  return true;
}

static bool zeiss_detect(const char *filename,
                         struct _openslide_tifflike *tl, GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f)
    return false;

  // string ZISRAWFILE occurs once per file, at positon 0
  char sid[CZI_SEG_ID_LEN];
  if (_openslide_fread(f, sid, CZI_SEG_ID_LEN) != CZI_SEG_ID_LEN) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read CZI file header");
    return false;
  }

  if (!g_str_has_prefix(sid, "ZISRAWFILE")) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a Zeiss CZI slide");
    return false;
  }
  return true;
}

static void read_dim_entry(struct zisraw_dim_entry_dv *p,
                           struct czi_subblk *sb)
{
  int start = GINT32_FROM_LE(p->start);
  int size = GINT32_FROM_LE(p->size);
  int stored_size = GINT32_FROM_LE(p->stored_size);

  switch (p->dimension[0]) {
  case 'X':
    sb->x1 = start;
    sb->w = size;
    sb->tw = stored_size;
    sb->x2 = start + size - 1;
    sb->downsample_i = DIV_ROUND_CLOSEST(size, stored_size);
    break;
  case 'Y':
    sb->y1 = start;
    sb->h = size;
    sb->th = stored_size;
    sb->y2 = start + size - 1;
    break;
  case 'C':
    sb->channel = start;
    break;
  case 'S':
    sb->scene = start;
    break;
  }
}

static int read_dir_entry(GPtrArray *subblks, char *p)
{
  struct czi_subblk *sb = g_slice_new0(struct czi_subblk);
  struct zisraw_dir_entry_dv *dv;
  char *b = p;
  size_t nread;
  int32_t ndim;

  dv = (struct zisraw_dir_entry_dv *) b;
  sb->pixel_type = GINT32_FROM_LE(dv->pixel_type);
  sb->compression = GINT32_FROM_LE(dv->compression);
  sb->file_pos = GINT64_FROM_LE(dv->file_pos);

  nread = sizeof(struct zisraw_dir_entry_dv);
  ndim = GINT32_FROM_LE(dv->ndimensions);
  b += nread;  // the first entry of dimensions
  for (int i = 0; i < ndim; i++) {
    read_dim_entry((struct zisraw_dim_entry_dv *)b , sb);
    b += 20;
  }

  nread += ndim * 20;
  sb->dir_entry_len = nread;
  g_ptr_array_add(subblks, sb);
  return nread;
}

/* read all data subblocks info (x, y, w, h etc.) from subblock directory */
static bool read_subblk_dir(struct zeiss_ops_data *data, GError **err)
{
  struct zisraw_subblk_dir_hdr *hdr;
  char buf[512];
  char *p;
  int dir_entry_len;
  size_t len;
  size_t total = 0;

  data->subblks = g_ptr_array_new_full(64, (GDestroyNotify) destroy_subblk);
  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (!f)
    return false;

  if (!_openslide_fseek(f, data->zisraw_offset + data->subblk_dir_pos,
                        SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to SubBlock directory");
    return false;
  }

  len  = sizeof(struct zisraw_subblk_dir_hdr);
  if (_openslide_fread(f, buf, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read FileHeader");
    return false;
  }
  hdr = (struct zisraw_subblk_dir_hdr *) buf;
  if (!g_str_has_prefix(hdr->seg_hdr.sid, "ZISRAWDIRECTORY")) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not SubBlockDirectory");
    return false;
  }

  data->nsubblk = GINT32_FROM_LE(hdr->entry_count);
  len = (size_t) GINT32_FROM_LE(hdr->seg_hdr.allocated_size);
  len -= 128;  // DirectoryEntryDV list starts at offset 128 of segment data
  g_autofree char *buf_dir = g_malloc(len);
  if (_openslide_fread(f, buf_dir, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read SubBlockDirectory");
    return false;
  }
  p = buf_dir;
  for (int i = 0; i < data->nsubblk; i++) {
    if (total > len) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Read beyond last byte of directory entires");
      return false;
    }

    dir_entry_len = read_dir_entry(data->subblks, p);
    p += dir_entry_len;
    total += dir_entry_len;
  }
  return true;
}

/* the topleft-most tile has none-zero (x, y), use its x,y as offset to adjust
 * x,y of other tiles.
 */
static bool adjust_coordinate_origin(struct zeiss_ops_data *data,
                                     GError **err G_GNUC_UNUSED)
{
  GPtrArray *subblks = data->subblks;
  struct czi_subblk *b;

  for (guint i = 0; i < subblks->len; i++) {
    b = subblks->pdata[i];
    if (b->x1 < data->offset_x)
      data->offset_x = b->x1;

    if (b->y1 < data->offset_y)
      data->offset_y = b->y1;
  }

  for (guint i = 0; i < subblks->len; i++) {
    b = subblks->pdata[i];
    b->x1 -= data->offset_x;
    b->y1 -= data->offset_y;
  }

  return true;
}

/* the uncompressed data in CZI also uses JXR pixel types such as BGR24 or
 * BGR48, thus use struct jxr_decoded for exchanging buffer
 */
static bool czi_uncompressed_read(const char *filename,
                                  int64_t pos, int64_t len,
                                  int32_t pixel_type,
                                  struct jxr_decoded *dst, GError **err)
{
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f)
    return false;

  if (!_openslide_fseek(f, pos, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to jxr pixel data");
    return false;
  }

  dst->size = len;
  dst->data = g_slice_alloc(len);
  if (_openslide_fread(f, dst->data, (size_t) len) != (size_t) len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read pixel data");
    g_slice_free1(dst->size, dst->data);
    return false;
  }

  switch (pixel_type) {
  case PT_BGR48:
    return convert_48bppbgr_to_cario24bpprgb(dst);
  case PT_GRAY16:
    return false;
  case PT_GRAY8:
    return false;
  default:
    break;
  }
  return convert_24bppbgr_to_cario24bpprgb(dst);
}

static bool read_data_from_subblk(const char *filename, int64_t zisraw_offset,
                                  struct czi_subblk *sb,
                                  struct jxr_decoded *dst,
                                  GError **err)
{
  struct zisraw_subblk_hdr *hdr;
  char buf[512];
  size_t len;
  int64_t data_pos, offset_meta, n;

  // work with BGR24, BGR48
  if (sb->pixel_type != PT_BGR24 && sb->pixel_type != PT_BGR48) {
    g_warning("read_data_at_address(): pixel type %d not supported\n",
              sb->pixel_type);
    return false;
  }

  dst->w = sb->tw;
  dst->h = sb->th;
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    g_warning("read_data_from_subblk(): cannot open file %s\n", filename);
    return false;
  }

  if (!_openslide_fseek(f, zisraw_offset + sb->file_pos, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to SubBlock");
    return false;
  }

  len  = sizeof(struct zisraw_subblk_hdr);
  if (_openslide_fread(f, buf, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read SubBlock header");
    return false;
  }
  hdr = (struct zisraw_subblk_hdr *)buf;
  n = MAX(256 - 16 - sb->dir_entry_len, 0);
  offset_meta = MAX(256, n);
  data_pos = zisraw_offset + sb->file_pos + sizeof(struct zisraw_seg_hdr) +
             offset_meta + GINT32_FROM_LE(hdr->meta_size);
  switch (sb->compression) {
  case COMP_NONE:
    czi_uncompressed_read(filename, data_pos, GINT64_FROM_LE(hdr->data_size),
                          sb->pixel_type, dst, NULL);
    break;
  case COMP_JXR:
    _openslide_jxr_read(filename, data_pos, GINT64_FROM_LE(hdr->data_size),
                        dst, NULL);
    break;
  case COMP_JPEG:
    g_warning("JPEG is not supported\n");
    return false;
  case COMP_LZW:
    g_warning("LZW is not supported\n");
    return false;
  default:
    g_warning("Unrecognized subblock format\n");
    return false;
  }
  return true;
}

static bool read_tile(openslide_t *osr, cairo_t *cr,
                      struct _openslide_level *level G_GNUC_UNUSED,
                      int64_t tid G_GNUC_UNUSED, void *tile_data,
                      void *arg G_GNUC_UNUSED, GError **err G_GNUC_UNUSED)
{
  struct zeiss_ops_data *data = (struct zeiss_ops_data *) osr->data;
  struct jxr_decoded dst;
  struct czi_subblk *sb = tile_data;
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  g_autoptr(cairo_surface_t) surface = NULL;
  unsigned char *img;
  int stride;

  // file_pos is unique
  img = (unsigned char *)_openslide_cache_get(osr->cache, 0, sb->file_pos, 0,
                                              &cache_entry);
  if (!img) {
    if (!read_data_from_subblk(data->filename, data->zisraw_offset,
                              sb, &dst, NULL)) {
      g_warning("read_data_from_subblk() failed\n");
      return false;
    }

    // _openslide_cache_entry_unref will free data
    _openslide_cache_put(osr->cache, 0, sb->file_pos, 0,
                         dst.data, dst.size, &cache_entry);
    img = (unsigned char *)dst.data;
  }

  stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, sb->tw);
  surface = cairo_image_surface_create_for_data(img, CAIRO_FORMAT_RGB24,
                                                sb->tw, sb->th, stride);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  return true;
}

static void finish_adding_tiles(void *key G_GNUC_UNUSED, void *value,
                                void *user_data G_GNUC_UNUSED)
{
  struct _openslide_grid *grid = (struct _openslide_grid *) value;
  _openslide_grid_range_finish_adding_tiles(grid);
}

static void destroy_int64_key(void *p)
{
  g_free(p);
}

static void count_levels(struct zeiss_ops_data *data, int64_t downsample)
{
  int64_t *k;
  void *unused;

  unused = g_hash_table_lookup(data->count_levels, &downsample);
  if (!unused) {
    k = g_new(int64_t, 1);
    *k = downsample;
    g_hash_table_insert(data->count_levels, k, NULL);
  }
}

static bool init_range_grids(openslide_t *osr, GError **err G_GNUC_UNUSED)
{
  struct zeiss_ops_data *data = osr->data;
  struct _openslide_grid *grid;
  struct level *l;
  GPtrArray *subblks = data->subblks;
  struct czi_subblk *b;
  int64_t *k;

  data->grids = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                      (GDestroyNotify) destroy_int64_key,
                                      (GDestroyNotify) _openslide_grid_destroy);
  for (int i = 0; i < osr->level_count; i++) {
    l = (struct level *) osr->levels[i];
    grid = _openslide_grid_create_range(osr, l->base.tile_w, l->base.tile_h,
                                        read_tile,
                                        NULL);
    k = g_new(int64_t, 1);
    *k = l->downsample_i;
    g_hash_table_insert(data->grids, k, grid);
  }

  for (guint i = 0; i < subblks->len; i++) {
    b = subblks->pdata[i];
    grid = g_hash_table_lookup(data->grids, &b->downsample_i);
    _openslide_grid_range_add_tile(grid,
                                   (double) b->x1 / b->downsample_i ,
                                   (double) b->y1 / b->downsample_i ,
                                   (double) b->tw, (double) b->th, b);
  }

  g_hash_table_foreach(data->grids, (GHFunc)finish_adding_tiles, NULL);
  return true;
}

static gint cmp_int64(gpointer a, gpointer b) {
  int64_t *x = (int64_t *)a;
  int64_t *y = (int64_t *)b;

  if (*x == *y)
    return 0;
  return (*x < *y) ?  -1 : 1;
}

static bool init_levels(openslide_t *osr, GError **err G_GNUC_UNUSED)
{
  struct zeiss_ops_data *data = osr->data;
  struct czi_subblk *b;
  struct level *l;
  GPtrArray *subblks = data->subblks;
  GPtrArray *levels = g_ptr_array_new();
  int64_t downsample_i;

  for (guint i = 0; i < subblks->len; i++) {
    b = subblks->pdata[i];
    count_levels(data, b->downsample_i);
  }

  GList *downsamples = g_hash_table_get_keys(data->count_levels);
  GList *p = g_list_sort(downsamples, (GCompareFunc) cmp_int64);
  downsamples = p;

  while (p) {
    downsample_i = *((int64_t *) p->data);
    l = g_slice_new0(struct level);
    l->base.downsample = (double) downsample_i;
    l->base.w = data->w / l->base.downsample;
    l->base.h = data->h / l->base.downsample;
    l->downsample_i = downsample_i;
    l->base.tile_w = 256;
    l->base.tile_h = 256;

    g_ptr_array_add(levels, l);
    p = p->next;
  }

  g_assert(osr->levels == NULL);
  osr->level_count = levels->len;
  osr->levels = (struct _openslide_level **) g_ptr_array_free(levels, false);
  g_list_free(downsamples);
  return true;
}

/* locate offset to metadata, to subblock and attachment directory */
static bool load_dir_position(struct zeiss_ops_data *data, GError **err)
{
  struct zisraw_data_file_hdr *hdr;
  char buf[512];

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (!f)
    return false;

  if (!_openslide_fseek(f, data->zisraw_offset, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to FileHeaderSegment start: ");
    return false;
  }

  size_t len = sizeof(struct zisraw_data_file_hdr);
  if (_openslide_fread(f, buf, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read FileHeader");
    return false;
  }
  hdr = (struct zisraw_data_file_hdr *) buf;
  data->subblk_dir_pos = GINT64_FROM_LE(hdr->subblk_dir_pos);
  data->meta_pos = GINT64_FROM_LE(hdr->meta_pos);
  data->att_dir_pos = GINT64_FROM_LE(hdr->att_dir_pos);
  return true;
}

static void set_prop(openslide_t *osr, const char *name, const char *value)
{
  if (value)
    g_hash_table_insert(osr->properties, g_strdup(name), g_strdup(value));
}

/* parse XML and set standard openslide properties. Also set width, height in
 * ops_data
 */
static void parse_xml_set_prop(openslide_t *osr, const char *xml, GError **err)
{
  struct zeiss_ops_data *data = osr->data;
  double d;
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
  if (doc == NULL) {
    g_printerr("Error: cannot parse XML to xmlDoc\n");
    return;
  }

  /* part of XML structure:

    ImageDocument
        Metadata
            Experiment
            HardwareSetting
            CustomAttributes
            Information
                User
                Application
                Document
                Image
                    ComponentBitCount
                    PixelType
                    SizeC
                    SizeX
                    SizeY

                    Dimensions
                        Channels
                            Channel
                            Channel
                        Tracks
                            Track
                            Track
                Instrument
                    Microscopes
                        <Microscope Id="Microscope:1" Name="Axioscan 7">
                    Objectives
                        Objective
                            NominalMagnification  (objective-power)
              Scaling
                  Items
                      <Distance Id="X">  (mpp X)
                          Value  (3.4443237544526617E-07, in meter)
                      <Distance Id="Y">  (mpp Y)
                          Value
  */
  g_autoptr(xmlXPathContext) ctx = _openslide_xml_xpath_create(doc);

  g_autofree char *size_x =
    _openslide_xml_xpath_get_string(ctx,
      "/ImageDocument/Metadata/Information/Image/SizeX/text()");
  data->w = (int32_t) atol(size_x);

  g_autofree char *size_y =
    _openslide_xml_xpath_get_string(ctx,
      "/ImageDocument/Metadata/Information/Image/SizeY/text()");
  data->h = (int32_t) atol(size_y);

  g_autofree char *size_s =
    _openslide_xml_xpath_get_string(ctx,
      "/ImageDocument/Metadata/Information/Image/SizeS/text()");
  data->scene = (int32_t) atol(size_s);

  // in meter/pixel
  g_autofree char *mpp_x =
    _openslide_xml_xpath_get_string(ctx,
      "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='X']/Value/text()");
  d = _openslide_parse_double(mpp_x);
  g_ascii_dtostr(buf, sizeof(buf), d * 1000000.0);
  // in um/pixel
  set_prop(osr, OPENSLIDE_PROPERTY_NAME_MPP_X, buf);

  g_autofree char *mpp_y =
    _openslide_xml_xpath_get_string(ctx,
      "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='Y']/Value/text()");
  d = _openslide_parse_double(mpp_y);
  g_ascii_dtostr(buf, sizeof(buf), d * 1000000.0);
  set_prop(osr, OPENSLIDE_PROPERTY_NAME_MPP_Y, buf);

  g_autofree char *obj =
    _openslide_xml_xpath_get_string(ctx,
      "/ImageDocument/Metadata/Information/Instrument/Objectives/Objective/NominalMagnification/text()");
  set_prop(osr, OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER, obj);

  set_prop(osr, OPENSLIDE_PROPERTY_NAME_VENDOR, "zeiss");
}

static char *read_czi_meta_xml(struct zeiss_ops_data *data, GError **err)
{
  struct zisraw_meta_hdr *hdr;
  size_t len;
  char buf[512];
  g_autofree char *xml = NULL;

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (!f)
    return NULL;

  if (!_openslide_fseek(f, data->zisraw_offset + data->meta_pos,
                        SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to MetaBlock");
    return NULL;
  }

  len = sizeof(struct zisraw_meta_hdr);
  if (_openslide_fread(f, buf, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read MetaBlock header");
    return NULL;
  }
  hdr = (struct zisraw_meta_hdr *) buf;
  len = (size_t) GINT32_FROM_LE(hdr->xml_size);
  xml = g_malloc(len + 1);
  if (_openslide_fread(f, xml, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read MetaBlock xml");
    return NULL;
  }
  xml[len] = '\0';
  return g_steal_pointer(&xml);
}

/* find offset to embedded image with @name, such as Label */
static bool locate_attachment_by_name(struct zeiss_ops_data *data,
                                      struct czi_att_info *att_info,
                                      const char *name, GError **err)
{
  struct zisraw_att_dir_hdr *hdr;
  struct zisraw_att_entry_a1 *att;
  size_t len;
  int nattch;
  char buf[512];

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (!f)
    return false;

  if (!_openslide_fseek(f, data->zisraw_offset + data->att_dir_pos,
                        SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to attachment directory: ");
    return false;
  }

  len = sizeof(struct zisraw_att_dir_hdr);
  if (_openslide_fread(f, buf, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read FileHeader");
    return false;
  }
  hdr = (struct zisraw_att_dir_hdr *) buf;
  nattch = GINT32_FROM_LE(hdr->entry_count);

  len = sizeof(struct zisraw_att_entry_a1);
  for (int i = 0; i < nattch; i++) {
    if (_openslide_fread(f, buf, len) != len) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot read attachment directory entry");
      return false;
    }

    att = (struct zisraw_att_entry_a1 *) buf;
    if (g_strcmp0(att->name, name) == 0) {
      // + 32 bytes segment header + 256 bytes offset
      att_info->data_offset = att->file_pos + 32 + 256;
      if (g_strcmp0(att->file_type, "JPG") == 0)
        att_info->file_type = ATT_JPG;
      else if (g_strcmp0(att->file_type, "CZI") == 0)
        att_info->file_type = ATT_CZI;

      break;
    }
  }

  if (att_info->file_type == ATT_JPG)
    _openslide_jpeg_read_dimensions(data->filename, att_info->data_offset,
                                    &att_info->w, &att_info->h, NULL);
  return true;
}

/* @dst is pre-allocated by openslide, size 4 * w * h */
static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dst,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;
  struct jxr_decoded cbuf;

  switch (img->file_type) {
  case ATT_CZI:
    if (!read_data_from_subblk(img->filename, img->data_offset, img->subblk,
                               &cbuf, err))
      return false;

    memcpy(dst, cbuf.data, cbuf.size);
    g_slice_free1(cbuf.size, cbuf.data);
    return true;
  case ATT_JPG:
    _openslide_jpeg_read(img->filename, img->data_offset, dst,
                         img->base.w, img->base.h, err);
    return true;
  }
  return false;
}

static void destroy_associated_image(struct _openslide_associated_image *_img)
{
  struct associated_image *img = (struct associated_image *) _img;

  g_free(img->filename);
  if (img->subblk)
    g_slice_free(struct czi_subblk, img->subblk);

  g_slice_free(struct associated_image, img);
}

static const struct _openslide_associated_image_ops zeiss_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

static bool _add_associated_image(openslide_t *osr, const char *filename,
                                  const char *name,
                                  struct czi_att_info *att_info,
                                  struct czi_subblk *sb,
                                  GError **err G_GNUC_UNUSED)
{
  struct associated_image *img = g_slice_new0(struct associated_image);

  img->base.ops = &zeiss_associated_ops;
  img->filename = g_strdup(filename);
  img->file_type = att_info->file_type;
  img->data_offset = att_info->data_offset;
  if (sb) {
    img->base.w = sb->tw;
    img->base.h = sb->th;
    img->subblk = g_slice_new(struct czi_subblk);
    memcpy(img->subblk, sb, sizeof(*sb));
  } else {
    img->subblk = NULL;
    img->base.w = att_info->w;
    img->base.h = att_info->h;
  }

  g_hash_table_insert(osr->associated_images, g_strdup(name), img);
  return true;
}

static bool zeiss_add_associated_image(openslide_t *osr, GError **err)
{
  struct zeiss_ops_data *outer_data = (struct zeiss_ops_data *) osr->data;
  struct zeiss_ops_data *data = NULL;
  struct czi_subblk *sb = NULL;
  struct associated_image_mapping *map = &known_associated_images[0];
  struct czi_att_info att_info;

  for ( ; map->czi_name; map++) {
    // read the outermost CZI to get offset to ZISRAWFILE, or to JPEG
    memset(&att_info, 0, sizeof(struct czi_att_info));
    locate_attachment_by_name(outer_data, &att_info, map->czi_name, err);
    if (att_info.data_offset == 0)
      continue;

    if (att_info.file_type == ATT_CZI) {
      data = g_slice_new0(struct zeiss_ops_data);
      data->filename = g_strdup(outer_data->filename);
      data->zisraw_offset = att_info.data_offset;

      // knowing offset to ZISRAWFILE, now parse the embeded CZI
      load_dir_position(data, err);
      read_subblk_dir(data, err);
      // expect the embeded CZI file has only one image subblock
      sb = (struct czi_subblk *)data->subblks->pdata[0];
    }

    _add_associated_image(osr, outer_data->filename, map->osr_name,
                          &att_info, sb, err);
    if (data) {
      destroy_ops_data(data);
      data = NULL;
      sb = NULL;
    }
  }
  return true;
}

static bool zeiss_open(openslide_t *osr, const char *filename,
                       struct _openslide_tifflike *t G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1 G_GNUC_UNUSED,
                       GError **err)
{
  struct zeiss_ops_data *data = g_slice_new0(struct zeiss_ops_data);

  osr->data = data;
  osr->ops = &zeiss_ops;
  data->zisraw_offset = 0;
  data->offset_x = G_MAXINT32;
  data->offset_y = G_MAXINT32;
  data->filename = g_strdup(filename);
  data->count_levels =
    g_hash_table_new_full(g_int64_hash, g_int64_equal,
                          (GDestroyNotify) destroy_int64_key, NULL);
  load_dir_position(data, err);
  read_subblk_dir(data, err);
  adjust_coordinate_origin(data, NULL);

  g_autofree char *xml = read_czi_meta_xml(data, err);
  parse_xml_set_prop(osr, xml, err);
  init_levels(osr, err);
  init_range_grids(osr, err);
  zeiss_add_associated_image(osr, err);
  return true;
}

const struct _openslide_format _openslide_format_zeiss = {
  .name = "zeiss",
  .vendor = "zeiss",
  .detect = zeiss_detect,
  .open = zeiss_open,
};
