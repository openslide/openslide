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
 */

/*
 * Ventana (bif) support
 *
 * quickhash comes from _openslide_tiff_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <math.h>
#include <tiffio.h>
#include <libxml/tree.h>

static const char LEVEL_DESCRIPTION_TOKEN[] = "level=";
static const char MACRO_DESCRIPTION[] = "Label Image";
static const char THUMBNAIL_DESCRIPTION[] = "Thumbnail";

static const char LEVEL_KEY[] = "level";
static const char MAGNIFICATION_KEY[] = "mag";

static const char INITIAL_ROOT_TAG[] = "iScan";
static const char ATTR_Z_LAYERS[] = "Z-layers";

#define PARSE_INT_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)		\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      g_propagate_error(err, tmp_err);				\
      goto FAIL;						\
    }								\
  } while (0)

struct ventana_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;

  double magnification;

  double subtile_w;
  double subtile_h;
};

static void destroy_data(struct ventana_ops_data *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct ventana_ops_data, data);

  for (int32_t i = 0; i < level_count; i++) {
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct ventana_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}

static bool read_subtile(openslide_t *osr,
                         cairo_t *cr,
                         struct _openslide_level *level,
                         int64_t subtile_col, int64_t subtile_row,
                         void *subtile G_GNUC_UNUSED,
                         void *arg,
                         GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;
  const int64_t subtiles_per_tile = l->base.downsample;
  bool success = true;

  // tile size and coordinates
  int64_t tile_col = subtile_col / subtiles_per_tile;
  int64_t tile_row = subtile_row / subtiles_per_tile;
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // subtile offset within tile
  double subtile_x = subtile_col % subtiles_per_tile * l->subtile_w;
  double subtile_y = subtile_row % subtiles_per_tile * l->subtile_h;

  // get tile data, possibly from cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, tiff,
                                   tiledata, tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
                         tiledata, tw * th * 4,
                         &cache_entry);
  }

  // draw
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);

  // if we are drawing a subtile, we must do an additional copy,
  // because cairo lacks source clipping
  if (subtiles_per_tile > 1) {
    cairo_surface_t *surface2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                           ceil(l->subtile_w),
                                                           ceil(l->subtile_h));
    cairo_t *cr2 = cairo_create(surface2);
    cairo_set_source_surface(cr2, surface, -subtile_x, -subtile_y);

    // replace original image surface
    cairo_surface_destroy(surface);
    surface = surface2;

    cairo_rectangle(cr2, 0, 0,
                    ceil(l->subtile_w),
                    ceil(l->subtile_h));
    cairo_fill(cr2);
    success = _openslide_check_cairo_status(cr2, err);
    cairo_destroy(cr2);
  }

  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return success;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct ventana_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  bool success = false;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff == NULL) {
    return false;
  }

  if (TIFFSetDirectory(tiff, l->tiffl.dir)) {
    success = _openslide_grid_paint_region(l->grid, cr, tiff,
                                           x / l->base.downsample,
                                           y / l->base.downsample,
                                           level, w, h,
                                           err);
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot set TIFF directory");
  }
  _openslide_tiffcache_put(data->tc, tiff);

  return success;
}

static const struct _openslide_ops ventana_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static char *read_xml_packet(TIFF *tiff) {
  void *xml;
  uint32_t len;
  if (!TIFFGetField(tiff, TIFFTAG_XMLPACKET, &len, &xml)) {
    return NULL;
  }
  // copy to ensure null-termination
  return g_strndup(xml, len);
}

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = *(const struct level **) a;
  const struct level *lb = *(const struct level **) b;

  if (la->tiffl.image_w > lb->tiffl.image_w) {
    return -1;
  } else if (la->tiffl.image_w == lb->tiffl.image_w) {
    return 0;
  } else {
    return 1;
  }
}

static bool parse_initial_xml(openslide_t *osr, const char *xml,
                              GError **err) {
  xmlDoc *doc = NULL;
  GError *tmp_err = NULL;

  // quick check for plausible XML string before parsing
  if (!strstr(xml, INITIAL_ROOT_TAG)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s not in XMLPacket", INITIAL_ROOT_TAG);
    goto FAIL;
  }

  // parse
  doc = _openslide_xml_parse(xml, &tmp_err);
  if (!doc) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s", tmp_err->message);
    g_clear_error(&tmp_err);
    goto FAIL;
  }
  xmlNode *root = xmlDocGetRootElement(doc);

  // check root tag name
  if (xmlStrcmp(root->name, BAD_CAST INITIAL_ROOT_TAG)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Root tag not %s", INITIAL_ROOT_TAG);
    goto FAIL;
  }

  // okay, assume Ventana slide

  // we don't know how to handle multiple Z layers
  int64_t z_layers;
  PARSE_INT_ATTRIBUTE_OR_FAIL(root, ATTR_Z_LAYERS, z_layers);
  if (z_layers != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Slides with multiple Z layers are not supported");
    goto FAIL;
  }

  if (osr) {
    // copy all iScan attributes to vendor properties
    for (xmlAttr *attr = root->properties; attr; attr = attr->next) {
      xmlChar *value = xmlGetNoNsProp(root, attr->name);
      if (value && *value) {
        g_hash_table_insert(osr->properties,
                            g_strdup_printf("ventana.%s", attr->name),
                            g_strdup((char *) value));
      }
      xmlFree(value);
    }

    // set standard properties
    _openslide_duplicate_int_prop(osr->properties, "ventana.Magnification",
                                  OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
    _openslide_duplicate_double_prop(osr->properties, "ventana.ScanRes",
                                     OPENSLIDE_PROPERTY_NAME_MPP_X);
    _openslide_duplicate_double_prop(osr->properties, "ventana.ScanRes",
                                     OPENSLIDE_PROPERTY_NAME_MPP_Y);
  }

  // clean up
  xmlFreeDoc(doc);
  return true;

FAIL:
  if (doc) {
    xmlFreeDoc(doc);
  }
  return false;
}

static bool parse_level_info(const char *desc,
                             int64_t *level, double *magnification,
                             GError **err) {
  bool success = false;

  // read all key/value pairs
  GHashTable *fields = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
  char **pairs = g_strsplit(desc, " ", 0);
  for (char **pair = pairs; *pair; pair++) {
    char **kv = g_strsplit(*pair, "=", 2);
    if (g_strv_length(kv) == 2) {
      g_hash_table_insert(fields, kv[0], kv[1]);
      g_free(kv);
    } else {
      g_strfreev(kv);
    }
  }
  g_strfreev(pairs);

  // get mandatory fields
  char *level_str = g_hash_table_lookup(fields, LEVEL_KEY);
  char *magnification_str = g_hash_table_lookup(fields, MAGNIFICATION_KEY);
  if (!level_str || !magnification_str) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Missing level fields");
    goto DONE;
  }

  // parse level
  gchar *endptr;
  *level = g_ascii_strtoll(level_str, &endptr, 10);
  if (level_str[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid level number");
    goto DONE;
  }

  // parse magnification
  *magnification = g_ascii_strtod(magnification_str, &endptr);
  if (magnification_str[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid magnification");
    goto DONE;
  }

  success = true;

DONE:
  g_hash_table_destroy(fields);
  return success;
}

static struct _openslide_grid *create_grid(openslide_t *osr,
                                           double subtile_w,
                                           double subtile_h,
                                           int64_t subtiles_across,
                                           int64_t subtiles_down) {
  struct _openslide_grid *grid =
    _openslide_grid_create_tilemap(osr, subtile_w, subtile_h,
                                   read_subtile, NULL);

  for (int64_t row = 0; row < subtiles_down; row++) {
    for (int64_t col = 0; col < subtiles_across; col++) {
      _openslide_grid_tilemap_add_tile(grid,
                                       col, row,
                                       0, 0,
                                       subtile_w, subtile_h,
                                       NULL);
    }
  }

  return grid;
}

bool _openslide_try_ventana(openslide_t *osr,
                            struct _openslide_tiffcache *tc,
                            TIFF *tiff,
                            struct _openslide_hash *quickhash1,
                            GError **err) {
  GPtrArray *level_array = g_ptr_array_new();

  // parse iScan XML
  char *xml = read_xml_packet(tiff);
  if (!xml) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Ventana slide");
    goto FAIL;
  }
  if (!parse_initial_xml(osr, xml, err)) {
    g_free(xml);
    goto FAIL;
  }
  g_free(xml);

  // okay, assume Ventana slide

  // walk directories
  int64_t next_level = 0;
  do {
    tdir_t dir = TIFFCurrentDirectory(tiff);

    // read ImageDescription
    char *image_desc;
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
      continue;
    }

    if (strstr(image_desc, LEVEL_DESCRIPTION_TOKEN)) {
      // is a level

      // parse description
      int64_t level;
      double magnification;
      if (!parse_level_info(image_desc, &level, &magnification, err)) {
        goto FAIL;
      }

      // verify that levels and magnifications are properly ordered
      if (level != next_level++) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unexpected encounter with level %"G_GINT64_FORMAT, level);
        goto FAIL;
      }
      if (level > 0) {
        struct level *prev_l = level_array->pdata[level - 1];
        if (magnification >= prev_l->magnification) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "Unexpected magnification in level %"G_GINT64_FORMAT,
                      level);
          goto FAIL;
        }
      }

      // compute downsample
      double downsample = 1;
      if (level > 0) {
        struct level *level0 = level_array->pdata[0];
        downsample = level0->magnification / magnification;
      }

      // confirm that this directory is tiled
      if (!TIFFIsTiled(tiff)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Directory %d is not tiled", dir);
        goto FAIL;
      }

      // verify that we can read this compression (hard fail if not)
      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Can't read compression scheme");
        goto FAIL;
      };
      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unsupported TIFF compression: %u", compression);
        goto FAIL;
      }

      // create level
      struct level *l = g_slice_new0(struct level);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      if (!_openslide_tiff_level_init(tiff, dir,
                                      (struct _openslide_level *) l,
                                      tiffl,
                                      err)) {
        g_slice_free(struct level, l);
        goto FAIL;
      }
      struct level *level0 = l;
      if (level > 0) {
        level0 = level_array->pdata[0];
      }
      l->base.downsample = downsample;
      l->magnification = magnification;
      l->subtile_w = level0->tiffl.tile_w / downsample;
      l->subtile_h = level0->tiffl.tile_h / downsample;
      l->grid = create_grid(osr,
                            l->subtile_w, l->subtile_h,
                            level0->tiffl.tiles_across,
                            level0->tiffl.tiles_down);
      //g_debug("level %"G_GINT64_FORMAT": magnification %g, downsample %g, subtile %g %g", level, magnification, downsample, l->subtile_w, l->subtile_h);

      // add to array
      g_ptr_array_add(level_array, l);

    } else if (!strcmp(image_desc, MACRO_DESCRIPTION)) {
      // macro image
      if (!_openslide_tiff_add_associated_image(osr, "macro", tc, dir,
                                                err)) {
	g_prefix_error(err, "Can't read macro image: ");
	goto FAIL;
      }

    } else if (!strcmp(image_desc, THUMBNAIL_DESCRIPTION)) {
      // thumbnail image
      if (!_openslide_tiff_add_associated_image(osr, "thumbnail", tc, dir,
                                                err)) {
	g_prefix_error(err, "Can't read thumbnail image: ");
	goto FAIL;
      }
    }
  } while (TIFFReadDirectory(tiff));

  // sort tiled levels
  g_ptr_array_sort(level_array, width_compare);

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct ventana_ops_data *data = g_slice_new0(struct ventana_ops_data);

  if (osr == NULL) {
    // free now and return
    _openslide_tiffcache_put(tc, tiff);
    data->tc = tc;
    destroy_data(data, levels, level_count);
    return true;
  }

  // set hash and properties
  if (!_openslide_tiff_init_properties_and_hash(osr, tiff, quickhash1,
                                                levels[level_count - 1]->tiffl.dir,
                                                levels[0]->tiffl.dir,
                                                err)) {
    destroy_data(data, levels, level_count);
    return false;
  }
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
                      g_strdup("ventana"));

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &ventana_ops;

  // put TIFF handle and assume tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

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

  return false;
}
