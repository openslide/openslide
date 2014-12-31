/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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
 */

/*
 * Philips TIFF support
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <math.h>
#include <string.h>
#include <tiffio.h>

static const char PHILIPS_SOFTWARE[] = "Philips";
static const char XML_ROOT[] = "DataObject";
static const char XML_ROOT_TYPE_ATTR[] = "ObjectType";
static const char XML_ROOT_TYPE_VALUE[] = "DPUfsImport";
static const char XML_NAME_ATTR[] = "Name";
static const char XML_SCANNED_IMAGES_NAME[] = "PIM_DP_SCANNED_IMAGES";
static const char XML_DATA_REPRESENTATION_NAME[] = "PIIM_PIXEL_DATA_REPRESENTATION_SEQUENCE";

static const char LABEL_DESCRIPTION[] = "Label";
static const char MACRO_DESCRIPTION[] = "Macro";

#define SCANNED_IMAGE_XPATH(image_type) \
  "/DataObject/Attribute[@Name='PIM_DP_SCANNED_IMAGES']/Array" \
  "/DataObject[Attribute/@Name='PIM_DP_IMAGE_TYPE' and " \
  "Attribute/text()='" image_type "']"
static const char MAIN_IMAGE_XPATH[] = SCANNED_IMAGE_XPATH("WSI");

#define ASSOCIATED_IMAGE_DATA_XPATH(image_type) \
  SCANNED_IMAGE_XPATH(image_type) "[1]" \
  "/Attribute[@Name='PIM_DP_IMAGE_DATA']/text()"
static const char LABEL_DATA_XPATH[] = ASSOCIATED_IMAGE_DATA_XPATH("LABELIMAGE");
static const char MACRO_DATA_XPATH[] = ASSOCIATED_IMAGE_DATA_XPATH("MACROIMAGE");

struct philips_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

struct xml_associated_image {
  struct _openslide_associated_image base;
  struct _openslide_tiffcache *tc;
  const char *xpath;  // static string; do not free
};

static void destroy(openslide_t *osr) {
  struct philips_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct philips_ops_data, data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }
  g_free(osr->levels);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    // slides with multiple ROIs are sparse
    bool is_missing;
    if (!_openslide_tiff_check_missing_tile(tiffl, tiff,
                                            tile_col, tile_row,
                                            &is_missing, err)) {
      return false;
    }

    if (is_missing) {
      // fill with transparent
      tiledata = g_slice_alloc0(tw * th * 4);

    } else {
      tiledata = g_slice_alloc(tw * th * 4);
      if (!_openslide_tiff_read_tile(tiffl, tiff,
                                     tiledata, tile_col, tile_row,
                                     err)) {
        g_slice_free1(tw * th * 4, tiledata);
        return false;
      }

      // clip, if necessary
      if (!_openslide_clip_tile(tiledata,
                                tw, th,
                                l->base.w - tile_col * tw,
                                l->base.h - tile_row * th,
                                err)) {
        g_slice_free1(tw * th * 4, tiledata);
        return false;
      }
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
                         tiledata, tw * th * 4,
                         &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct philips_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff == NULL) {
    return false;
  }

  bool success = _openslide_grid_paint_region(l->grid, cr, tiff,
                                              x / l->base.downsample,
                                              y / l->base.downsample,
                                              level, w, h,
                                              err);
  _openslide_tiffcache_put(data->tc, tiff);

  return success;
}

static const struct _openslide_ops philips_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool philips_detect(const char *filename G_GNUC_UNUSED,
                           struct _openslide_tifflike *tl,
                           GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // check Software field
  const char *software = _openslide_tifflike_get_buffer(tl, 0,
                                                        TIFFTAG_SOFTWARE,
                                                        err);
  if (!software) {
    return false;
  }
  if (!g_str_has_prefix(software, PHILIPS_SOFTWARE)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a Philips slide");
    return false;
  }

  // read XML description
  const char *image_desc =
    _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_IMAGEDESCRIPTION, err);
  if (!image_desc) {
    return false;
  }

  // try to parse the XML
  xmlDoc *doc = _openslide_xml_parse(image_desc, err);
  if (doc == NULL) {
    return false;
  }

  // check root tag name
  xmlNode *root = xmlDocGetRootElement(doc);
  if (xmlStrcmp(root->name, BAD_CAST XML_ROOT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Root tag not %s", XML_ROOT);
    xmlFreeDoc(doc);
    return false;
  }

  // check root tag type
  xmlChar *type = xmlGetProp(root, BAD_CAST XML_ROOT_TYPE_ATTR);
  if (!type || xmlStrcmp(type, BAD_CAST XML_ROOT_TYPE_VALUE)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Root %s not \"%s\"", XML_ROOT_TYPE_ATTR, XML_ROOT_TYPE_VALUE);
    xmlFree(type);
    xmlFreeDoc(doc);
    return false;
  }
  xmlFree(type);

  xmlFreeDoc(doc);
  return true;
}

static xmlDoc *parse_xml(TIFF *tiff, GError **err) {
  if (!_openslide_tiff_set_dir(tiff, 0, err)) {
    return NULL;
  }

  const char *image_desc;
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ImageDescription");
    return NULL;
  }
  return _openslide_xml_parse(image_desc, err);
}

static bool get_compressed_xml_associated_image_data(xmlDoc *doc,
                                                     const char *xpath,
                                                     void **out_data,
                                                     gsize *out_len,
                                                     GError **err) {
  xmlXPathContext *ctx = _openslide_xml_xpath_create(doc);
  char *b64_data = _openslide_xml_xpath_get_string(ctx, xpath);
  if (b64_data) {
    *out_data = g_base64_decode(b64_data, out_len);
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read associated image data");
  }
  g_free(b64_data);
  xmlXPathFreeContext(ctx);
  return b64_data != NULL;
}

static bool get_xml_associated_image_data(struct _openslide_associated_image *_img,
                                          uint32_t *dest,
                                          GError **err) {
  struct xml_associated_image *img = (struct xml_associated_image *) _img;
  void *data = NULL;
  bool success = false;

  TIFF *tiff = _openslide_tiffcache_get(img->tc, err);
  if (!tiff) {
    return false;
  }

  xmlDoc *doc = parse_xml(tiff, err);
  if (!doc) {
    goto DONE;
  }

  gsize len;
  if (!get_compressed_xml_associated_image_data(doc, img->xpath,
                                                &data, &len, err)) {
    goto DONE;
  }

  success = _openslide_jpeg_decode_buffer(data, len, dest,
                                          img->base.w, img->base.h, err);

DONE:
  g_free(data);
  if (doc) {
    xmlFreeDoc(doc);
  }
  _openslide_tiffcache_put(img->tc, tiff);
  return success;
}

static void destroy_xml_associated_image(struct _openslide_associated_image *_img) {
  struct xml_associated_image *img = (struct xml_associated_image *) _img;

  g_slice_free(struct xml_associated_image, img);
}

static const struct _openslide_associated_image_ops philips_xml_associated_ops = {
  .get_argb_data = get_xml_associated_image_data,
  .destroy = destroy_xml_associated_image,
};

// xpath is not copied (must be a static string)
static bool maybe_add_xml_associated_image(openslide_t *osr,
                                           struct _openslide_tiffcache *tc,
                                           xmlDoc *doc,
                                           const char *name,
                                           const char *xpath,
                                           GError **err) {
  if (g_hash_table_lookup(osr->associated_images, name)) {
    // already added from TIFF directory
    return true;
  }

  void *data;
  gsize len;
  if (!get_compressed_xml_associated_image_data(doc, xpath,
                                                &data, &len, err)) {
    g_prefix_error(err, "Can't locate %s associated image: ", name);
    return false;
  }

  int32_t w, h;
  bool success =
    _openslide_jpeg_decode_buffer_dimensions(data, len, &w, &h, err);
  g_free(data);
  if (!success) {
    g_prefix_error(err, "Can't decode %s associated image: ", name);
    return false;
  }

  //g_debug("Adding %s image from XML", name);
  struct xml_associated_image *img = g_slice_new0(struct xml_associated_image);
  img->base.ops = &philips_xml_associated_ops;
  img->base.w = w;
  img->base.h = h;
  img->tc = tc;
  img->xpath = xpath;

  g_hash_table_insert(osr->associated_images, g_strdup(name), img);

  return true;
}

static void add_properties(openslide_t *osr,
                           xmlXPathContext *ctx,
                           const char *prefix,
                           const char *xpath);

static void add_properties_from_array(openslide_t *osr,
                                      xmlXPathContext *ctx,
                                      const char *prefix,
                                      xmlNode *node) {
  xmlChar *name = xmlGetProp(node, BAD_CAST XML_NAME_ATTR);
  ctx->node = node;
  xmlXPathObject *result = _openslide_xml_xpath_eval(ctx, "Array/DataObject");
  for (int i = 0; result && i < result->nodesetval->nodeNr; i++) {
    ctx->node = result->nodesetval->nodeTab[i];
    char *sub_prefix = g_strdup_printf("%s.%s[%d]", prefix, name, i);
    add_properties(osr, ctx, sub_prefix, "Attribute");
    g_free(sub_prefix);
  }
  xmlXPathFreeObject(result);
  xmlFree(name);
}

static void add_properties(openslide_t *osr,
                           xmlXPathContext *ctx,
                           const char *prefix,
                           const char *xpath) {
  xmlXPathObject *result = _openslide_xml_xpath_eval(ctx, xpath);
  for (int i = 0; result && i < result->nodesetval->nodeNr; i++) {
    xmlNode *node = result->nodesetval->nodeTab[i];
    xmlChar *name = xmlGetProp(node, BAD_CAST XML_NAME_ATTR);

    if (name) {
      if (!xmlStrcmp(name, BAD_CAST XML_SCANNED_IMAGES_NAME)) {
        // Recurse only into first WSI image
        ctx->node = node;
        add_properties(osr, ctx, prefix,
                       "Array/DataObject[Attribute/@Name='PIM_DP_IMAGE_TYPE' "
                       "and Attribute/text()='WSI'][1]/Attribute");

      } else if (!xmlStrcmp(name, BAD_CAST XML_DATA_REPRESENTATION_NAME)) {
        // Recurse into every PixelDataRepresentation
        add_properties_from_array(osr, ctx, prefix, node);

      } else if (!xmlFirstElementChild(node)) {
        // Add value
        xmlChar *value = xmlNodeGetContent(node);
        if (value) {
          g_hash_table_insert(osr->properties,
                              g_strdup_printf("%s.%s", prefix, (char *) name),
                              g_strdup((char *) value));
        }
        xmlFree(value);
      }
    }
    xmlFree(name);
  }
  xmlXPathFreeObject(result);
}

// returns *w and *h in mm
static bool parse_pixel_spacing(const char *spacing,
                                double *w, double *h,
                                GError **err) {
  bool success = false;
  char **spacings = g_strsplit(spacing, " ", 0);
  if (g_strv_length(spacings) == 2) {
    for (int i = 0; i < 2; i++) {
      // strip quotes
      g_strstrip(g_strdelimit(spacings[i], "\"", ' '));
    }
    // row spacing, then column spacing
    *w = _openslide_parse_double(spacings[1]);
    *h = _openslide_parse_double(spacings[0]);
    success = !isnan(*w) && !isnan(*h);
  }
  g_strfreev(spacings);
  if (!success) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't parse pixel spacing");
  }
  return success;
}

static void add_mpp_properties(openslide_t *osr) {
  const char *spacing = g_hash_table_lookup(osr->properties,
                                            "philips.DICOM_PIXEL_SPACING");
  if (spacing) {
    double w, h;
    if (parse_pixel_spacing(spacing, &w, &h, NULL)) {
      g_hash_table_insert(osr->properties,
                          g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                          _openslide_format_double(1e3 * w));
      g_hash_table_insert(osr->properties,
                          g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                          _openslide_format_double(1e3 * h));
    }
  }
}

static bool fix_level_dimensions(struct level **levels,
                                 int32_t level_count,
                                 xmlDoc *doc,
                                 GError **err) {
  bool success = false;

  // query pixel spacings
  xmlXPathContext *ctx = _openslide_xml_xpath_create(doc);
  xmlXPathObject *result =
    _openslide_xml_xpath_eval(ctx,
                              "/DataObject"
                              "/Attribute[@Name='PIM_DP_SCANNED_IMAGES']"
                              "/Array"
                              "/DataObject[Attribute/@Name='PIM_DP_IMAGE_TYPE' "
                              "and Attribute/text()='WSI'][1]"
                              "/Attribute[@Name='PIIM_PIXEL_DATA_REPRESENTATION_SEQUENCE']"
                              "/Array"
                              "/DataObject[@ObjectType='PixelDataRepresentation']"
                              "/Attribute[@Name='DICOM_PIXEL_SPACING']"
                              "/text()");
  if (!result || result->nodesetval->nodeNr != level_count) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't get level downsamples");
    goto DONE;
  }

  // walk levels
  double l0_w = 0;
  double l0_h = 0;
  for (int32_t i = 0; i < level_count; i++) {
    xmlChar *spacing = xmlNodeGetContent(result->nodesetval->nodeTab[i]);
    double w, h;
    bool ok = parse_pixel_spacing((const char *) spacing, &w, &h, err);
    xmlFree(spacing);
    if (!ok) {
      g_prefix_error(err, "Level %d: ", i);
      goto DONE;
    }

    if (i == 0) {
      l0_w = w;
      l0_h = h;
    } else {
      // calculate downsample
      // assume integer downsamples (which seems valid so far) to avoid
      // issues with floating-point error
      levels[i]->base.downsample = round(((w / l0_w) + (h / l0_h)) / 2);

      // clip excess padding
      levels[i]->base.w = levels[0]->base.w / levels[i]->base.downsample;
      levels[i]->base.h = levels[0]->base.h / levels[i]->base.downsample;
    }
  }

  success = true;

DONE:
  xmlXPathFreeObject(result);
  xmlXPathFreeContext(ctx);
  return success;
}

static bool verify_main_image_count(xmlDoc *doc, GError **err) {
  xmlXPathContext *ctx = _openslide_xml_xpath_create(doc);
  xmlXPathObject *result = _openslide_xml_xpath_eval(ctx, MAIN_IMAGE_XPATH);
  int count = result ? result->nodesetval->nodeNr : 0;
  xmlXPathFreeObject(result);
  xmlXPathFreeContext(ctx);
  if (count != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected one WSI image, found %d", count);
    return false;
  }
  return true;
}

static bool philips_open(openslide_t *osr,
                         const char *filename,
                         struct _openslide_tifflike *tl,
                         struct _openslide_hash *quickhash1,
                         GError **err) {
  GPtrArray *level_array = g_ptr_array_new();
  xmlDoc *doc = NULL;
  bool success = false;

  // open TIFF
  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  if (!tiff) {
    goto FAIL;
  }

  // parse XML document
  doc = parse_xml(tiff, err);
  if (doc == NULL) {
    goto FAIL;
  }

  // ensure there is only one WSI DPScannedImage in the XML
  if (!verify_main_image_count(doc, err)) {
    goto FAIL;
  }

  // create levels
  struct level *prev_l = NULL;
  do {
    // get directory
    tdir_t dir = TIFFCurrentDirectory(tiff);

    // get ImageDescription
    const char *image_desc;
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
      image_desc = NULL;
    }

    if (TIFFIsTiled(tiff)) {
      // pyramid level

      // confirm it is either the first image, or reduced-resolution
      if (prev_l) {
        uint32_t subfiletype;
        if (!TIFFGetField(tiff, TIFFTAG_SUBFILETYPE, &subfiletype) ||
            !(subfiletype & FILETYPE_REDUCEDIMAGE)) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "Directory %d is not reduced-resolution", dir);
          goto FAIL;
        }
      }

      // verify that we can read this compression
      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read compression scheme");
        goto FAIL;
      };
      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unsupported TIFF compression: %u", compression);
        goto FAIL;
      }

      // create level
      struct level *l = g_slice_new0(struct level);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      if (!_openslide_tiff_level_init(tiff, dir,
                                      (struct _openslide_level *) l, tiffl,
                                      err)) {
        g_slice_free(struct level, l);
        goto FAIL;
      }
      l->grid = _openslide_grid_create_simple(osr,
                                              tiffl->tiles_across,
                                              tiffl->tiles_down,
                                              tiffl->tile_w,
                                              tiffl->tile_h,
                                              read_tile);

      // add to array
      g_ptr_array_add(level_array, l);

      // verify that levels are sorted by size
      if (prev_l &&
          (tiffl->image_w > prev_l->tiffl.image_w ||
           tiffl->image_h > prev_l->tiffl.image_h)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unexpected dimensions for directory %d", dir);
        goto FAIL;
      }
      prev_l = l;

    } else if (image_desc &&
               g_str_has_prefix(image_desc, LABEL_DESCRIPTION)) {
      // label
      //g_debug("Adding label image from directory %d", dir);
      if (!_openslide_tiff_add_associated_image(osr, "label", tc, dir, err)) {
        goto FAIL;
      }

    } else if (image_desc &&
               g_str_has_prefix(image_desc, MACRO_DESCRIPTION)) {
      // macro image
      //g_debug("Adding macro image from directory %d", dir);
      if (!_openslide_tiff_add_associated_image(osr, "macro", tc, dir, err)) {
        goto FAIL;
      }
    }
  } while (TIFFReadDirectory(tiff));

  // override level dimensions and downsamples to work around incorrect
  // level dimensions in the metadata
  if (!fix_level_dimensions((struct level **) level_array->pdata,
                            level_array->len,
                            doc, err)) {
    goto FAIL;
  }

  // set hash and properties
  g_assert(level_array->len > 0);
  struct level *top_level = level_array->pdata[level_array->len - 1];
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    top_level->tiffl.dir,
                                                    0,
                                                    err)) {
    goto FAIL;
  }

  // keep the XML document out of the properties
  g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
  g_hash_table_remove(osr->properties, "tiff.ImageDescription");

  // add properties from XML
  xmlXPathContext *ctx = _openslide_xml_xpath_create(doc);
  add_properties(osr, ctx, "philips", "/DataObject/Attribute");
  add_mpp_properties(osr);
  xmlXPathFreeContext(ctx);

  // add associated images from XML
  // errors are non-fatal
  maybe_add_xml_associated_image(osr, tc, doc,
                                 "label", LABEL_DATA_XPATH, NULL);
  maybe_add_xml_associated_image(osr, tc, doc,
                                 "macro", MACRO_DATA_XPATH, NULL);

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct philips_ops_data *data = g_slice_new0(struct philips_ops_data);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &philips_ops;

  // put TIFF handle and store tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  // done
  success = true;
  goto DONE;

FAIL:
  // free the level array
  if (level_array) {
    for (uint32_t n = 0; n < level_array->len; n++) {
      struct level *l = level_array->pdata[n];
      _openslide_grid_destroy(l->grid);
      g_slice_free(struct level, l);
    }
    g_ptr_array_free(level_array, true);
  }
  // free TIFF
  _openslide_tiffcache_put(tc, tiff);
  _openslide_tiffcache_destroy(tc);

DONE:
  // free XML
  if (doc) {
    xmlFreeDoc(doc);
  }
  return success;
}

const struct _openslide_format _openslide_format_philips = {
  .name = "philips",
  .vendor = "philips",
  .detect = philips_detect,
  .open = philips_open,
};
