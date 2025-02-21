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

/*
 * Zeiss CZI support
 *
 * quickhash comes from file header UUIDs and the metadata XML
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

#define CZI_GUID_LEN 16
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
} __attribute__((__packed__));

// beginning of a CZI file, SID = ZISRAWFILE
struct zisraw_data_file_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t major;
  int32_t minor;
  int32_t _reserved1;
  int32_t _reserved2;
  uint8_t primary_file_guid[CZI_GUID_LEN];
  uint8_t file_guid[CZI_GUID_LEN];
  int32_t file_part; // this causes off-align
  int64_t subblk_dir_pos;
  int64_t meta_pos;
  int32_t update_pending;
  int64_t att_dir_pos;
} __attribute__((__packed__));

// SubBlockDirectorySegment, SID = ZISRAWDIRECTORY
struct zisraw_subblk_dir_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t entry_count;
  char _reserved[124];
  // followed by DirectoryEntryDV list
} __attribute__((__packed__));

// Metadata segment, SID = ZISRAWMETADATA
struct zisraw_meta_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t xml_size;
  int32_t _attach_size;
  char _reserved[248];
} __attribute__((__packed__));

// SubBlock segment, SID = ZISRAWSUBBLOCK
struct zisraw_subblk_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t meta_size;
  int32_t attach_size;
  int64_t data_size;
  // followed by DirectoryEntryDV of this subblock, followed by padding
  // to 288 bytes, followed by meta (and attach?) and data
} __attribute__((__packed__));

// Directory Entry - Schema DV
struct zisraw_dir_entry_dv {
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
} __attribute__((__packed__));

// DimensionEntryDV1
struct zisraw_dim_entry_dv {
  char dimension[4];
  int32_t start;
  int32_t size;
  float start_coordinate;
  int32_t stored_size;
} __attribute__((__packed__));

// AttachmentEntry - Schema A1
struct zisraw_att_entry_a1 {
  char schema[2];
  char _reserved2[10];
  int64_t file_pos;
  int32_t _file_part;
  uint8_t guid[CZI_GUID_LEN];
  char file_type[8]; // ZIP, ZISRAW, JPG etc.
  char name[80];     // Thumbnail, Label, SlidePreview etc.
} __attribute__((__packed__));

// Attachment Segment, SID = ZISRAWATTACH
struct zisraw_seg_att_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t data_size;
  char _reserved1[12];
  struct zisraw_att_entry_a1 att_entry;
  char _reserved2[112];
  // followed by data
} __attribute__((__packed__));

// AttachmentDirectory Segment, SID = ZISRAWATTDIR
struct zisraw_att_dir_hdr {
  struct zisraw_seg_hdr seg_hdr;
  int32_t entry_count;
  char _reserved[252];
  // followed by AttachementEntryA1 list
} __attribute__((__packed__));

struct zisraw_zstd_payload_hdr {
  uint8_t size;           /* either 1 or 3 */
  uint8_t chunk_type;     /* the only valid type is 1 */
  uint8_t is_hi_low_pack; /* whether high low byte packing is used */
} __attribute__((__packed__));

enum zisraw_compression {
  COMP_NONE = 0,
  COMP_JPEG,
  COMP_LZW,
  COMP_JXR = 4,
  COMP_ZSTD0,
  COMP_ZSTD1,
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

static const struct czi_compression_name {
  enum zisraw_compression compression;
  const char *name;
} czi_compression_names[] = {
  {COMP_NONE, "uncompressed"},
  {COMP_JPEG, "JPEG"},
  {COMP_LZW, "LZW"},
  {3, "type 3"},
  {COMP_JXR, "JPEG XR"},
  {COMP_ZSTD0, "zstd v0"},
  {COMP_ZSTD1, "zstd v1"},
  {COMP_OTHER, "unknown"},
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

static const struct associated_image_mapping {
  const char *czi_name;
  const char *osr_name;
} known_associated_images[] = {
  {"Label", "label"},
  {"SlidePreview", "macro"},
  {"Thumbnail", "thumbnail"},
};

static const char * const metadata_property_xpaths[] = {
  "/ImageDocument/Metadata/AttachmentInfos",
  "/ImageDocument/Metadata/DisplaySetting",
  "/ImageDocument/Metadata/Information",
  "/ImageDocument/Metadata/Scaling",
};

struct czi_subblk {
  int64_t file_pos;
  int64_t downsample_i;
  int32_t pixel_type;
  int32_t compression;
  // higher z-index overlaps a lower z-index
  int32_t x, y, z;
  uint32_t w, h;
  int8_t scene;
};

struct czi {
  uint8_t primary_file_guid[CZI_GUID_LEN];
  uint8_t file_guid[CZI_GUID_LEN];
  // offset to ZISRAWFILE, one for each file, usually 0. CZI file is like
  // Russian doll, it can embed other CZI files. Non-zero value is the
  // offset to embedded CZI file
  int64_t zisraw_offset;
  int64_t subblk_dir_pos;
  int64_t meta_pos;
  int64_t att_dir_pos;
  int32_t w;
  int32_t h;
  int32_t nscene;
  int32_t nsubblk; // total number of subblocks
  struct czi_subblk *subblks;
};

struct associated_image {
  struct _openslide_associated_image base;
  char *filename;
  int64_t data_offset;
  struct czi_subblk *subblk;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
  int64_t downsample_i;
  uint32_t max_tile_w;
  uint32_t max_tile_h;
};

struct zeiss_ops_data {
  struct czi *czi;
  char *filename;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}

static void destroy_czi(struct czi *czi) {
  g_free(czi->subblks);
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
    struct zeiss_ops_data *data = osr->data;
    destroy_czi(data->czi);
    g_free(data->filename);
    g_free(data);
  }
}

static bool freadn_to_buf(struct _openslide_file *f, off_t offset,
                          void *buf, size_t len, GError **err) {
  if (!_openslide_fseek(f, offset, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to offset %"PRId64": ", offset);
    return false;
  }
  if (!_openslide_fread_exact(f, buf, len, err)) {
    g_prefix_error(err, "At offset %"PRId64": ", (int64_t) offset);
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

static void bgr24_to_argb32(uint8_t *src, size_t src_len, uint32_t *dst) {
  // one 24-bit pixel at a time
  for (size_t i = 0; i < src_len; i += 3, src += 3) {
    *dst++ = (0xFF000000 |
              (uint32_t)(src[0]) |
              ((uint32_t)(src[1]) << 8) |
              ((uint32_t)(src[2]) << 16));
  }
}

static void bgr48_to_argb32(uint8_t *src, size_t src_len, uint32_t *dst) {
  // one 48-bit pixel at a time
  for (size_t i = 0; i < src_len; i += 6, src += 6) {
    *dst++ = (0xFF000000 |
              (uint32_t)(src[1]) |
              ((uint32_t)(src[3]) << 8) |
              ((uint32_t)(src[5]) << 16));
  }
}

static bool czi_read_raw(struct _openslide_file *f, int64_t pos, int64_t len,
                         int32_t compression, int32_t pixel_type,
                         uint32_t *dst, int32_t w, int32_t h,
                         GError **err) {
  // figure out what to do with pixel type
  void (*convert)(uint8_t *, size_t, uint32_t *);
  int bytes_per_pixel;
  switch (pixel_type) {
  case PT_BGR24:
    convert = bgr24_to_argb32;
    bytes_per_pixel = 3;
    break;
  case PT_BGR48:
    convert = bgr48_to_argb32;
    bytes_per_pixel = 6;
    break;
  default:
    g_assert_not_reached();
  }
  const int64_t pixel_bytes = w * h * bytes_per_pixel;

  // read from file
  g_autofree uint8_t *file_data = g_try_malloc(len);
  if (!file_data) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate %"PRId64" bytes for image data", len);
    return false;
  }
  if (!freadn_to_buf(f, pos, file_data, len, err)) {
    g_prefix_error(err, "Couldn't read image data: ");
    return false;
  }

  // process compression
  uint8_t *src = file_data;
  g_autofree uint8_t *decompressed_data = NULL;
  bool do_hilo = false;
  switch (compression) {
  case COMP_NONE:
    // file_data will be directly converted to ARGB, without any decompression
    // step, so make sure the input and output sizes match
    if (pixel_bytes != len) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read %"PRId64" pixels into a %"PRId32"x%"PRId32
                  " image", len / bytes_per_pixel, w, h);
      return false;
    }
    break;
  case COMP_ZSTD1:
    // parse zstd1 header
    ;  // make compiler happy
    const struct zisraw_zstd_payload_hdr *hdr =
      (const struct zisraw_zstd_payload_hdr *) src;
    // check that we can read the size field, then use it to check that we
    // can read the whole header
    if (len < (int64_t) sizeof(hdr->size) || len < hdr->size) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Image data length %"PRId64" too small for zstd header", len);
      return false;
    }
    switch (hdr->size) {
    case 1:
      break;
    case 3:
      if (hdr->chunk_type != 1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unexpected zstd chunk type: %d", hdr->chunk_type);
        return false;
      }
      do_hilo = hdr->is_hi_low_pack & 1;
      break;
    default:
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unexpected zstd header length: %u", hdr->size);
      return false;
    }
    src += hdr->size;
    len -= hdr->size;
    // fall through
  case COMP_ZSTD0:
    // decompress
    decompressed_data =
      _openslide_zstd_decompress_buffer(src, len, pixel_bytes, err);
    if (!decompressed_data) {
      g_prefix_error(err, "Decompressing pixel data: ");
      return false;
    }
    src = decompressed_data;
    break;
  default:
    g_assert_not_reached();
  }

  // zstd1 compression has an option to pack less significant bytes of 16-
  // bit pixels in the first half of the image array, and more significant
  // bytes in the second half of the image array.  Undo this.
  g_autofree uint8_t *unhilo_data = NULL;
  if (do_hilo) {
    if (pixel_bytes % 2) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't perform HiLo unpacking with an odd number of bytes "
                  "%"PRId64, pixel_bytes);
      return false;
    }
    int64_t half_bytes = pixel_bytes / 2;
    unhilo_data = g_try_malloc(pixel_bytes);
    if (!unhilo_data) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't allocate %"PRId64" bytes for HiLo unpacking",
                  pixel_bytes);
      return false;
    }
    uint8_t *p = unhilo_data;
    uint8_t *slo = src;
    uint8_t *shi = src + half_bytes;
    for (int64_t i = 0; i < half_bytes; i++) {
      *p++ = *slo++;
      *p++ = *shi++;
    }
    src = unhilo_data;
  }

  // convert pixels to ARGB
  convert(src, pixel_bytes, dst);
  return true;
}

// dst must be sb->w * sb->h * 4 bytes
static bool read_subblk(struct _openslide_file *f, int64_t zisraw_offset,
                        struct czi_subblk *sb, uint32_t *dst, GError **err) {
  struct zisraw_subblk_hdr hdr;
  if (!freadn_to_buf(f, zisraw_offset + sb->file_pos,
                     &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read subblock header: ");
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
  case COMP_ZSTD0:
  case COMP_ZSTD1:
    return czi_read_raw(f, data_pos, data_size, sb->compression, sb->pixel_type,
                        dst, sb->w, sb->h, err);
  default:
    g_assert_not_reached();
  }
  return true;
}

static bool read_tile(openslide_t *osr, cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tid, void *tile_data,
                      void *arg, GError **err) {
  struct zeiss_ops_data *data = osr->data;
  struct czi *czi = data->czi;
  struct _openslide_file *f = arg;
  struct czi_subblk *sb = tile_data;

  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache, level, tid, 0,
                                            &cache_entry);
  if (!tiledata) {
    g_autofree uint32_t *buf = g_malloc(sb->w * sb->h * 4);
    if (!read_subblk(f, czi->zisraw_offset, sb, buf, err)) {
      return false;
    }
    tiledata = g_steal_pointer(&buf);
    _openslide_cache_put(osr->cache, level, tid, 0, tiledata,
                         sb->w * sb->h * 4, &cache_entry);
  }

  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        sb->w, sb->h, sb->w * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct zeiss_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
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

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dst, GError **err) {
  struct associated_image *img = (struct associated_image *) _img;

  g_autoptr(_openslide_file) f = _openslide_fopen(img->filename, err);
  if (!f) {
    return false;
  }

  if (img->subblk) {
    return read_subblk(f, img->data_offset, img->subblk, dst, err);
  } else {
    return _openslide_jpeg_read_file(f, img->data_offset, dst,
                                     img->base.w, img->base.h, err);
  }
}

static void _destroy_associated_image(struct associated_image *img) {
  g_free(img->subblk);
  g_free(img->filename);
  g_free(img);
}
typedef struct associated_image associated_image;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(associated_image, _destroy_associated_image)

static void destroy_associated_image(struct _openslide_associated_image *p) {
  _destroy_associated_image((struct associated_image *) p);
}

static const struct _openslide_associated_image_ops zeiss_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

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

  g_autofree char *name = g_strndup(dim->dimension, sizeof(dim->dimension));
  int start = GINT32_FROM_LE(dim->start);
  int size = GINT32_FROM_LE(dim->size);
  int stored_size = GINT32_FROM_LE(dim->stored_size);

  if (g_str_equal(name, "X")) {
    sb->x = start;
    sb->w = stored_size;
    sb->downsample_i = DIV_ROUND_CLOSEST(size, stored_size);
  } else if (g_str_equal(name, "Y")) {
    sb->y = start;
    sb->h = stored_size;
  } else if (g_str_equal(name, "S")) {
    sb->scene = start;
  } else if (g_str_equal(name, "C")) {
    // channel
    if (start) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Nonzero subblock channel %d", start);
      return false;
    }
  } else if (g_str_equal(name, "M")) {
    // mosaic tile index in drawing stack; highest number is frontmost
    sb->z = start;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unrecognized subblock dimension \"%s\"", name);
    return false;
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
  if (!sb->w || !sb->h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Missing X or Y dimension in directory entry");
    return false;
  }
  return true;
}

/* read all data subblocks info (x, y, w, h etc.) from subblock directory */
static bool read_subblk_dir(struct czi *czi, struct _openslide_file *f,
                            GError **err) {
  int64_t offset = czi->zisraw_offset + czi->subblk_dir_pos;
  struct zisraw_subblk_dir_hdr hdr;
  if (!freadn_to_buf(f, offset, &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read subblock directory header: ");
    return false;
  }
  offset += sizeof(hdr);

  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWDIRECTORY, err)) {
    return false;
  }

  czi->nsubblk = GINT32_FROM_LE(hdr.entry_count);
  int64_t seg_size =
    GINT64_FROM_LE(hdr.seg_hdr.used_size) - sizeof(hdr) + sizeof(hdr.seg_hdr);
  g_autofree char *buf_dir = g_try_malloc(seg_size);
  if (!buf_dir) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate %"PRId64" bytes for subblock directory",
                seg_size);
    return false;
  }
  if (!freadn_to_buf(f, offset, buf_dir, seg_size, err)) {
    g_prefix_error(err, "Couldn't read subblock directory: ");
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
                "Found %"PRIu64" trailing bytes after subblock directory",
                (uint64_t) avail);
    return false;
  }
  return true;
}

// adjust tile coordinates relative to the top-left tile (which may have
// negative X)
static void adjust_coordinate_origin(struct czi *czi) {
  int32_t offset_x = G_MAXINT32;
  int32_t offset_y = G_MAXINT32;

  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    offset_x = MIN(offset_x, b->x);
    offset_y = MIN(offset_y, b->y);
  }

  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    b->x -= offset_x;
    b->y -= offset_y;
  }
}

static struct czi *create_czi(struct _openslide_file *f, int64_t offset,
                              GError **err) {
  struct zisraw_data_file_hdr hdr;
  if (!freadn_to_buf(f, offset, &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read file header: ");
    return NULL;
  }
  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWFILE, err)) {
    return NULL;
  }

  g_autoptr(czi) czi = g_new0(struct czi, 1);
  memcpy(czi->primary_file_guid, hdr.primary_file_guid, CZI_GUID_LEN);
  memcpy(czi->file_guid, hdr.file_guid, CZI_GUID_LEN);
  czi->zisraw_offset = offset;
  czi->subblk_dir_pos = GINT64_FROM_LE(hdr.subblk_dir_pos);
  czi->meta_pos = GINT64_FROM_LE(hdr.meta_pos);
  czi->att_dir_pos = GINT64_FROM_LE(hdr.att_dir_pos);

  if (!read_subblk_dir(czi, f, err)) {
    return NULL;
  }
  adjust_coordinate_origin(czi);
  return g_steal_pointer(&czi);
}

static char *read_czi_meta_xml(struct czi *czi,
                               struct _openslide_file *f, GError **err) {
  int64_t offset = czi->zisraw_offset + czi->meta_pos;
  struct zisraw_meta_hdr hdr;
  if (!freadn_to_buf(f, offset, &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Couldn't read metadata header: ");
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
    g_prefix_error(err, "Couldn't read metadata XML: ");
    return NULL;
  }

  xml[xml_size] = 0;
  return g_steal_pointer(&xml);
}

static char *get_xml_element_name(GPtrArray *path, xmlNode *node) {
  const char *name = (const char *) node->name;

  g_autofree char *name_plural = g_strdup_printf("%ss", name);
  char *parent_name = path->pdata[path->len - 1];
  if (g_str_equal(parent_name, name_plural) ||
      (g_str_equal(parent_name, "Items") && g_str_equal(name, "Distance"))) {
    // element array member: a path of the form "[...].Scenes.Scene", or as
    // a special case, "[...].Items.Distance"
    // find an XML attribute to use as a name
    g_autoptr(xmlChar) id = xmlGetProp(node, BAD_CAST "Id");
    if (!id) {
      id = xmlGetProp(node, BAD_CAST "Name");
    }
    if (!id) {
      // can't find unique identifier; skip the element
      //g_debug("Couldn't find identifier for list item with parent %s", parent_name);
      return NULL;
    }
    return g_strdup((char *) id);
  }

  // normal element
  return g_strdup(name);
}

static void add_xml_props(openslide_t *osr, xmlDoc *doc, GPtrArray *path,
                          xmlNode *node) {
  // If we don't know a good name for a property, skip it rather than use a
  // bad name.  We can always add properties, but existing ones are a
  // compatibility constraint.

  if (xmlNodeIsText(node)) {
    // text node (which has no children); add property if non-blank
    if (!xmlIsBlankNode(node)) {
      // glib >= 2.74 has g_ptr_array_new_null_terminated()
      g_ptr_array_add(path, NULL);
      char *name = g_strjoinv(".", (char **) path->pdata);
      g_ptr_array_remove_index(path, path->len - 1);

      g_hash_table_insert(osr->properties, name,
                          g_strdup((char *) node->content));
    }
    return;
  }

  // add path component
  char *name = get_xml_element_name(path, node);
  g_assert(name);
  g_ptr_array_add(path, name);

  // add properties for attributes
  for (xmlAttr *attr = node->properties; attr; attr = attr->next) {
    g_ptr_array_add(path, g_strdup((char *) attr->name));
    add_xml_props(osr, doc, path, attr->children);
    g_ptr_array_remove_index(path, path->len - 1);
  }

  // build a set of children to skip: those where multiple children have the
  // same name (otherwise their properties would clobber each other)
  g_autoptr(GHashTable) child_names =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) child_skip =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (xmlNode *child = node->children; child; child = child->next) {
    if (xmlNodeIsText(child)) {
      continue;
    }
    char *child_name = get_xml_element_name(path, child);
    if (child_name && !g_hash_table_add(child_names, child_name)) {
      g_hash_table_add(child_skip, g_strdup(child_name));
    }
  }

  // descend children
  for (xmlNode *child = node->children; child; child = child->next) {
    if (!xmlNodeIsText(child)) {
      g_autofree char *child_name = get_xml_element_name(path, child);
      if (!child_name) {
        // list element that we don't know how to uniquely rename
        continue;
      }
      if (g_hash_table_lookup(child_skip, child_name)) {
        // duplicate name
        //g_debug("Skipping duplicate element: %s", child_name);
        continue;
      }
    }
    add_xml_props(osr, doc, path, child);
  }

  // drop path component
  g_ptr_array_remove_index(path, path->len - 1);
}

// parse XML, set CZI parameters and OpenSlide properties
static bool parse_xml_set_prop(openslide_t *osr, struct czi *czi,
                               const char *xml, GError **err) {
  g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
  if (doc == NULL) {
    g_prefix_error(err, "Couldn't parse metadata XML: ");
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
                    ObjectiveSettings
                        <ObjectiveRef Id="Objective:1"/>
                Instrument
                    Microscopes
                        <Microscope Id="Microscope:1" Name="Axioscan 7">
                    Objectives
                        <Objective Id="Objective:1">
                            NominalMagnification  (objective-power)
              Scaling
                  Items
                      <Distance Id="X">  (mpp X)
                          Value  (3.4443237544526617E-07, in meter)
                      <Distance Id="Y">  (mpp Y)
                          Value
  */
  g_autoptr(xmlXPathContext) ctx = _openslide_xml_xpath_create(doc);

  g_autoptr(GPtrArray) path = g_ptr_array_new_full(16, g_free);
  g_ptr_array_add(path, g_strdup("zeiss"));
  for (unsigned i = 0; i < G_N_ELEMENTS(metadata_property_xpaths); i++) {
    xmlNode *node =
      _openslide_xml_xpath_get_node(ctx, metadata_property_xpaths[i]);
    if (node) {
      add_xml_props(osr, doc, path, node);
    }
  }

  const char *size_x =
    g_hash_table_lookup(osr->properties, "zeiss.Information.Image.SizeX");
  const char *size_y =
    g_hash_table_lookup(osr->properties, "zeiss.Information.Image.SizeY");
  const char *size_s =
    g_hash_table_lookup(osr->properties, "zeiss.Information.Image.SizeS");
  if (!size_x || !size_y) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read image dimensions");
    return false;
  }
  int64_t w, h, nscene;
  if (!_openslide_parse_int64(size_x, &w) ||
      !_openslide_parse_int64(size_y, &h)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't parse image dimensions");
    return false;
  }
  // some CZI files may not have SizeS, e.g. extracted SlidePreview
  if (!size_s) {
    nscene = 1;
  } else if (!_openslide_parse_int64(size_s, &nscene)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't parse image scene dimension");
    return false;
  }
  czi->w = w;
  czi->h = h;
  czi->nscene = nscene;

  // in meter/pixel
  const char *meters =
    g_hash_table_lookup(osr->properties, "zeiss.Scaling.Items.X.Value");
  if (meters) {
    double d = _openslide_parse_double(meters);
    if (!isnan(d)) {
      // in um/pixel
      g_hash_table_insert(osr->properties,
                          g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                          _openslide_format_double(d * 1000000.0));
    }
  }

  meters = g_hash_table_lookup(osr->properties, "zeiss.Scaling.Items.Y.Value");
  if (meters) {
    double d = _openslide_parse_double(meters);
    if (!isnan(d)) {
      g_hash_table_insert(osr->properties,
                          g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                          _openslide_format_double(d * 1000000.0));
    }
  }

  char *objective_id = g_hash_table_lookup(osr->properties,
                                           "zeiss.Information.Image.ObjectiveSettings.ObjectiveRef.Id");
  if (objective_id) {
    g_autofree char *objective_key =
      g_strdup_printf("zeiss.Information.Instrument.Objectives.%s.NominalMagnification",
                      objective_id);
    _openslide_duplicate_double_prop(osr, objective_key,
                                     OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  }

  return true;
}

// add region bounds props for each scene; compute common max downsample
static bool read_scenes_set_prop(openslide_t *osr, struct czi *czi,
                                 int64_t *max_downsample_OUT,
                                 GError **err) {
  // init out-argument
  *max_downsample_OUT = INT64_MAX;

  // allocate scene arrays
  g_autofree int64_t *x1 = g_try_new(int64_t, czi->nscene);
  g_autofree int64_t *y1 = g_try_new(int64_t, czi->nscene);
  g_autofree int64_t *x2 = g_try_new(int64_t, czi->nscene);
  g_autofree int64_t *y2 = g_try_new(int64_t, czi->nscene);
  g_autofree int64_t *max_downsample = g_try_new0(int64_t, czi->nscene);
  if (!x1 || !y1 || !x2 || !y2 || !max_downsample) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate memory for %d scenes", czi->nscene);
    return false;
  }
  for (int i = 0; i < czi->nscene; i++) {
    x1[i] = INT64_MAX;
    y1[i] = INT64_MAX;
    x2[i] = INT64_MIN;
    y2[i] = INT64_MIN;
  }

  // walk subblocks, build up scene info
  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    if (b->scene < 0 || b->scene >= czi->nscene) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Subblock %d specifies out-of-range scene %d", i, b->scene);
      return false;
    }
    max_downsample[b->scene] = MAX(max_downsample[b->scene], b->downsample_i);

    // only check scene boundary on bottom level
    if (b->downsample_i == 1) {
      x1[b->scene] = MIN(x1[b->scene], b->x);
      y1[b->scene] = MIN(y1[b->scene], b->y);
      x2[b->scene] = MAX(x2[b->scene], b->x + b->w);
      y2[b->scene] = MAX(y2[b->scene], b->y + b->h);
    }
  }

  // walk scenes, add properties and compute common downsample
  for (int i = 0; i < czi->nscene; i++) {
    if (!max_downsample[i]) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "No subblocks for scene %d", i);
      return false;
    }

    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_X, i),
        g_strdup_printf("%" PRId64, x1[i]));
    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_Y, i),
        g_strdup_printf("%" PRId64, y1[i]));
    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_WIDTH, i),
        g_strdup_printf("%" PRId64, x2[i] - x1[i]));
    g_hash_table_insert(
        osr->properties,
        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_HEIGHT, i),
        g_strdup_printf("%" PRId64, y2[i] - y1[i]));

    // Scenes on a slide may have different pyramid depths.  For example,
    // rat kidney is likely to have more levels than a mouse kidney on the
    // same slide.  Find the maximum downsample value available on all
    // scenes and use it to set the total levels.  This ensures we show all
    // sections on a slide at max zoom out.
    *max_downsample_OUT = MIN(*max_downsample_OUT, max_downsample[i]);
  }
  return true;
}

static bool validate_subblk(const struct czi_subblk *sb, GError **err) {
  switch (sb->pixel_type) {
  case PT_BGR24:
  case PT_BGR48:
    break;
  default:
    if (sb->pixel_type >= 0 &&
        sb->pixel_type < (int) G_N_ELEMENTS(czi_pixel_type_names)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Pixel type %s is not supported",
                  czi_pixel_type_names[sb->pixel_type].name);
      return false;
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Pixel type %d is not supported", sb->pixel_type);
      return false;
    }
  }

  switch (sb->compression) {
  case COMP_NONE:
  case COMP_ZSTD0:
  case COMP_ZSTD1:
    break;
  default:
    if (sb->compression >= 0 &&
        sb->compression < (int) G_N_ELEMENTS(czi_compression_names)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "%s compression is not supported",
                  czi_compression_names[sb->compression].name);
      return false;
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Compression %d is not supported", sb->compression);
      return false;
    }
  }
  return true;
}

static int compare_level_downsamples(const void *a, const void *b) {
  const struct level *la = *(const struct level **) a;
  const struct level *lb = *(const struct level **) b;

  if (la->downsample_i == lb->downsample_i) {
    return 0;
  }
  return (la->downsample_i < lb->downsample_i) ? -1 : 1;
}

static GPtrArray *create_levels(openslide_t *osr, struct czi *czi,
                                int64_t max_downsample,
                                uint64_t *default_cache_size_OUT,
                                GError **err) {
  // walk subblocks, create a level struct for each valid downsample
  g_autoptr(GPtrArray) levels =
    g_ptr_array_new_full(10, (GDestroyNotify) destroy_level);
  g_autoptr(GHashTable) level_hash =
    g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    if (b->downsample_i > max_downsample) {
      continue;
    }

    struct level *l = g_hash_table_lookup(level_hash, &b->downsample_i);
    if (!l) {
      l = g_new0(struct level, 1);
      l->base.downsample = b->downsample_i;
      l->base.w = czi->w / l->base.downsample;
      l->base.h = czi->h / l->base.downsample;
      l->downsample_i = b->downsample_i;

      g_ptr_array_add(levels, l);
      int64_t *k = g_new(int64_t, 1);
      *k = l->downsample_i;
      g_hash_table_insert(level_hash, k, l);
    }

    l->max_tile_w = MAX(l->max_tile_w, b->w);
    l->max_tile_h = MAX(l->max_tile_h, b->h);
  }
  if (!levels->len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Found no levels in slide");
    return NULL;
  }
  g_ptr_array_sort(levels, compare_level_downsamples);

  // now that we know bucket sizes, create grids.  also collect max tile size
  uint32_t max_tile_w = 0;
  uint32_t max_tile_h = 0;
  for (unsigned i = 0; i < levels->len; i++) {
    struct level *l = levels->pdata[i];
    // assume the largest tile dimensions are the routine ones, and smaller
    // tiles are at boundaries
    l->grid = _openslide_grid_create_range(osr,
                                           l->max_tile_w, l->max_tile_h,
                                           read_tile, NULL);
    max_tile_w = MAX(max_tile_w, l->max_tile_w);
    max_tile_h = MAX(max_tile_h, l->max_tile_h);
  }

  // select cache size large enough to hold at least two tiles
  *default_cache_size_OUT =
    MAX(DEFAULT_CACHE_SIZE, 2 * 4 * max_tile_w * max_tile_h);
  //g_debug("Default cache size: %"PRIu64, *default_cache_size_OUT);

  // add subblocks to grids
  for (int i = 0; i < czi->nsubblk; i++) {
    struct czi_subblk *b = &czi->subblks[i];
    if (b->downsample_i > max_downsample) {
      // subblock from a level that was omitted because not all scenes have it
      continue;
    }
    if (!validate_subblk(b, err)) {
      return NULL;
    }

    struct level *l = g_hash_table_lookup(level_hash, &b->downsample_i);
    g_assert(l);
    _openslide_grid_range_add_tile(l->grid,
                                   (double) b->x / b->downsample_i,
                                   (double) b->y / b->downsample_i,
                                   b->z, b->w, b->h, b);
  }

  // postprocess grids
  for (unsigned i = 0; i < levels->len; i++) {
    struct level *l = levels->pdata[i];
    _openslide_grid_range_finish_adding_tiles(l->grid);
  }
  return g_steal_pointer(&levels);
}

static bool add_one_associated_image(openslide_t *osr, const char *filename,
                                     struct _openslide_file *f,
                                     const char *name, const char *file_type,
                                     int64_t data_offset, GError **err) {
  g_autoptr(associated_image) img = g_new0(struct associated_image, 1);
  img->base.ops = &zeiss_associated_ops;
  img->filename = g_strdup(filename);
  img->data_offset = data_offset;

  if (g_str_equal(file_type, "JPG")) {
    int32_t w, h;
    if (!_openslide_jpeg_read_file_dimensions(f, data_offset, &w, &h, err)) {
      g_prefix_error(err, "Reading JPEG header for associated image \"%s\": ",
                     name);
      return false;
    }
    img->base.w = w;
    img->base.h = h;
  } else if (g_str_equal(file_type, "CZI")) {
    g_autoptr(czi) czi = create_czi(f, data_offset, err);
    if (!czi) {
      g_prefix_error(err, "Reading CZI for associated image \"%s\": ", name);
      return false;
    }
    if (czi->nsubblk != 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Embedded CZI for associated image \"%s\" has %d subblocks, expected one",
                  name, czi->nsubblk);
      return false;
    }
    struct czi_subblk *sb = &czi->subblks[0];
    if (!validate_subblk(sb, err)) {
      g_prefix_error(err, "Adding associated image \"%s\": ", name);
      return false;
    }
    img->base.w = sb->w;
    img->base.h = sb->h;
    img->subblk = g_memdup(sb, sizeof(*sb));
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Associated image \"%s\" has unrecognized type \"%s\"",
                name, file_type);
    return false;
  }
  g_hash_table_insert(osr->associated_images, g_strdup(name),
                      g_steal_pointer(&img));
  return true;
}

static const char *get_associated_image_name_for_attachment(const char *name) {
  for (int i = 0; i < (int) G_N_ELEMENTS(known_associated_images); i++) {
    if (g_str_equal(name, known_associated_images[i].czi_name)) {
      return known_associated_images[i].osr_name;
    }
  }
  return NULL;
}

static bool add_associated_images(openslide_t *osr, struct czi *czi,
                                  const char *filename,
                                  struct _openslide_file *f,
                                  GError **err) {
  // read attachment directory header
  struct zisraw_att_dir_hdr hdr;
  int64_t offset = czi->zisraw_offset + czi->att_dir_pos;
  if (!freadn_to_buf(f, offset, &hdr, sizeof(hdr), err)) {
    g_prefix_error(err, "Reading attachment directory header: ");
    return false;
  }
  if (!check_magic(hdr.seg_hdr.sid, SID_ZISRAWATTDIR, err)) {
    return false;
  }
  offset += sizeof(hdr);

  // walk directory
  int nattch = GINT32_FROM_LE(hdr.entry_count);
  for (int i = 0; i < nattch; i++) {
    // read entry
    struct zisraw_att_entry_a1 att;
    if (!freadn_to_buf(f, offset, &att, sizeof(att), err)) {
      g_prefix_error(err, "Reading attachment directory entry: ");
      return false;
    }
    if (!check_magic(att.schema, SCHEMA_A1, err)) {
      return false;
    }
    offset += sizeof(att);

    g_autofree char *file_type =
      g_strndup(att.file_type, sizeof(att.file_type));
    g_autofree char *name = g_strndup(att.name, sizeof(att.name));

    // if it's a known associated image, add it
    const char *osr_name = get_associated_image_name_for_attachment(name);
    if (osr_name &&
        !add_one_associated_image(osr, filename, f, osr_name, file_type,
                                  GINT64_FROM_LE(att.file_pos) +
                                  sizeof(struct zisraw_seg_att_hdr),
                                  err)) {
      return false;
    }
  }
  return true;
}

static bool zeiss_open(openslide_t *osr, const char *filename,
                       struct _openslide_tifflike *tl G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1, GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  g_autoptr(czi) czi = create_czi(f, 0, err);
  if (!czi) {
    return false;
  }

  g_autofree char *xml = read_czi_meta_xml(czi, f, err);
  if (!xml) {
    return false;
  }
  if (!parse_xml_set_prop(osr, czi, xml, err)) {
    return false;
  }

  int64_t max_downsample;
  if (!read_scenes_set_prop(osr, czi, &max_downsample, err)) {
    return false;
  }

  uint64_t default_cache_size;
  g_autoptr(GPtrArray) levels =
    create_levels(osr, czi, max_downsample, &default_cache_size, err);
  if (!levels) {
    return false;
  }

  if (!add_associated_images(osr, czi, filename, f, err)) {
    return false;
  }

  // compute quickhash
  _openslide_hash_data(quickhash1, czi->primary_file_guid, CZI_GUID_LEN);
  _openslide_hash_data(quickhash1, czi->file_guid, CZI_GUID_LEN);
  _openslide_hash_string(quickhash1, xml);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  g_assert(osr->cache == NULL);
  osr->level_count = levels->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&levels), false);
  struct zeiss_ops_data *data = g_new0(struct zeiss_ops_data, 1);
  data->czi = g_steal_pointer(&czi);
  data->filename = g_strdup(filename);
  osr->data = data;
  osr->ops = &zeiss_ops;
  osr->cache = _openslide_cache_binding_create(default_cache_size);

  return true;
}

const struct _openslide_format _openslide_format_zeiss = {
  .name = "zeiss-czi",
  .vendor = "zeiss",
  .detect = zeiss_detect,
  .open = zeiss_open,
};
