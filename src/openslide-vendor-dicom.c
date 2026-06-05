/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2022-2026 Benjamin Gilbert
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
 * DICOM (.dcm) support
 *
 * quickhash comes from the Series Instance UID
 */

/*
 * Development was supported by NCI Imaging Data Commons
 * <https://imaging.datacommons.cancer.gov/> and has been funded in whole
 * or in part with Federal funds from the National Cancer Institute,
 * National Institutes of Health, under Task Order No. HHSN26110071 under
 * Contract No. HHSN261201500003l.
 */

#include "openslide-private.h"
#include "openslide-decode-dicom.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-jp2k.h"

#include <glib.h>
#include <math.h>

#include <dicom/dicom.h>

enum image_format {
  FORMAT_JPEG,
  FORMAT_JPEG2000,
  FORMAT_JPEG2000_LOSSLESS,
  FORMAT_RGB,
};

struct dicom_file {
  char *filename;

  GMutex lock;
  DcmFilehandle *filehandle;
  struct _openslide_dicom_io *dio;
  uint64_t dio_users;
  const DcmDataSet *file_meta;
  const DcmDataSet *metadata;
  const char *slide_id;
  const char *instance_id;
  const char *concatenation_id;
  enum image_format format;
  J_COLOR_SPACE jpeg_colorspace;
};

// g_auto wrapper struct with reference for runtime I/O
struct dicom_file_io {
  struct dicom_file *file;
};

// g_auto wrapper struct for fio array
struct dicom_file_io_set {
  struct dicom_file_io *fios;
  uint16_t len;
};

struct dicom_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  int64_t tiles_across;
  int64_t tiles_down;

  struct dicom_file **files;
  uint16_t file_count;
  uint16_t *tile_files;
};

struct associated {
  struct _openslide_associated_image base;
  struct dicom_file *file;
};

// map transfer syntax uids to the image formats we support
struct syntax_format {
  const char *syntax;
  enum image_format format;
};

// pyramid level flavor
static const char FLAVOR_VOLUME[] = "VOLUME";
// associated image flavors
static const char FLAVOR_LABEL[] = "LABEL";
static const char FLAVOR_OVERVIEW[] = "OVERVIEW";
static const char FLAVOR_THUMBNAIL[] = "THUMBNAIL";
static const char *const ASSOCIATED_FLAVORS[] = {
  FLAVOR_LABEL,
  FLAVOR_OVERVIEW,
  FLAVOR_THUMBNAIL,
  NULL,
};

/* The DICOM UIDs and fields we check.
 */
static const char BitsAllocated[] = "BitsAllocated";
static const char BitsStored[] = "BitsStored";
static const char Columns[] = "Columns";
static const char ConcatenationUID[] = "ConcatenationUID";
static const char DimensionOrganizationType[] = "DimensionOrganizationType";
static const char HighBit[] = "HighBit";
static const char ICCProfile[] = "ICCProfile";
static const char ImageType[] = "ImageType";
static const char InConcatenationNumber[] = "InConcatenationNumber";
static const char InConcatenationTotalNumber[] = "InConcatenationTotalNumber";
static const char MediaStorageSOPClassUID[] = "MediaStorageSOPClassUID";
static const char OpticalPathSequence[] = "OpticalPathSequence";
static const char PhotometricInterpretation[] = "PhotometricInterpretation";
static const char PixelRepresentation[] = "PixelRepresentation";
static const char PlanarConfiguration[] = "PlanarConfiguration";
static const char Rows[] = "Rows";
static const char SamplesPerPixel[] = "SamplesPerPixel";
static const char SeriesInstanceUID[] = "SeriesInstanceUID";
static const char SOPInstanceUID[] = "SOPInstanceUID";
static const char TotalPixelMatrixColumns[] = "TotalPixelMatrixColumns";
static const char TotalPixelMatrixFocalPlanes[] = "TotalPixelMatrixFocalPlanes";
static const char TotalPixelMatrixRows[] = "TotalPixelMatrixRows";
static const char VLWholeSlideMicroscopyImageStorage[] =
  "1.2.840.10008.5.1.4.1.1.77.1.6";

// the transfer syntaxes we support, and the format we use to decode pixels
static struct syntax_format supported_syntax_formats[] = {
  // simple uncompressed array
  { "1.2.840.10008.1.2.1", FORMAT_RGB },

  // jpeg baseline, we don't handle lossless or 12 bit
  { "1.2.840.10008.1.2.4.50", FORMAT_JPEG },

  // jp2k, forbidding or permitting lossy compression
  { "1.2.840.10008.1.2.4.90", FORMAT_JPEG2000_LOSSLESS },
  { "1.2.840.10008.1.2.4.91", FORMAT_JPEG2000 },
};

OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(g_ptr_array_unref)

static void dicom_file_destroy(struct dicom_file *f) {
  if (!f) {
    return;
  }
  dcm_filehandle_destroy(f->filehandle);
  g_mutex_clear(&f->lock);
  g_free(f->filename);
  g_free(f);
}
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(dicom_file_destroy)

typedef struct dicom_file dicom_file;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(dicom_file, dicom_file_destroy)

// get a dicom_file reference for I/O
static struct dicom_file_io dicom_file_io_get(struct dicom_file *f) {
  g_mutex_lock(&f->lock);
  f->dio_users++;
  g_mutex_unlock(&f->lock);

  struct dicom_file_io fio = {
    .file = f,
  };
  return fio;
}

// put a dicom_file reference, and close the underlying _openslide_file if idle
static void dicom_file_io_put(struct dicom_file_io *fio) {
  g_mutex_lock(&fio->file->lock);
  if (!--fio->file->dio_users) {
    _openslide_dicom_io_suspend(fio->file->dio);
  }
  g_mutex_unlock(&fio->file->lock);
}

typedef struct dicom_file_io dicom_file_io;
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(dicom_file_io, dicom_file_io_put)

static struct dicom_file_io_set dicom_file_io_set_make(uint16_t len) {
  struct dicom_file_io_set fioset = {
    .fios = g_new0(struct dicom_file_io, len),
    .len = len,
  };
  return fioset;
}

static void dicom_file_io_set_put(struct dicom_file_io_set *fioset) {
  for (uint16_t i = 0; i < fioset->len; i++) {
    if (fioset->fios[i].file) {
      dicom_file_io_put(&fioset->fios[i]);
    }
  }
  g_free(fioset->fios);
}

typedef struct dicom_file_io_set dicom_file_io_set;
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(dicom_file_io_set, dicom_file_io_set_put)

static bool get_tag_int(const DcmDataSet *dataset,
                        const char *keyword,
                        int64_t *result) {
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  return element &&
         dcm_element_get_value_integer(NULL, element, 0, result);
}

static bool get_tag_str(const DcmDataSet *dataset,
                        const char *keyword,
                        int index,
                        const char **result) {
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  return element &&
         dcm_element_get_value_string(NULL, element, index, result);
}

static bool get_tag_binary(const DcmDataSet *dataset,
                           const char *keyword,
                           const void **result,
                           int64_t *length) {
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  if (!element) {
    return false;
  }
  if (!dcm_element_get_value_binary(NULL, element, result)) {
    return false;
  }
  if (length) {
    *length = dcm_element_get_length(element);
  }
  return true;
}

static bool get_tag_seq(const DcmDataSet *dataset,
                        const char *keyword,
                        DcmSequence **result) {
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  return element &&
         dcm_element_get_value_sequence(NULL, element, result);
}

static bool get_tag_seq_item(const DcmDataSet *dataset,
                             const char *keyword,
                             uint32_t index,
                             DcmDataSet **result) {
  DcmSequence *seq;
  if (!get_tag_seq(dataset, keyword, &seq)) {
    return false;
  }
  *result = dcm_sequence_get(NULL, seq, index);
  return *result != NULL;
}

static bool verify_tag_int(const DcmDataSet *dataset,
                           const char *keyword,
                           int64_t expected_value,
                           bool required,
                           GError **err) {
  int64_t value;
  if (!get_tag_int(dataset, keyword, &value)) {
    if (!required) {
      return true;
    }
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read %s", keyword);
    return false;
  }
  if (value != expected_value) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Attribute %s value %"PRId64" != %"PRId64,
                keyword, value, expected_value);
    return false;
  }
  return true;
}

// Do the initial DICOM detection and return a half-initialized dicom_file.
// Only do the minimum checks necessary to reject files that are not valid
// DICOM WSI files.  Allow skipping metadata load for vendor detection.
// The rest of the initialization will happen in maybe_add_file().
static struct dicom_file *dicom_file_new(const char *filename,
                                         bool load_metadata, GError **err) {
  g_autoptr(dicom_file) f = g_new0(struct dicom_file, 1);
  g_mutex_init(&f->lock);

  f->filehandle = _openslide_dicom_open(filename, &f->dio, err);
  if (!f->filehandle) {
    return NULL;
  }
  f->filename = g_strdup(filename);

  DcmError *dcm_error = NULL;
  f->file_meta = dcm_filehandle_get_file_meta(&dcm_error, f->filehandle);
  if (!f->file_meta) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return NULL;
  }

  const char *sop;
  if (!get_tag_str(f->file_meta, MediaStorageSOPClassUID, 0, &sop) ||
      !g_str_equal(sop, VLWholeSlideMicroscopyImageStorage)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a WSI DICOM: class UID %s", sop);
    return NULL;
  }

  if (load_metadata) {
    f->metadata = dcm_filehandle_get_metadata_subset(&dcm_error, f->filehandle);
    if (!f->metadata) {
      _openslide_dicom_propagate_error(err, dcm_error);
      return NULL;
    }

    if (!get_tag_str(f->metadata, SeriesInstanceUID, 0, &f->slide_id)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "SeriesInstanceUID not found");
      return NULL;
    }
  }

  return g_steal_pointer(&f);
}

static void level_destroy(struct dicom_level *l) {
  _openslide_grid_destroy(l->grid);
  for (uint16_t i = 0; i < l->file_count; i++) {
    dicom_file_destroy(l->files[i]);
  }
  g_free(l->files);
  g_free(l->tile_files);
  g_free(l);
}
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(level_destroy)

typedef struct dicom_level dicom_level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(dicom_level, level_destroy)

static void destroy(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; i++) {
    level_destroy((struct dicom_level *) osr->levels[i]);
  }
  g_free(osr->levels);
}

static void rgb_to_cairo(const uint8_t *rgb, uint32_t *dest,
                         int64_t width, int64_t height) {
  int64_t n_pixels = width * height;
  for (int64_t i = 0; i < n_pixels; i++) {
    dest[i] = 0xff000000 | rgb[0] << 16 | rgb[1] << 8 | rgb[2];
    rgb += 3;
  }
}

static bool decode_frame(struct dicom_file *file,
                         int64_t tile_col, int64_t tile_row,
                         uint32_t *dest, int64_t w, int64_t h,
                         GError **err) {
  g_mutex_lock(&file->lock);
  DcmError *dcm_error = NULL;
  g_autoptr(DcmFrame) frame =
      dcm_filehandle_read_frame_position(&dcm_error,
                                         file->filehandle,
                                         tile_col, tile_row);
  g_mutex_unlock(&file->lock);

  if (!frame) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return false;
  }

  const void *frame_value = dcm_frame_get_value(frame);
  uint32_t frame_length = dcm_frame_get_length(frame);
  uint32_t frame_width = dcm_frame_get_columns(frame);
  uint32_t frame_height = dcm_frame_get_rows(frame);
  if (frame_width != w || frame_height != h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected image size: %ux%u != %"PRId64"x%"PRId64,
                frame_width, frame_height, w, h);
    return false;
  }

  switch (file->format) {
  case FORMAT_JPEG:
    return _openslide_jpeg_decode_buffer_colorspace(frame_value, frame_length,
                                                    file->jpeg_colorspace,
                                                    dest, w, h, err);
  case FORMAT_JPEG2000:
  case FORMAT_JPEG2000_LOSSLESS:
    // ICT and RCT are processed by OpenJPEG and return RGB
    return _openslide_jp2k_decode_buffer(dest, w, h,
                                         frame_value, frame_length,
                                         OPENSLIDE_JP2K_RGB, err);
  case FORMAT_RGB:
    if (frame_length != w * h * 3) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "RGB frame length %u != %"PRIu64, frame_length, w * h * 3);
      return false;
    }
    rgb_to_cairo(frame_value, dest, w, h);
  }
  return true;
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct dicom_level *l = (struct dicom_level *) level;
  struct dicom_file_io *fios = arg;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    // get file
    uint16_t file_num =
      l->tile_files ? l->tile_files[tile_col + tile_row * l->tiles_across] : 0;
    g_assert(file_num < l->file_count);
    if (!fios[file_num].file) {
      fios[file_num] = dicom_file_io_get(l->files[file_num]);
    }

    g_autofree uint32_t *buf = g_new(uint32_t, l->base.tile_w * l->base.tile_h);
    if (!decode_frame(fios[file_num].file, tile_col, tile_row,
                      buf, l->base.tile_w, l->base.tile_h, err)) {
      return false;
    }

    // clip, if necessary
    if (!_openslide_clip_tile(buf,
                              l->base.tile_w, l->base.tile_h,
                              l->base.w - tile_col * l->base.tile_w,
                              l->base.h - tile_row * l->base.tile_h,
                              err)) {
      return false;
    }

    // put it in the cache
    tiledata = g_steal_pointer(&buf);
    _openslide_cache_put(osr->cache,
                         level, tile_col, tile_row,
                         tiledata, l->base.tile_w * l->base.tile_h * 4,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        l->base.tile_w, l->base.tile_h,
                                        l->base.tile_w * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return true;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED,
                         cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct dicom_level *l = (struct dicom_level *) level;

  g_auto(dicom_file_io_set) fioset = dicom_file_io_set_make(l->file_count);
  return _openslide_grid_paint_region(l->grid, cr, fioset.fios,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h,
                                      err);
}

static const void *get_icc_profile(struct dicom_file *file, int64_t *len) {
  const DcmDataSet *metadata = file->metadata;

  DcmDataSet *optical_path;
  const void *icc_profile;
  if (!get_tag_seq_item(metadata, OpticalPathSequence, 0, &optical_path) ||
      !get_tag_binary(optical_path, ICCProfile, &icc_profile, len)) {
    return NULL;
  }

  return icc_profile;
}

static bool read_icc_profile(openslide_t *osr, void *dest,
                             GError **err) {
  struct dicom_level *l = (struct dicom_level *) osr->levels[0];
  int64_t icc_profile_size;
  const void *icc_profile = get_icc_profile(l->files[0], &icc_profile_size);
  if (!icc_profile) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No ICC profile");
    return false;
  }
  if (icc_profile_size != osr->icc_profile_size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "ICC profile size changed");
    return false;
  }

  memcpy(dest, icc_profile, icc_profile_size);

  return true;
}

static const struct _openslide_ops dicom_ops = {
  .paint_region = paint_region,
  .read_icc_profile = read_icc_profile,
  .destroy = destroy,
};

static bool dicom_detect(const char *filename, struct _openslide_tifflike *tl,
                         GError **err) {
  if (tl && (g_str_has_suffix(filename, ".tif") ||
             g_str_has_suffix(filename, ".tiff"))) {
    // let the generic-tiff driver handle it
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Dual-personality DICOM-TIFF with TIFF filename extension");
    return false;
  }
  // otherwise, ignore any TIFF metadata

  g_autoptr(dicom_file) f = dicom_file_new(filename, false, err);
  return f != NULL;
}

static bool associated_get_argb_data(struct _openslide_associated_image *img,
                                     uint32_t *dest,
                                     GError **err) {
  struct associated *a = (struct associated *) img;
  g_auto(dicom_file_io) fio G_GNUC_UNUSED = dicom_file_io_get(a->file);
  return decode_frame(a->file, 0, 0, dest, a->base.w, a->base.h, err);
}

static bool associated_read_icc_profile(struct _openslide_associated_image *img,
                                        void *dest, GError **err) {
  struct associated *a = (struct associated *) img;
  int64_t icc_profile_size;
  const void *icc_profile = get_icc_profile(a->file, &icc_profile_size);
  if (!icc_profile) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No ICC profile");
    return false;
  }
  if (icc_profile_size != img->icc_profile_size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "ICC profile size changed");
    return false;
  }

  memcpy(dest, icc_profile, icc_profile_size);

  return true;
}

static void _associated_destroy(struct associated *a) {
  dicom_file_destroy(a->file);
  g_free(a);
}

typedef struct associated associated;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(associated, _associated_destroy)

static void associated_destroy(struct _openslide_associated_image *img) {
  struct associated *a = (struct associated *) img;
  _associated_destroy(a);
}

static const struct _openslide_associated_image_ops dicom_associated_ops = {
  .get_argb_data = associated_get_argb_data,
  .read_icc_profile = associated_read_icc_profile,
  .destroy = associated_destroy,
};

static void log_duplicate_file(struct dicom_file *cur,
                               struct dicom_file *prev) {
  if (_openslide_debug(OPENSLIDE_DEBUG_SEARCH)) {
    g_message("opening %s: SOP instance UID %s matches %s",
              cur->filename, cur->instance_id, prev->filename);
  }
}

// unconditionally takes ownership of dicom_file
static bool add_associated(openslide_t *osr,
                           struct dicom_file *f,
                           const char *image_flavor,
                           GError **err) {
  g_autoptr(associated) a = g_new0(struct associated, 1);
  a->base.ops = &dicom_associated_ops;
  a->file = f;

  // do checks
  if (f->concatenation_id) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Found associated image as a concatenation member");
    return false;
  }

  // dimensions
  if (!get_tag_int(f->metadata, TotalPixelMatrixColumns, &a->base.w) ||
      !get_tag_int(f->metadata, TotalPixelMatrixRows, &a->base.h)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read associated image dimensions");
    return false;
  }

  // size of ICC profile, if present
  (void) get_icc_profile(f, &a->base.icc_profile_size);

  // associated image name
  char *name;
  if (g_str_equal(image_flavor, FLAVOR_LABEL)) {
    name = "label";
  } else if (g_str_equal(image_flavor, FLAVOR_OVERVIEW)) {
    name = "macro";
  } else if (g_str_equal(image_flavor, FLAVOR_THUMBNAIL)) {
    name = "thumbnail";
  } else {
    // ASSOCIATED_FLAVORS contains something unexpected
    g_assert_not_reached();
  }

  // if we've seen this associated image type before and the SOP instance
  // UIDs match, someone duplicated a file; ignore it.  otherwise there's
  // something we don't understand about this slide and we must fail
  struct associated *previous =
    g_hash_table_lookup(osr->associated_images, name);
  if (previous) {
    if (g_str_equal(f->instance_id, previous->file->instance_id)) {
      log_duplicate_file(f, previous->file);
      return true;
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Slide contains unexpected associated image (%s vs. %s)",
                  f->instance_id, previous->file->instance_id);
      return false;
    }
  }

  // done with I/O
  _openslide_dicom_io_suspend(f->dio);

  // add
  g_hash_table_insert(osr->associated_images,
                      g_strdup(name),
                      g_steal_pointer(&a));
  return true;
}

static bool find_level_by_dimensions(GPtrArray *level_array,
                                     GPtrArray *level_files_array,
                                     int64_t w, int64_t h,
                                     struct dicom_level **level,
                                     GPtrArray **level_files) {
  for (guint i = 0; i < level_array->len; i++) {
    struct dicom_level *l = level_array->pdata[i];
    if (l->base.w == w && l->base.h == h) {
      *level = l;
      *level_files = level_files_array->pdata[i];
      return true;
    }
  }
  return false;
}

// unconditionally takes ownership of dicom_file
static bool add_level_file(openslide_t *osr,
                           GPtrArray *level_array,
                           GPtrArray *level_files_array,
                           struct dicom_file *file,
                           GError **err) {
  g_autoptr(dicom_file) f = file;

  int64_t level_width;
  int64_t level_height;
  int64_t tile_width;
  int64_t tile_height;
  if (!get_tag_int(f->metadata, TotalPixelMatrixColumns, &level_width) ||
      !get_tag_int(f->metadata, TotalPixelMatrixRows, &level_height) ||
      !get_tag_int(f->metadata, Columns, &tile_width) ||
      !get_tag_int(f->metadata, Rows, &tile_height)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read level dimensions");
    return false;
  }

  // get index within concatenation, if concatenation
  int64_t file_num = 1;
  get_tag_int(f->metadata, InConcatenationNumber, &file_num);
  if (file_num <= 0 || file_num > UINT16_MAX) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "In-concatenation number out of range: %"PRId64, file_num);
    return false;
  }
  // zero-index
  file_num--;

  struct dicom_level *l;
  GPtrArray *files;
  if (find_level_by_dimensions(level_array, level_files_array,
                               level_width, level_height,
                               &l, &files)) {
    g_assert(files->len > 0);

    // check for duplicate SOP Instance UIDs
    for (uint16_t file_num = 0; file_num < files->len; file_num++) {
      struct dicom_file *prev = files->pdata[file_num];
      if (prev && g_str_equal(f->instance_id, prev->instance_id)) {
        // someone duplicated a file; ignore it
        log_duplicate_file(f, prev);
        return true;
      }
    }

    // must be in the same concatenation
    for (uint16_t file_num = 0; file_num < files->len; file_num++) {
      struct dicom_file *prev = files->pdata[file_num];
      if (prev) {
        if (!f->concatenation_id && !prev->concatenation_id) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "Slide contains unexpected image (%s vs. %s)",
                      f->instance_id, prev->instance_id);
          return false;
        }
        if (!f->concatenation_id ||
            !prev->concatenation_id ||
            !g_str_equal(f->concatenation_id, prev->concatenation_id)) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "Concatenation UID %s must match UID %s in existing level file %s",
                      f->concatenation_id ?: "<none>",
                      prev->concatenation_id ?: "<none>",
                      prev->filename);
          return false;
        }
        break;
      }
    }

    // must have the same tile size
    if (l->base.tile_w != tile_width || l->base.tile_h != tile_height) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Tile size %"PRId64"x%"PRId64" doesn't match "
                  "level tile size %"PRId64"x%"PRId64,
                  tile_width, tile_height, l->base.tile_w, l->base.tile_h);
      return false;
    }
  } else {
    // we must make a new level
    l = g_new0(struct dicom_level, 1);
    l->base.w = level_width;
    l->base.h = level_height;
    l->base.tile_w = tile_width;
    l->base.tile_h = tile_height;
    l->tiles_across = (l->base.w / l->base.tile_w) +
                      !!(l->base.w % l->base.tile_w);
    l->tiles_down = (l->base.h / l->base.tile_h) +
                    !!(l->base.h % l->base.tile_h);
    l->grid = _openslide_grid_create_simple(osr,
                                            l->tiles_across, l->tiles_down,
                                            l->base.tile_w, l->base.tile_h,
                                            read_tile);

    files = g_ptr_array_new_full(4,
                                 OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(dicom_file_destroy));

    g_ptr_array_add(level_array, l);
    g_ptr_array_add(level_files_array, files);
  }

  // put it in the correct spot in the array.  having a defined ordering
  // ensures we always read properties from the same SOP Instance and allows
  // us to cross-check for missing files when InConcatenationTotalNumber is
  // omitted
  if (files->len < file_num + 1) {
    g_ptr_array_set_size(files, file_num + 1);
  }
  if (files->pdata[file_num]) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Found multiple SOP instances in concatenation with index %"PRId64,
                file_num + 1);
    return false;
  }
  files->pdata[file_num] = g_steal_pointer(&f);
  return true;
}

// unconditionally takes ownership of dicom_file
static bool maybe_add_file(openslide_t *osr,
                           GPtrArray *level_array,
                           GPtrArray *level_files_array,
                           struct dicom_file *file,
                           GError **err) {
  g_autoptr(dicom_file) f = file;
  g_assert(f->metadata);

  // check image flavor; ignore the rest of ImageType
  // see discussion in https://github.com/openslide/openslide/issues/642
  const char *image_flavor;
  if (!get_tag_str(f->metadata, ImageType, 2, &image_flavor)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't get image flavor");
    return false;
  }
  bool is_level = g_str_equal(image_flavor, FLAVOR_VOLUME);
  bool is_associated = g_strv_contains(ASSOCIATED_FLAVORS, image_flavor);
  if (!is_level && !is_associated) {
    // unknown flavor; ignore
    if (_openslide_debug(OPENSLIDE_DEBUG_SEARCH)) {
      g_message("opening %s: unknown image flavor %s",
                file->filename, image_flavor);
    }
    return true;
  }

  // check transfer syntax
  const char *syntax = dcm_filehandle_get_transfer_syntax_uid(f->filehandle);
  bool found = false;
  for (uint64_t i = 0; i < G_N_ELEMENTS(supported_syntax_formats); i++) {
    if (g_str_equal(syntax, supported_syntax_formats[i].syntax)) {
      f->format = supported_syntax_formats[i].format;
      found = true;
      break;
    }
  }
  if (!found) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported transfer syntax %s", syntax);
    return false;
  }

  // check the other image format tags
  if (!verify_tag_int(f->metadata, PlanarConfiguration, 0, true, err) ||
      !verify_tag_int(f->metadata, BitsAllocated, 8, true, err) ||
      !verify_tag_int(f->metadata, BitsStored, 8, true, err) ||
      !verify_tag_int(f->metadata, HighBit, 7, true, err) ||
      !verify_tag_int(f->metadata, SamplesPerPixel, 3, true, err) ||
      !verify_tag_int(f->metadata, PixelRepresentation, 0, true, err) ||
      !verify_tag_int(f->metadata, TotalPixelMatrixFocalPlanes, 1, false, err)) {
    return false;
  }

  // check color space
  const char *photometric;
  if (!get_tag_str(f->metadata, PhotometricInterpretation, 0, &photometric)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't get PhotometricInterpretation");
    return false;
  }
  found = false;
  switch (f->format) {
  case FORMAT_JPEG2000:
    found =
      g_str_equal(photometric, "YBR_ICT") ||
      g_str_equal(photometric, "YBR_RCT") ||
      g_str_equal(photometric, "RGB");
    break;
  case FORMAT_JPEG2000_LOSSLESS:
    found =
      g_str_equal(photometric, "YBR_RCT") ||
      g_str_equal(photometric, "RGB");
    break;
  case FORMAT_JPEG:
    if (g_str_equal(photometric, "YBR_FULL_422")) {
      f->jpeg_colorspace = JCS_YCbCr;
      found = true;
    } else if (g_str_equal(photometric, "RGB")) {
      f->jpeg_colorspace = JCS_RGB;
      found = true;
    }
    break;
  case FORMAT_RGB:
    found = g_str_equal(photometric, "RGB");
    break;
  }
  if (!found) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported photometric interpretation %s for %s",
                photometric, syntax);
    return false;
  }

  // populate IDs
  if (!get_tag_str(f->metadata, SOPInstanceUID, 0, &f->instance_id)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "SOPInstanceUID not found");
    return false;
  }
  // optional
  get_tag_str(f->metadata, ConcatenationUID, 0, &f->concatenation_id);

  // add
  if (is_level) {
    return add_level_file(osr, level_array, level_files_array,
                          g_steal_pointer(&f), err);
  } else {
    return add_associated(osr, g_steal_pointer(&f), image_flavor, err);
  }
}

struct property_iterate {
  openslide_t *osr;
  const char *prefix;
  bool first;
};

static bool add_properties_element(const DcmElement *element,
                                   void *client);

static bool add_properties_dataset(const DcmDataSet *dataset,
                                   uint32_t index,
                                   void *client) {
  struct property_iterate *iter = (struct property_iterate *) client;
  g_autofree char *new_prefix = iter->first ?
      g_strdup(iter->prefix) :
      g_strdup_printf("%s[%u]", iter->prefix, index);
  struct property_iterate new_iter = { iter->osr, new_prefix, false };
  return dcm_dataset_foreach(dataset, add_properties_element, &new_iter);
}

static char *get_element_value_as_string(const DcmElement *element, int index) {
  DcmVR vr = dcm_element_get_vr(element);
  DcmVRClass klass = dcm_dict_vr_class(vr);

  const char *str;
  double d;
  int64_t i64;

  switch (klass) {
  case DCM_VR_CLASS_STRING_MULTI:
  case DCM_VR_CLASS_STRING_SINGLE:
    if (dcm_element_get_value_string(NULL, element, index, &str)) {
      return g_strdup(str);
    }
    break;

  case DCM_VR_CLASS_NUMERIC_DECIMAL:
    if (dcm_element_get_value_decimal(NULL, element, index, &d)) {
      return _openslide_format_double(d);
    }
    break;

  case DCM_VR_CLASS_NUMERIC_INTEGER:
    if (dcm_element_get_value_integer(NULL, element, index, &i64)) {
      if (vr == DCM_VR_UV) {
        return g_strdup_printf("%"PRIu64, i64);
      } else {
        return g_strdup_printf("%"PRId64, i64);
      }
    }
    break;

  case DCM_VR_CLASS_BINARY:
  default:
    break;
  }

  return NULL;
}

static bool add_properties_element(const DcmElement *element,
                                   void *client) {
  struct property_iterate *iter = (struct property_iterate *) client;
  DcmVR vr = dcm_element_get_vr(element);
  uint32_t tag = dcm_element_get_tag(element);
  const char *keyword = dcm_dict_keyword_from_tag(tag);
  DcmVRClass klass = dcm_dict_vr_class(vr);

  // ignore unknown tags
  if (!keyword) {
    return true;
  }

  if (klass == DCM_VR_CLASS_SEQUENCE) {
    DcmSequence *seq;
    if (dcm_element_get_value_sequence(NULL, element, &seq)) {
      g_autofree char *new_prefix = g_strdup_printf("%s.%s",
                                                    iter->prefix,
                                                    keyword);
      struct property_iterate new_iter = { iter->osr, new_prefix, false };
      dcm_sequence_foreach(seq, add_properties_dataset, &new_iter);
    }
  } else {
    uint32_t vm = dcm_element_get_vm(element);

    if (vm == 1) {
      char *value = get_element_value_as_string(element, 0);
      if (value) {
        g_hash_table_insert(iter->osr->properties,
                            g_strdup_printf("%s.%s", iter->prefix, keyword),
                            value);
      }
    } else {
      for (uint32_t index = 0; index < vm; index++) {
        char *value = get_element_value_as_string(element, index);
        if (value) {
          g_hash_table_insert(iter->osr->properties,
                              g_strdup_printf("%s.%s[%u]",
                                              iter->prefix,
                                              keyword,
                                              index),
                              value);
        }
      }
    }
  }

  return true;
}

static void add_properties(openslide_t *osr, struct dicom_level *level0) {
  // add all dicom elements
  struct property_iterate iter = { osr, "dicom", true };
  add_properties_dataset(level0->files[0]->file_meta, 0, &iter);
  add_properties_dataset(level0->files[0]->metadata, 0, &iter);

  // add MPP and objective power
  // pixel spacing is in mm, so convert to microns
  // row spacing, then column spacing
  _openslide_duplicate_double_prop_scaled(osr,
                                          "dicom.SharedFunctionalGroupsSequence[0].PixelMeasuresSequence[0].PixelSpacing[0]",
                                          1000.0,
                                          OPENSLIDE_PROPERTY_NAME_MPP_Y);
  _openslide_duplicate_double_prop_scaled(osr,
                                          "dicom.SharedFunctionalGroupsSequence[0].PixelMeasuresSequence[0].PixelSpacing[1]",
                                          1000.0,
                                          OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr,
                                   "dicom.OpticalPathSequence[0].ObjectiveLensPower",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
}

static gint compare_level_width(const void *a, const void *b) {
  const struct dicom_level *aa = *((const struct dicom_level **) a);
  const struct dicom_level *bb = *((const struct dicom_level **) b);

  return bb->base.w - aa->base.w;
}

static bool finalize_level(struct dicom_level *l, GPtrArray *files,
                           GError **err) {
  // check for each file
  for (uint16_t file_num = 0; file_num < files->len; file_num++) {
    if (!files->pdata[file_num]) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Missing SOP instance %u/%u in concatenation",
                  file_num + 1, files->len);
      return false;
    }
  }

  // convert to file array
  g_assert(files->len <= UINT16_MAX);
  l->file_count = files->len;
  // g_ptr_array_steal() on glib 2.64+
  l->files = g_new(struct dicom_file *, l->file_count);
  for (uint16_t file_num = 0; file_num < l->file_count; file_num++) {
    l->files[file_num] = g_steal_pointer(&files->pdata[file_num]);
  }

  // check file count
  g_assert(l->file_count > 0);
  if (l->file_count > 1) {
    int64_t total_instances;
    // optional
    if (get_tag_int(l->files[0]->metadata, InConcatenationTotalNumber,
                    &total_instances) &&
        total_instances != l->file_count) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Expected %"PRId64" SOP instances in concatenation %s, found %d",
                  total_instances, l->files[0]->concatenation_id,
                  l->file_count);
      return false;
    }
  } else if (l->files[0]->concatenation_id) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Found only one SOP instance in concatenation %s",
                l->files[0]->concatenation_id);
    return false;
  }

  // sparse dimension organization?
  const char *organization;
  bool sparse =
    // optional, defaults to TILED_SPARSE
    !get_tag_str(l->files[0]->metadata, DimensionOrganizationType, 0,
                 &organization) ||
    !g_str_equal(organization, "TILED_FULL");

  // for concatenations, find the first file containing each tile
  // for concatenations and sparse levels, inform grid of missing tiles
  if (sparse || l->file_count > 1) {
    if (l->file_count > 1) {
      g_assert(!l->tile_files);
      l->tile_files = g_new(uint16_t, l->tiles_across * l->tiles_down);
    }
    for (int64_t tile_row = 0; tile_row < l->tiles_down; tile_row++) {
      for (int64_t tile_col = 0; tile_col < l->tiles_across; tile_col++) {
        int64_t tile_index = tile_col + tile_row * l->tiles_across;
        uint16_t file_num;
        for (file_num = 0; file_num < l->file_count; file_num++) {
          DcmError *dcm_error = NULL;
          uint32_t n;
          if (dcm_filehandle_get_frame_number(&dcm_error,
                                              l->files[file_num]->filehandle,
                                              tile_col, tile_row, &n)) {
            // found one, record the file that has this tile and break
            if (l->tile_files) {
              l->tile_files[tile_index] = file_num;
            }
            break;
          } else if (dcm_error_get_code(dcm_error) == DCM_ERROR_CODE_MISSING_FRAME) {
            dcm_error_clear(&dcm_error);
          } else {
            _openslide_dicom_propagate_error(err, dcm_error);
            return false;
          }
        }
        if (file_num == l->file_count) {
          // none
          _openslide_grid_simple_set_missing(l->grid, tile_col, tile_row);
          if (l->tile_files) {
            l->tile_files[tile_index] = UINT16_MAX;
          }
        }
      }
    }
  }

  // done with I/O
  for (uint16_t file_num = 0; file_num < l->file_count; file_num++) {
    _openslide_dicom_io_suspend(l->files[file_num]->dio);
  }
  return true;
}

static bool dicom_open(openslide_t *osr,
                       const char *filename,
                       struct _openslide_tifflike *tl G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1,
                       GError **err) {
  g_autofree char *dirname = g_path_get_dirname(filename);
  g_autofree char *basename = g_path_get_basename(filename);

  g_autoptr(_openslide_dir) dir = _openslide_dir_open(dirname, err);
  if (!dir) {
    return false;
  }

  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_full(10,
                         OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(level_destroy));
  g_autoptr(GPtrArray) level_files_array =
    g_ptr_array_new_full(10,
                         OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(g_ptr_array_unref));

  // open the passed-in file and get the slide-id
  g_autoptr(dicom_file) start = dicom_file_new(filename, true, err);
  if (!start) {
    return false;
  }
  g_autofree char *slide_id = g_strdup(start->slide_id);

  if (!maybe_add_file(osr, level_array, level_files_array,
                      g_steal_pointer(&start), err)) {
    g_prefix_error(err, "Reading %s: ", filename);
    return false;
  }

  // scan for other DICOMs with this slide id
  const char *name;
  GError *dir_err = NULL;
  while ((name = _openslide_dir_next(dir, &dir_err))) {
    // no need to add the start file again
    if (g_str_equal(name, basename)) {
      continue;
    }

    g_autofree char *path = g_build_filename(dirname, name, NULL);

    GError *tmp_err = NULL;
    g_autoptr(dicom_file) f = dicom_file_new(path, true, &tmp_err);
    if (!f) {
      if (_openslide_debug(OPENSLIDE_DEBUG_SEARCH)) {
        g_message("opening %s: %s", path, tmp_err->message);
      }
      g_error_free(tmp_err);
      continue;
    }

    if (!g_str_equal(f->slide_id, slide_id)) {
      if (_openslide_debug(OPENSLIDE_DEBUG_SEARCH)) {
        g_message("opening %s: Series Instance UID %s != %s",
                  path, f->slide_id, slide_id);
      }
      continue;
    }

    if (!maybe_add_file(osr, level_array, level_files_array,
                        g_steal_pointer(&f), err)) {
      g_prefix_error(err, "Reading %s: ", path);
      return false;
    }
  }
  if (dir_err) {
    g_propagate_error(err, dir_err);
    return false;
  }

  if (level_array->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No pyramid levels found");
    return false;
  }

  // finalize levels
  for (uint32_t i = 0; i < level_array->len; i++) {
    if (!finalize_level(level_array->pdata[i], level_files_array->pdata[i],
                        err)) {
      return false;
    }
  }

  // sort levels by width
  g_ptr_array_sort(level_array, compare_level_width);

  // add properties
  struct dicom_level *level0 = level_array->pdata[0];
  add_properties(osr, level0);

  (void) get_icc_profile(level0->files[0], &osr->icc_profile_size);

  // compute quickhash
  _openslide_hash_string(quickhash1, slide_id);

  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->ops = &dicom_ops;
  return true;
}

const struct _openslide_format _openslide_format_dicom = {
  .name = "dicom",
  .vendor = "dicom",
  .detect = dicom_detect,
  .open = dicom_open,
};
