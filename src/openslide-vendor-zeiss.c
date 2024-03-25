/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2024 Benjamin Gilbert
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

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CZI_FILEHDR_LEN 544
#define CZI_SUBBLK_HDR_LEN 288

static const char SID_ZISRAWATTDIR[] = "ZISRAWATTDIR";
static const char SID_ZISRAWDIRECTORY[] = "ZISRAWDIRECTORY";
static const char SID_ZISRAWFILE[] = "ZISRAWFILE";
static const char SID_ZISRAWMETADATA[] = "ZISRAWMETADATA";
static const char SID_ZISRAWSUBBLOCK[] = "ZISRAWSUBBLOCK";

static const char SCHEMA_A1[] = "A1";
static const char SCHEMA_DV[] = "DV";

#define DIV_ROUND_CLOSEST(n, d)                                                \
  ((((n) < 0) != ((d) < 0)) ? (((n) - (d) / 2) / (d)) : (((n) + (d) / 2) / (d)))

/* zeiss uses little-endian */
struct zisraw_seg_hdr {
  char sid[16];
  int64_t allocated_size;
  int64_t used_size;
};

// beginning of a CZI file, SID = ZISRAWFILE
struct __attribute__((__packed__)) zisraw_data_file_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t major;
  int32_t minor;
  int32_t _reserved1;
  int32_t _reserved2;
  char primary_file_guid[16];
  char file_guid[16];
  int32_t file_part; // this causes off-align
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
  // followed by DirectoryEntryDV of this subblock, followed by padding
  // to 288 bytes, followed by meta (and attach?) and data
};

// Directory Entry - Schema DV
struct __attribute__((__packed__)) zisraw_dir_entry_dv {
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
struct __attribute__((__packed__)) zisraw_att_entry_a1 {
  char schema[2];
  char _reserved2[10];
  int64_t file_pos;
  int32_t _file_part;
  char guid[16];
  char file_type[8]; // ZIP, ZISRAW, JPG etc.
  char name[80];     // Thumbnail, Label, SlidePreview etc.
};

// Attachment Segment, SID = ZISRAWATTACH
struct __attribute__((__packed__)) zisraw_seg_att_hdr {
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

enum zisraw_compression {
  COMP_NONE = 0,
  COMP_JPEG,
  COMP_LZW,
  COMP_JXR = 4,
  COMP_ZSTD0 = 5,
  COMP_ZSTD1 = 6,
  COMP_OTHER,
};

enum zisraw_pixel_type {
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

enum zisraw_pyramid_type {
  PYR_NONE = 0,
  PYR_SINGLE,
  PYR_MULTIPLE,
};

enum czi_attach_content_file_type {
  ATT_CZI,
  ATT_JPG,
};

struct czi_subblk {
  int64_t file_pos;
  int64_t downsample_i;
  int32_t pixel_type;
  int32_t compression;
  int32_t x1, x2, y1, y2;
  uint32_t w, h, tw, th;
  int32_t dir_entry_len;
  int8_t scene;
};

struct associated_image {
  struct _openslide_associated_image base;
  struct czi *czi;
  int64_t data_offset;
  struct czi_subblk *subblk;
  enum czi_attach_content_file_type file_type;
};

struct czi_att_info {
  int64_t data_offset;
  int32_t w, h;
  enum czi_attach_content_file_type file_type;
};

struct czi_decbuf {
  uint8_t *data;
  uint32_t w;
  uint32_t h;
  size_t size;
};

static const struct associated_image_mapping {
  const char *czi_name;
  const char *osr_name;
} known_associated_images[] = {
    {"Label", "label"},
    {"SlidePreview", "macro"},
    {"Thumbnail", "thumbnail"},
    {NULL, NULL},
};

static const struct czi_pixel_type_name {
  enum zisraw_pixel_type pixel_type;
  const char *name;
} czi_pixel_type_names[] = {
    {PT_GRAY8, "GRAY8"},
    {PT_GRAY16, "GRAY16"},
    {PT_GRAY32FLOAT, "GRAY32FLOAT"},
    {PT_BGR24, "BGR24"},
    {PT_BGR48, "BGR48"},
    {5, "5"},
    {6, "6"},
    {7, "7"},
    {PT_BGR96FLOAT, "BGR96FLOAT"},
    {PT_BGRA32, "BGRA32"},
    {PT_GRAY64COMPLEX, "GRAY64COMPLEX"},
    {PT_BGR192COMPLEX, "BGR192COMPLEX"},
    {PT_GRAY32, "GRAY32"},
    {PT_GRAY64, "GRAY64"},
};

// for finding location of each scene (region) and pyramid level
struct czi_scene {
  int64_t x1;
  int64_t y1;
  int64_t x2;
  int64_t y2;
  int64_t max_downsample;
};

struct czi {
  // offset to ZISRAWFILE, one for each file, usually 0. CZI file is like
  // Russian doll, it can embed other CZI files. Non-zero value is the
  // offset to embedded CZI file
  int64_t zisraw_offset;

  char *filename;
  int64_t subblk_dir_pos;
  int64_t meta_pos;
  int64_t att_dir_pos;
  int64_t common_downsample;
  int32_t nsubblk; // total number of subblocks
  int32_t w;
  int32_t h;
  int32_t nscene;
  int32_t offset_x;
  int32_t offset_y;
  struct czi_subblk *subblks;
  struct czi_scene *scenes;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
  int64_t downsample_i;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}

static void destroy_czi(struct czi *czi) {
  g_free(czi->filename);
  g_free(czi->subblks);
  g_free(czi->scenes);
  g_free(czi);
}
typedef struct czi czi;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(czi, destroy_czi)

static void destroy(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
  }
  g_free(osr->levels);

  if (osr->data) {
    destroy_czi(osr->data);
  }
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct czi *czi = osr->data;
  struct level *l = (struct level *) level;

  g_autoptr(_openslide_file) f = _openslide_fopen(czi->filename, err);
  if (!f) {
    return false;
  }
  return _openslide_grid_paint_region(l->grid, cr, f,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h, err);
}

static const struct _openslide_ops zeiss_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool freadn_to_buf(struct _openslide_file *f, off_t offset,
                          void *buf, size_t len, GError **err) {
  if (!_openslide_fseek(f, offset, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to offset %"PRId64": ", offset);
    return false;
  }
  if (_openslide_fread(f, buf, len) != len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Short read: wanted %"PRIu64" bytes at offset %"PRId64,
                (uint64_t) len, (int64_t) offset);
    return false;
  }
  return true;
}

static bool check_magic(const void *found, const char *expected,
                        GError **err) {
  // don't expect trailing null byte
  if (memcmp(expected, found, strlen(expected))) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No magic string \"%s\" in struct header", expected);
    return false;
  }
  return true;
}

static bool zeiss_detect(const char *filename,
                         struct _openslide_tifflike *tl, GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "Is a TIFF file");
    return false;
  }

  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  struct zisraw_seg_hdr hdr;
  if (!freadn_to_buf(f, 0, &hdr, sizeof(hdr), err)) {
    return false;
  }
  if (!check_magic(hdr.sid, SID_ZISRAWFILE, err)) {
    g_prefix_error(err, "Not a Zeiss CZI file: ");
    return false;
  }
  return true;
}

static bool read_dim_entry(struct czi_subblk *sb, char **p, size_t *avail,
                           GError **err) {
  const size_t len = sizeof(struct zisraw_dim_entry_dv);
  if (*avail < len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Premature end of directory when reading dimension");
    return false;
  }
  struct zisraw_dim_entry_dv *dim = (struct zisraw_dim_entry_dv *) *p;
  *p += len;
  *avail -= len;

  int start = GINT32_FROM_LE(dim->start);
  int size = GINT32_FROM_LE(dim->size);
  int stored_size = GINT32_FROM_LE(dim->stored_size);

  switch (dim->dimension[0]) {
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
  case 'S':
    sb->scene = start;
    break;
  }
  return true;
}

static bool read_dir_entry(struct czi_subblk *sb, char **p, size_t *avail,
                           GError **err) {
  const size_t len = sizeof(struct zisraw_dir_entry_dv);
  if (*avail < len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Premature end of directory when reading directory entry");
    return false;
  }
  struct zisraw_dir_entry_dv *dv = (struct zisraw_dir_entry_dv *) *p;
  *p += len;
  *avail -= len;

  if (!check_magic(dv->schema, SCHEMA_DV, err)) {
    return false;
  }

  sb->pixel_type = GINT32_FROM_LE(dv->pixel_type);
  sb->compression = GINT32_FROM_LE(dv->compression);
  sb->file_pos = GINT64_FROM_LE(dv->file_pos);
  int32_t ndim = GINT32_FROM_LE(dv->ndimensions);

  for (int i = 0; i < ndim; i++) {
    if (!read_dim_entry(sb, p, avail, err)) {
      return false;
    }
  }
  sb->dir_entry_len = *p - (char *) dv;
  return true;
}

/* read all data subblocks info (x, y, w, h etc.) from subblock directory */
static bool read_subblk_dir(struct czi *czi, struct _openslide_file *f,
                            GError **err) {
  int64_t offset = czi->zisraw_offset + czi->subblk_dir_pos;
  struct zisraw_subblk_dir_hdr hdr;
  if (!freadn_to_buf(f, offset, &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read FileHeader: ");
    return false;
  }
  offset += sizeof(hdr);

  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWDIRECTORY, err)) {
    return false;
  }

  czi->nsubblk = GINT32_FROM_LE(hdr.entry_count);
  int64_t seg_size =
    GINT32_FROM_LE(hdr.seg_hdr.used_size) - sizeof(hdr) + sizeof(hdr.seg_hdr);
  g_autofree char *buf_dir = g_try_malloc(seg_size);
  if (!buf_dir) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate %"PRId64" bytes for SubBlockDirectory",
                seg_size);
    return false;
  }
  if (!freadn_to_buf(f, offset, buf_dir, seg_size, err)) {
    g_prefix_error(err, "Couldn't read SubBlockDirectory: ");
    return false;
  }

  char *p = buf_dir;
  size_t avail = seg_size;
  czi->subblks = g_try_new0(struct czi_subblk, czi->nsubblk);
  if (!czi->subblks) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate memory for %d subblocks", czi->nsubblk);
    return false;
  }
  for (int i = 0; i < czi->nsubblk; i++) {
    if (!read_dir_entry(&czi->subblks[i], &p, &avail, err)) {
      return false;
    }
  }
  if (avail) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Found %"PRIu64" trailing bytes after SubBlockDirectory",
                (uint64_t) avail);
    return false;
  }
  return true;
}

/* the topleft-most tile has none-zero (x, y), use its x,y as offset to adjust
 * x,y of other tiles.
 */
static void adjust_coordinate_origin(struct czi *czi) {
  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    if (b->x1 < czi->offset_x) {
      czi->offset_x = b->x1;
    }
    if (b->y1 < czi->offset_y) {
      czi->offset_y = b->y1;
    }
  }

  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    b->x1 -= czi->offset_x;
    b->y1 -= czi->offset_y;
  }
}

#define BGR24TOXRGB32(p)						\
  (0xFF000000 | (uint32_t)((p)[0]) | ((uint32_t)((p)[1]) << 8) |	\
   ((uint32_t)((p)[2]) << 16))

#define BGR48TOXRGB32(p)						\
  (0xFF000000 | (uint32_t)((p)[1]) | ((uint32_t)((p)[3]) << 8) |	\
   ((uint32_t)((p)[5]) << 16))

static void bgr24_to_xrgb32(uint8_t *src, size_t src_len, uint8_t *dst) {
  uint32_t *p = (uint32_t *) dst;
  size_t i = 0;
  /* one 24-bits pixels a time */
  while (i < src_len) {
    *p++ = BGR24TOXRGB32(src);
    i += 3;
    src += 3;
  }
}

static void bgr48_to_xrgb32(uint8_t *src, size_t src_len, uint8_t *dst) {
  uint32_t *p = (uint32_t *) dst;
  size_t i = 0;
  /* one 48-bits pixels a time */
  while (i < src_len) {
    *p++ = BGR48TOXRGB32(src);
    i += 6;
    src += 6;
  }
}

static bool czi_bgrn_to_xrgb32(struct czi_decbuf *p, int in_pixel_bits) {
  if (in_pixel_bits != 24 && in_pixel_bits != 48) {
    return false;
  }

  size_t new_size = p->w * p->h * 4;
  g_autofree uint8_t *buf = g_malloc(new_size);
  if (in_pixel_bits == 24) {
    bgr24_to_xrgb32(p->data, p->size, buf);
  } else if (in_pixel_bits == 48) {
    bgr48_to_xrgb32(p->data, p->size, buf);
  }
  g_free(p->data);
  p->size = new_size;
  p->data = g_steal_pointer(&buf);
  return true;
}

/* the uncompressed data in CZI also uses pixel types such as BGR24 or
 * BGR48, thus use struct czi_decbuf for exchanging buffer
 */
static bool czi_read_uncompressed(struct _openslide_file *f, int64_t pos,
                                  int64_t len, int32_t pixel_type,
                                  struct czi_decbuf *dst, GError **err) {
  dst->size = len;
  dst->data = g_try_malloc(len);
  if (!dst->data) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate %"PRId64" bytes for uncompressed pixels",
                len);
    return false;
  }
  if (!freadn_to_buf(f, pos, dst->data, len, err)) {
    g_prefix_error(err, "Couldn't read pixel data: ");
    g_free(dst->data);
    return false;
  }

  switch (pixel_type) {
  case PT_BGR48:
    return czi_bgrn_to_xrgb32(dst, 48);
  case PT_GRAY16:
    return false;
  case PT_GRAY8:
    return false;
  default:
    break;
  }
  return czi_bgrn_to_xrgb32(dst, 24);
}

static bool read_subblk(struct _openslide_file *f,
                        int64_t zisraw_offset, struct czi_subblk *sb,
                        struct czi_decbuf *dst, GError **err) {
  // work with BGR24, BGR48
  if (sb->pixel_type != PT_BGR24 && sb->pixel_type != PT_BGR48) {
    if (sb->pixel_type >= 0 &&
        sb->pixel_type < (int) G_N_ELEMENTS(czi_pixel_type_names)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "pixel type %s is not supported",
                  czi_pixel_type_names[sb->pixel_type].name);
      return false;
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "pixel type %d is not supported", sb->pixel_type);
      return false;
    }
  }

  dst->w = sb->tw;
  dst->h = sb->th;
  struct zisraw_subblk_hdr hdr;
  if (!freadn_to_buf(f, zisraw_offset + sb->file_pos,
                     &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read SubBlock header: ");
    return false;
  }
  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWSUBBLOCK, err)) {
    return false;
  }

  int64_t data_pos = zisraw_offset + sb->file_pos + CZI_SUBBLK_HDR_LEN +
                     GINT32_FROM_LE(hdr.meta_size);
  int64_t data_size = GINT64_FROM_LE(hdr.data_size);
  switch (sb->compression) {
  case COMP_NONE:
    return czi_read_uncompressed(f, data_pos, data_size, sb->pixel_type, dst,
                                 NULL);
  case COMP_JXR:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "JPEG XR is not supported");
    return false;
  case COMP_JPEG:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "JPEG is not supported");
    return false;
  case COMP_LZW:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "LZW is not supported");
    return false;
  case COMP_ZSTD0:
  case COMP_ZSTD1:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "ZSTD is not supported");
    return false;
  default:
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unrecognized subblock format");
    return false;
  }
  return true;
}

static bool read_tile(openslide_t *osr, cairo_t *cr,
                      struct _openslide_level *level G_GNUC_UNUSED,
                      int64_t tid G_GNUC_UNUSED, void *tile_data,
                      void *arg, GError **err) {
  struct czi *czi = osr->data;
  struct _openslide_file *f = arg;
  struct czi_subblk *sb = tile_data;

  // file position of a subblock is unique
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  int64_t key_x = czi->zisraw_offset + sb->file_pos;
  unsigned char *img = (unsigned char *) _openslide_cache_get(
      osr->cache, 0, key_x, 0, &cache_entry);
  if (!img) {
    struct czi_decbuf dst;
    if (!read_subblk(f, czi->zisraw_offset, sb, &dst, err)) {
      return false;
    }

    // _openslide_cache_entry_unref will free data
    _openslide_cache_put(osr->cache, 0, key_x, 0, dst.data, dst.size,
                         &cache_entry);
    img = (unsigned char *) dst.data;
  }

  int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, sb->tw);
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data(img, CAIRO_FORMAT_RGB24, sb->tw,
                                        sb->th, stride);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  return true;
}

static void init_range_grids(openslide_t *osr, struct czi *czi) {
  g_autoptr(GHashTable) grids =
    g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
  for (int i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    l->grid = _openslide_grid_create_range(osr,
                                           l->base.tile_w, l->base.tile_h,
                                           read_tile, NULL);
    int64_t *k = g_new(int64_t, 1);
    *k = l->downsample_i;
    g_hash_table_insert(grids, k, l->grid);
  }

  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    if (b->downsample_i > czi->common_downsample) {
      continue;
    }

    struct _openslide_grid *grid = g_hash_table_lookup(grids, &b->downsample_i);
    g_assert(grid);
    _openslide_grid_range_add_tile(grid, (double) b->x1 / b->downsample_i,
                                   (double) b->y1 / b->downsample_i,
                                   (double) b->tw, (double) b->th, b);
  }

  for (int i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_range_finish_adding_tiles(l->grid);
  }
}

static gint cmp_int64(gpointer a, gpointer b) {
  int64_t *x = (int64_t *) a;
  int64_t *y = (int64_t *) b;

  if (*x == *y) {
    return 0;
  }
  return (*x < *y) ? -1 : 1;
}

/* Scenes on a slide may have different sizes and pyramid levels. For example,
 * rat kidney is likely to have more levels than a mouse kidney on the same
 * slide. Find the maximum downsample value available on all scenes and use it
 * to set the total levels. It helps deepzoom to show all sections on a slide
 * at max zoom out.
 */
static int64_t get_common_downsample(struct czi *czi) {
  int64_t downsample = INT64_MAX;

  for (int i = 0; i < czi->nscene; i++) {
    struct czi_scene *s = &czi->scenes[i];
    if (downsample > s->max_downsample) {
      downsample = s->max_downsample;
    }
  }
  return downsample;
}

static void init_levels(openslide_t *osr, struct czi *czi) {
  GPtrArray *levels = g_ptr_array_new();

  g_autoptr(GHashTable) count_levels =
    g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    if (!g_hash_table_lookup(count_levels, &b->downsample_i)) {
      int64_t *k = g_new(int64_t, 1);
      *k = b->downsample_i;
      g_hash_table_insert(count_levels, k, GINT_TO_POINTER(1));
    }
  }

  g_autoptr(GList) downsamples = g_hash_table_get_keys(count_levels);
  downsamples = g_list_sort(downsamples, (GCompareFunc) cmp_int64);

  GList *p = downsamples;
  czi->common_downsample = get_common_downsample(czi);
  while (p) {
    int64_t downsample_i = *((int64_t *) p->data);
    if (downsample_i > czi->common_downsample) {
      break;
    }

    struct level *l = g_new0(struct level, 1);
    l->base.downsample = (double) downsample_i;
    l->base.w = czi->w / l->base.downsample;
    l->base.h = czi->h / l->base.downsample;
    l->downsample_i = downsample_i;
    l->base.tile_w = 256;
    l->base.tile_h = 256;

    g_ptr_array_add(levels, l);
    p = p->next;
  }

  g_assert(osr->levels == NULL);
  osr->level_count = levels->len;
  osr->levels = (struct _openslide_level **) g_ptr_array_free(levels, false);
}

/* locate offset to metadata, to subblock and attachment directory */
static bool load_dir_position(struct czi *czi,
                              struct _openslide_file *f, GError **err) {
  struct zisraw_data_file_hdr hdr;
  if (!freadn_to_buf(f, czi->zisraw_offset, &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read FileHeader: ");
    return false;
  }
  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWFILE, err)) {
    return false;
  }

  czi->subblk_dir_pos = GINT64_FROM_LE(hdr.subblk_dir_pos);
  czi->meta_pos = GINT64_FROM_LE(hdr.meta_pos);
  czi->att_dir_pos = GINT64_FROM_LE(hdr.att_dir_pos);
  return true;
}

/* parse XML and set standard openslide properties. Also set width, height in
 * czi
 */
static bool parse_xml_set_prop(openslide_t *osr, struct czi *czi,
                               const char *xml, GError **err) {
  g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
  if (doc == NULL) {
    return false;
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

  g_autofree char *size_x = _openslide_xml_xpath_get_string(
      ctx, "/ImageDocument/Metadata/Information/Image/SizeX/text()");
  czi->w = (int32_t) atol(size_x);

  g_autofree char *size_y = _openslide_xml_xpath_get_string(
      ctx, "/ImageDocument/Metadata/Information/Image/SizeY/text()");
  czi->h = (int32_t) atol(size_y);

  g_autofree char *size_s = _openslide_xml_xpath_get_string(
      ctx, "/ImageDocument/Metadata/Information/Image/SizeS/text()");
  czi->nscene = (int32_t) atol(size_s);

  // in meter/pixel
  g_autofree char *mpp_x = _openslide_xml_xpath_get_string(
      ctx,
      "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='X']/Value/text()");
  if (mpp_x) {
    double d = _openslide_parse_double(mpp_x);
    if (!isnan(d)) {
      // in um/pixel
      g_hash_table_insert(osr->properties,
                          g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                          _openslide_format_double(d * 1000000.0));
    }
  }

  g_autofree char *mpp_y = _openslide_xml_xpath_get_string(
      ctx,
      "/ImageDocument/Metadata/Scaling/Items/Distance[@Id='Y']/Value/text()");
  if (mpp_y) {
    double d = _openslide_parse_double(mpp_y);
    if (!isnan(d)) {
      // in um/pixel
      g_hash_table_insert(osr->properties,
                          g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                          _openslide_format_double(d * 1000000.0));
    }
  }

  g_autofree char *obj = _openslide_xml_xpath_get_string(
      ctx, "/ImageDocument/Metadata/Information/Instrument/Objectives/"
           "Objective/NominalMagnification/text()");
  if (obj) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER),
                        g_strdup(obj));
  }

  return true;
}

static char *read_czi_meta_xml(struct czi *czi,
                               struct _openslide_file *f, GError **err) {
  int64_t offset = czi->zisraw_offset + czi->meta_pos;
  struct zisraw_meta_hdr hdr;
  if (!freadn_to_buf(f, offset, &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read MetaBlock header: ");
    return NULL;
  }
  offset += sizeof(hdr);

  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWMETADATA, err)) {
    return NULL;
  }

  int64_t xml_size = GINT32_FROM_LE(hdr.xml_size);
  g_autofree char *xml = g_try_malloc(xml_size + 1);
  if (!xml) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate %"PRId64" bytes for metadata XML",
                xml_size + 1);
    return NULL;
  }
  if (!freadn_to_buf(f, offset, xml, xml_size, err)) {
    g_prefix_error(err, "Couldn't read MetaBlock xml: ");
    return NULL;
  }

  xml[xml_size] = 0;
  return g_steal_pointer(&xml);
}

/* find offset to embedded image with @name, such as Label */
static bool locate_attachment_by_name(struct czi *czi,
                                      struct czi_att_info *att_info,
                                      const char *name, GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(czi->filename, err);
  if (!f) {
    return false;
  }
  if (!_openslide_fseek(f, czi->zisraw_offset + czi->att_dir_pos, SEEK_SET,
                        err)) {
    g_prefix_error(err, "Couldn't seek to attachment directory: ");
    return false;
  }

  struct zisraw_att_dir_hdr hdr;
  if (_openslide_fread(f, &hdr, sizeof(hdr)) != sizeof(hdr)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read FileHeader");
    return false;
  }
  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWATTDIR, err)) {
    return false;
  }

  int nattch = GINT32_FROM_LE(hdr.entry_count);

  for (int i = 0; i < nattch; i++) {
    struct zisraw_att_entry_a1 att;
    if (_openslide_fread(f, &att, sizeof(att)) != sizeof(att)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read attachment directory entry");
      return false;
    }
    if (!check_magic(att.schema, SCHEMA_A1, err)) {
      return false;
    }

    if (g_str_equal(att.name, name)) {
      att_info->data_offset = att.file_pos + sizeof(struct zisraw_seg_att_hdr);
      if (g_str_equal(att.file_type, "JPG")) {
        att_info->file_type = ATT_JPG;
        if (!_openslide_jpeg_read_file_dimensions(f, att_info->data_offset,
                                                  &att_info->w, &att_info->h,
                                                  err)) {
          g_prefix_error(err, "Reading JPEG header for attachment \"%s\": ",
                         name);
          return false;
        }
      } else if (g_str_equal(att.file_type, "CZI")) {
        att_info->file_type = ATT_CZI;
      } else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Attachment \"%s\" has unrecognized type \"%s\"",
                    name, att.file_type);
        return false;
      }
      return true;
    }
  }

  // not found; succeed with att_info->data_offset unchanged
  return true;
}

/* @dst is pre-allocated by openslide, size 4 * w * h */
static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dst, GError **err) {
  struct associated_image *img = (struct associated_image *) _img;

  g_autoptr(_openslide_file) f = _openslide_fopen(img->czi->filename, err);
  if (!f) {
    return false;
  }

  switch (img->file_type) {
  case ATT_CZI:
    ; // make compiler happy
    struct czi_decbuf cbuf;
    if (!read_subblk(f, img->data_offset, img->subblk, &cbuf, err)) {
      return false;
    }

    memcpy(dst, cbuf.data, cbuf.size);
    g_free(cbuf.data);
    return true;
  case ATT_JPG:
    return _openslide_jpeg_read_file(f, img->data_offset, dst,
                                     img->base.w, img->base.h, err);
  default:
    g_assert_not_reached();
  }
}

static void destroy_associated_image(struct _openslide_associated_image *p) {
  struct associated_image *img = (struct associated_image *) p;
  if (img->subblk) {
    g_free(img->subblk);
  }

  g_free(img);
}

static const struct _openslide_associated_image_ops zeiss_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

static void add_one_associated_image(openslide_t *osr, struct czi *czi,
                                     const char *name,
                                     struct czi_att_info *att_info,
                                     struct czi_subblk *sb) {
  struct associated_image *img = g_new0(struct associated_image, 1);

  img->base.ops = &zeiss_associated_ops;
  img->czi = czi;
  img->file_type = att_info->file_type;
  img->data_offset = att_info->data_offset;
  if (sb) {
    img->base.w = sb->tw;
    img->base.h = sb->th;
    img->subblk = g_new0(struct czi_subblk, 1);
    memcpy(img->subblk, sb, sizeof(*sb));
  } else {
    img->subblk = NULL;
    img->base.w = att_info->w;
    img->base.h = att_info->h;
  }

  g_hash_table_insert(osr->associated_images, g_strdup(name), img);
}

static bool zeiss_add_associated_images(openslide_t *osr,
                                        struct czi *outer_czi,
                                        struct _openslide_file *f,
                                        GError **err) {
  const struct associated_image_mapping *map = &known_associated_images[0];

  for (; map->czi_name; map++) {
    // read the outermost CZI to get offset to ZISRAWFILE, or to JPEG
    struct czi_att_info att_info = {0};
    if (!locate_attachment_by_name(outer_czi, &att_info, map->czi_name, err)) {
      return false;
    }
    if (att_info.data_offset == 0) {
      continue;
    }

    g_autoptr(czi) czi = NULL;
    struct czi_subblk *sb = NULL;
    if (att_info.file_type == ATT_CZI) {
      czi = g_new0(struct czi, 1);
      czi->filename = g_strdup(outer_czi->filename);
      czi->zisraw_offset = att_info.data_offset;
      // knowing offset to ZISRAWFILE, now parse the embedded CZI
      if (!load_dir_position(czi, f, err)) {
        return false;
      }
      if (!read_subblk_dir(czi, f, err)) {
        return false;
      }
      // expect the embedded CZI file has only one image subblock
      if (czi->nsubblk != 1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Embedded CZI for associated image \"%s\" has %d subblocks, expected one",
                    map->czi_name, czi->nsubblk);
        return false;
      }
      sb = &czi->subblks[0];
    }

    add_one_associated_image(osr, outer_czi, map->osr_name, &att_info, sb);
  }
  return true;
}

/* find scene boundaries on level 0, and pyramid levels of each scene */
static bool init_scenes(struct czi *czi, GError **err) {
  czi->scenes = g_try_new0(struct czi_scene, czi->nscene);
  if (!czi->scenes) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate memory for %d scenes", czi->nscene);
    return false;
  }
  for (int i = 0; i < czi->nscene; i++) {
    struct czi_scene *s = &czi->scenes[i];
    s->x1 = INT64_MAX;
    s->y1 = INT64_MAX;
    s->x2 = INT64_MIN;
    s->y2 = INT64_MIN;
  }

  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    struct czi_scene *s = &czi->scenes[b->scene];
    if (s->max_downsample < b->downsample_i) {
      s->max_downsample = b->downsample_i;
    }

    // only check scene boundary on top level
    if (b->downsample_i != 1) {
      continue;
    }

    s->x1 = MIN(s->x1, b->x1);
    s->y1 = MIN(s->y1, b->y1);
    s->x2 = MAX(s->x2, b->x1 + b->w);
    s->y2 = MAX(s->y2, b->y1 + b->h);
  }
  return true;
}

static void set_region_props(openslide_t *osr, struct czi *czi) {
  for (int i = 0; i < czi->nscene; i++) {
    struct czi_scene *s = &czi->scenes[i];
    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_X, i),
        g_strdup_printf("%" PRId64, s->x1));
    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_Y, i),
        g_strdup_printf("%" PRId64, s->y1));
    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_WIDTH, i),
        g_strdup_printf("%" PRId64, s->x2 - s->x1));
    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_HEIGHT, i),
        g_strdup_printf("%" PRId64, s->y2 - s->y1));
  }
}

static bool zeiss_open(openslide_t *osr, const char *filename,
                       struct _openslide_tifflike *t G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1, GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  g_autoptr(czi) czi = g_new0(struct czi, 1);
  czi->offset_x = G_MAXINT32;
  czi->offset_y = G_MAXINT32;
  czi->filename = g_strdup(filename);
  if (!load_dir_position(czi, f, err)) {
    return false;
  }
  if (!read_subblk_dir(czi, f, err)) {
    return false;
  }
  adjust_coordinate_origin(czi);

  g_autofree char *xml = read_czi_meta_xml(czi, f, err);
  if (!xml) {
    return false;
  }
  if (!parse_xml_set_prop(osr, czi, xml, err)) {
    return false;
  }

  if (!init_scenes(czi, err)) {
    return false;
  }
  init_levels(osr, czi);
  init_range_grids(osr, czi);
  set_region_props(osr, czi);
  if (!zeiss_add_associated_images(osr, czi, f, err)) {
    return false;
  }

  char buf[CZI_FILEHDR_LEN];
  if (!freadn_to_buf(f, 0, buf, CZI_FILEHDR_LEN, err)) {
    return false;
  }
  _openslide_hash_data(quickhash1, buf, CZI_FILEHDR_LEN);

  // store osr data
  g_assert(osr->data == NULL);
  osr->data = g_steal_pointer(&czi);
  osr->ops = &zeiss_ops;

  return true;
}

const struct _openslide_format _openslide_format_zeiss = {
  .name = "zeiss",
  .vendor = "zeiss",
  .detect = zeiss_detect,
  .open = zeiss_open,
};
