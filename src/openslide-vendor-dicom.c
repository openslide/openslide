/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
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
#include "openslide-hash.h"

#include <glib.h>
#include <math.h>

#include <dicom/dicom.h>

enum image_format {
  FORMAT_JPEG,
  FORMAT_JPEG2000,
  FORMAT_RGB,
};

struct dicom_file {
  char *filename;

  GMutex lock;
  DcmFilehandle *filehandle;
  const DcmDataSet *file_meta;
  const DcmDataSet *metadata;
  const char *slide_id;
  enum image_format format;
  enum _openslide_jp2k_colorspace jp2k_colorspace;
};

struct dicom_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  double pixel_spacing_x;
  double pixel_spacing_y;
  double objective_lens_power;

  struct dicom_file *file;
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

// a set of allowed image types for a class of image
struct allowed_types {
  const char *const *const *types;
  int n_types;
};

// the ImageTypes we allow for pyr levels
static const char *const ORIGINAL_TYPES[] = {
  "ORIGINAL", "PRIMARY", "VOLUME", "NONE", NULL
};
// if the image has been re-encoded during conversion to DICOM
static const char *const DERIVED_ORIGINAL_TYPES[] = {
  "DERIVED", "PRIMARY", "VOLUME", "NONE", NULL
};
static const char *const RESAMPLED_TYPES[] = {
  "DERIVED", "PRIMARY", "VOLUME", "RESAMPLED", NULL
};
static const char *const *const LEVEL_TYPE_STRINGS[] = {
  ORIGINAL_TYPES,
  DERIVED_ORIGINAL_TYPES,
  RESAMPLED_TYPES,
};

static const struct allowed_types LEVEL_TYPES = {
  LEVEL_TYPE_STRINGS,
  G_N_ELEMENTS(LEVEL_TYPE_STRINGS)
};

// the ImageTypes we allow for associated images
static const char LABEL_TYPE[] = "LABEL";
static const char OVERVIEW_TYPE[] = "OVERVIEW";
static const char THUMBNAIL_TYPE[] = "THUMBNAIL";
static const char *const LABEL_TYPES[] = {
  "ORIGINAL", "PRIMARY", LABEL_TYPE, "NONE", NULL
};
static const char *const DERIVED_LABEL_TYPES[] = {
  "DERIVED", "PRIMARY", LABEL_TYPE, "NONE", NULL
};
static const char *const OVERVIEW_TYPES[] = {
  "ORIGINAL", "PRIMARY", OVERVIEW_TYPE, "NONE", NULL
};
static const char *const DERIVED_OVERVIEW_TYPES[] = {
  "DERIVED", "PRIMARY", OVERVIEW_TYPE, "NONE", NULL
};
static const char *const THUMBNAIL_TYPES[] = {
  "ORIGINAL", "PRIMARY", THUMBNAIL_TYPE, "RESAMPLED", NULL
};
static const char *const DERIVED_THUMBNAIL_TYPES[] = {
  "DERIVED", "PRIMARY", THUMBNAIL_TYPE, "RESAMPLED", NULL
};
static const char *const *const ASSOCIATED_TYPE_STRINGS[] = {
  LABEL_TYPES,
  DERIVED_LABEL_TYPES,
  OVERVIEW_TYPES,
  DERIVED_OVERVIEW_TYPES,
  THUMBNAIL_TYPES,
  DERIVED_THUMBNAIL_TYPES,
};
static const struct allowed_types ASSOCIATED_TYPES = {
  ASSOCIATED_TYPE_STRINGS,
  G_N_ELEMENTS(ASSOCIATED_TYPE_STRINGS)
};

/* The DICOM UIDs and fields we check.
 */
static const char BitsAllocated[] = "BitsAllocated";
static const char BitsStored[] = "BitsStored";
static const char Columns[] = "Columns";
static const char HighBit[] = "HighBit";
static const char ICCProfile[] = "ICCProfile";
static const char ImageType[] = "ImageType";
static const char MediaStorageSOPClassUID[] = "MediaStorageSOPClassUID";
static const char ObjectiveLensPower[] = "ObjectiveLensPower";
static const char OpticalPathSequence[] = "OpticalPathSequence";
static const char PhotometricInterpretation[] = "PhotometricInterpretation";
static const char PixelMeasuresSequence[] = "PixelMeasuresSequence";
static const char PixelRepresentation[] = "PixelRepresentation";
static const char PixelSpacing[] = "PixelSpacing";
static const char PlanarConfiguration[] = "PlanarConfiguration";
static const char Rows[] = "Rows";
static const char SamplesPerPixel[] = "SamplesPerPixel";
static const char SeriesInstanceUID[] = "SeriesInstanceUID";
static const char SharedFunctionalGroupsSequence[] =
  "SharedFunctionalGroupsSequence";
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

  // lossless and lossy jp2k
  // we separate RGB and YCbCr with other tags
  { "1.2.840.10008.1.2.4.90", FORMAT_JPEG2000 },
  { "1.2.840.10008.1.2.4.91", FORMAT_JPEG2000 },
};

static void dicom_file_destroy(struct dicom_file *f) {
  dcm_filehandle_destroy(f->filehandle);
  g_mutex_clear(&f->lock);
  g_free(f->filename);
  g_free(f);
}

typedef struct dicom_file dicom_file;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(dicom_file, dicom_file_destroy)

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

static bool get_tag_decimal_str(const DcmDataSet *dataset,
                                const char *keyword,
                                int index,
                                double *result) {
  const char *value_ptr;
  if (!get_tag_str(dataset, keyword, index, &value_ptr)) {
    return false;
  }
  const double value = _openslide_parse_double(value_ptr);
  if (isnan(value)) {
    return false;
  }
  *result = value;
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

static char **get_tag_strv(const DcmDataSet *dataset,
                           const char *keyword,
                           int length) {
  g_auto(GStrv) a = g_new0(char *, length + 1);
  uint32_t tag = dcm_dict_tag_from_keyword(keyword);
  DcmElement *element = dcm_dataset_get(NULL, dataset, tag);
  if (!element) {
    return NULL;
  }
  for (int i = 0; i < length; i++) {
    const char *item;
    if (!dcm_element_get_value_string(NULL, element, i, &item)) {
      return NULL;
    }
    a[i] = g_strdup(item);
  }
  return g_steal_pointer(&a);
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

  f->filehandle = _openslide_dicom_open(filename, err);
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
  if (l->file) {
    dicom_file_destroy(l->file);
  }
  g_free(l);
}

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
    if (dcm_error_get_code(dcm_error) == DCM_ERROR_CODE_MISSING_FRAME) {
      dcm_error_clear(&dcm_error);
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE,
                  "No frame for (%"PRId64", %"PRId64")", tile_col, tile_row);
    } else {
      _openslide_dicom_propagate_error(err, dcm_error);
    }
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
    return _openslide_jpeg_decode_buffer(frame_value, frame_length,
                                         dest, w, h, err);
  case FORMAT_JPEG2000:
    return _openslide_jp2k_decode_buffer(dest, w, h,
                                         frame_value, frame_length,
                                         file->jp2k_colorspace,
                                         err);
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
                      void *arg G_GNUC_UNUSED,
                      GError **err) {
  struct dicom_level *l = (struct dicom_level *) level;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    g_autofree uint32_t *buf = g_malloc(l->base.tile_w * l->base.tile_h * 4);
    GError *tmp_err = NULL;
    if (!decode_frame(l->file, tile_col, tile_row,
                      buf, l->base.tile_w, l->base.tile_h,
                      &tmp_err)) {
      if (g_error_matches(tmp_err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE)) {
        // missing tile
        g_clear_error(&tmp_err);
        return true;
      } else {
        g_propagate_error(err, tmp_err);
        return false;
      }
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

  return _openslide_grid_paint_region(l->grid, cr, NULL,
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
  const void *icc_profile = get_icc_profile(l->file, &icc_profile_size);
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

static bool dicom_detect(const char *filename,
                         struct _openslide_tifflike *tl G_GNUC_UNUSED,
                         GError **err) {
  // some vendors use dual-personality TIFF/DCM files, so we can't just reject
  // tifflike files
  g_autoptr(dicom_file) f = dicom_file_new(filename, false, err);
  return f != NULL;
}

// replace with g_strv_equal() once we have glib 2.60
static bool strv_equal(const char *const *a, const char *const *b) {
  while (1) {
    if (!*a && !*b) {
      return true;
    }
    if (!*a || !*b || !g_str_equal(*a, *b)) {
      return false;
    }
    a++;
    b++;
  }
}

static bool is_type(char **type, const struct allowed_types *types) {
  // ImageType must be one of the combinations we accept
  for (int i = 0; i < types->n_types; i++) {
    if (strv_equal(types->types[i], (const char *const *) type)) {
      return true;
    }
  }
  return false;
}

static bool associated_get_argb_data(struct _openslide_associated_image *img,
                                     uint32_t *dest,
                                     GError **err) {
  struct associated *a = (struct associated *) img;
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
  if (a->file) {
    dicom_file_destroy(a->file);
  }
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

// error if two files have different SOP instance UIDs
// if we discover two files with the same purpose (e.g. two label images)
// and their UIDs are the same, it's a simple file duplication and we can
// ignore it ... if the UIDs are different, then something unexpected has
// happened and we must fail
static bool ensure_sop_instance_uids_equal(struct dicom_file *cur,
                                           struct dicom_file *prev,
                                           GError **err) {
  const char *cur_sop;
  const char *prev_sop;
  if (!get_tag_str(cur->metadata, SOPInstanceUID, 0, &cur_sop) ||
      !get_tag_str(prev->metadata, SOPInstanceUID, 0, &prev_sop)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read SOPInstanceUID");
    return false;
  }

  if (!g_str_equal(cur_sop, prev_sop)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Slide contains unexpected image (%s vs. %s)",
                cur_sop, prev_sop);
    return false;
  }

  if (_openslide_debug(OPENSLIDE_DEBUG_SEARCH)) {
    g_message("opening %s: SOP instance UID %s matches %s",
              cur->filename, cur_sop, prev->filename);
  }
  return true;
}

// unconditionally takes ownership of dicom_file
static bool add_associated(openslide_t *osr,
                           struct dicom_file *f,
                           char **image_type,
                           GError **err) {
  g_autoptr(associated) a = g_new0(struct associated, 1);
  a->base.ops = &dicom_associated_ops;
  a->file = f;

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
  if (g_str_equal(image_type[2], LABEL_TYPE)) {
    name = "label";
  } else if (g_str_equal(image_type[2], OVERVIEW_TYPE)) {
    name = "macro";
  } else if (g_str_equal(image_type[2], THUMBNAIL_TYPE)) {
    name = "thumbnail";
  } else {
    // is_type() let something unexpected through
    g_assert_not_reached();
  }

  // if we've seen this associated image type before and the SOP instance
  // UIDs match, someone duplicated a file; ignore it.  otherwise there's
  // something we don't understand about this slide and we must fail
  struct associated *previous =
    g_hash_table_lookup(osr->associated_images, name);
  if (previous) {
    return ensure_sop_instance_uids_equal(f, previous->file, err);
  }

  // add
  g_hash_table_insert(osr->associated_images,
                      g_strdup(name),
                      g_steal_pointer(&a));
  return true;
}

static struct dicom_level *find_level_by_dimensions(GPtrArray *level_array,
                                                    int64_t w, int64_t h) {
  for (guint i = 0; i < level_array->len; i++) {
    struct dicom_level *l = (struct dicom_level *) level_array->pdata[i];
    if (l->base.w == w && l->base.h == h) {
      return l;
    }
  }
  return NULL;
}

// unconditionally takes ownership of dicom_file
static bool add_level(openslide_t *osr,
                      GPtrArray *level_array,
                      struct dicom_file *f,
                      GError **err) {
  g_autoptr(dicom_level) l = g_new0(struct dicom_level, 1);
  l->file = f;

  // dimensions
  if (!get_tag_int(f->metadata, TotalPixelMatrixColumns, &l->base.w) ||
      !get_tag_int(f->metadata, TotalPixelMatrixRows, &l->base.h) ||
      !get_tag_int(f->metadata, Columns, &l->base.tile_w) ||
      !get_tag_int(f->metadata, Rows, &l->base.tile_h)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read level dimensions");
    return false;
  }

  // read PixelSpacing to expose as the mpp settings, if present
  DcmDataSet *shared_functional_group;
  DcmDataSet *pixel_measures;
  if (get_tag_seq_item(f->metadata,
                       SharedFunctionalGroupsSequence,
                       0,
                       &shared_functional_group) &&
      get_tag_seq_item(shared_functional_group,
                       PixelMeasuresSequence,
                       0,
                       &pixel_measures)) {
    get_tag_decimal_str(pixel_measures, PixelSpacing, 0, &l->pixel_spacing_x);
    get_tag_decimal_str(pixel_measures, PixelSpacing, 1, &l->pixel_spacing_y);
  }

  // objective power
  DcmDataSet *optical_path;
  if (get_tag_seq_item(f->metadata, OpticalPathSequence, 0, &optical_path)) {
    get_tag_decimal_str(optical_path, ObjectiveLensPower, 0, &l->objective_lens_power);
  }

  // grid
  int64_t tiles_across = (l->base.w / l->base.tile_w) + !!(l->base.w % l->base.tile_w);
  int64_t tiles_down = (l->base.h / l->base.tile_h) + !!(l->base.h % l->base.tile_h);
  l->grid = _openslide_grid_create_simple(osr,
                                          tiles_across, tiles_down,
                                          l->base.tile_w, l->base.tile_h,
                                          read_tile);

  // is this level already there?  if the SOP instance UIDs match, someone
  // duplicated a file; ignore it.  otherwise there's something about this
  // slide we don't understand and we must fail
  struct dicom_level *previous =
    find_level_by_dimensions(level_array, l->base.w, l->base.h);
  if (previous) {
    return ensure_sop_instance_uids_equal(f, previous->file, err);
  }

  // add
  g_ptr_array_add(level_array, g_steal_pointer(&l));
  return true;
}

// unconditionally takes ownership of dicom_file
static bool maybe_add_file(openslide_t *osr,
                           GPtrArray *level_array,
                           struct dicom_file *file,
                           GError **err) {
  g_autoptr(dicom_file) f = file;
  g_assert(f->metadata);

  // check ImageType
  g_auto(GStrv) image_type = get_tag_strv(f->metadata, ImageType, 4);
  if (!image_type) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't get ImageType");
    return false;
  }
  bool is_level = is_type(image_type, &LEVEL_TYPES);
  bool is_associated = is_type(image_type, &ASSOCIATED_TYPES);
  if (!is_level && !is_associated) {
    // unknown type; ignore
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
    if (g_str_equal(photometric, "YBR_ICT")) {
      f->jp2k_colorspace = OPENSLIDE_JP2K_YCBCR;
      found = true;
    } else if (g_str_equal(photometric, "RGB")) {
      f->jp2k_colorspace = OPENSLIDE_JP2K_RGB;
      found = true;
    }
    break;
  case FORMAT_JPEG:
    found = g_str_equal(photometric, "YBR_FULL_422") ||
            g_str_equal(photometric, "RGB");
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

  // add
  if (is_level) {
    return add_level(osr, level_array, g_steal_pointer(&f), err);
  } else {
    return add_associated(osr, g_steal_pointer(&f), image_type, err);
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
  // pixel spacing is in mm, so convert to microns
  if (level0->pixel_spacing_x && level0->pixel_spacing_y) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                        _openslide_format_double(1000.0 * level0->pixel_spacing_x));
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                        _openslide_format_double(1000.0 * level0->pixel_spacing_y));
  }
  if (level0->objective_lens_power) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER),
                        _openslide_format_double(level0->objective_lens_power));
  }

  // add all dicom elements
  struct property_iterate iter = { osr, "dicom", true };
  add_properties_dataset(level0->file->file_meta, 0, &iter);
  add_properties_dataset(level0->file->metadata, 0, &iter);
}

static gint compare_level_width(const void *a, const void *b) {
  const struct dicom_level *aa = *((const struct dicom_level **) a);
  const struct dicom_level *bb = *((const struct dicom_level **) b);

  return bb->base.w - aa->base.w;
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
    g_ptr_array_new_full(10, (GDestroyNotify) level_destroy);

  // open the passed-in file and get the slide-id
  g_autoptr(dicom_file) start = dicom_file_new(filename, true, err);
  if (!start) {
    return false;
  }
  g_autofree char *slide_id = g_strdup(start->slide_id);

  if (!maybe_add_file(osr, level_array, g_steal_pointer(&start), err)) {
    g_prefix_error(err, "Reading %s: ", filename);
    return false;
  }

  // scan for other DICOMs with this slide id
  const char *name;
  while ((name = _openslide_dir_next(dir))) {
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

    if (!maybe_add_file(osr, level_array, g_steal_pointer(&f), err)) {
      g_prefix_error(err, "Reading %s: ", path);
      return false;
    }
  }

  if (level_array->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No pyramid levels found");
    return false;
  }

  // sort levels by width
  g_ptr_array_sort(level_array, compare_level_width);

  struct dicom_level *level0 = level_array->pdata[0];
  add_properties(osr, level0);

  (void) get_icc_profile(level0->file, &osr->icc_profile_size);

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
