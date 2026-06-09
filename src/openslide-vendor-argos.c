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
 * ARGOS AVS support
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash,
 * using the top level of the middle Z-stack, plus the metadata XML
 *
 */

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

#define ARGOS_METADATA_TAG 65000

static const char ARGOS_ROOT_ELEMENT[] = "Argos.Scan.Metadata";

struct argos_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(destroy_level)

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr) {
  struct argos_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_free(data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
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
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    g_autofree uint32_t *buf = g_new(uint32_t, tw * th);
    if (!_openslide_tiff_read_tile(tiffl, tiff,
                                   buf, tile_col, tile_row,
                                   err)) {
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, buf,
                                   tile_col, tile_row,
                                   err)) {
      return false;
    }

    // put it in the cache
    tiledata = g_steal_pointer(&buf);
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
                         tiledata, tw * th * 4,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        tw, th, tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct argos_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(data->tc, err);
  if (ct.tiff == NULL) {
    return false;
  }

  return _openslide_grid_paint_region(l->grid, cr, ct.tiff,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops argos_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool argos_detect(const char *filename G_GNUC_UNUSED,
                         struct _openslide_tifflike *tl,
                         GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // ensure TIFF is tiled
  if (!_openslide_tifflike_is_tiled(tl, 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF is not tiled");
    return false;
  }

  // check for plausible root element before parsing
  const char *xml =
    _openslide_tifflike_get_buffer(tl, 0, ARGOS_METADATA_TAG, err);
  if (!xml) {
    return false;
  }
  if (!strstr(xml, ARGOS_ROOT_ELEMENT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "%s not in metadata field", ARGOS_ROOT_ELEMENT);
    return false;
  }

  // parse
  g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
  if (!doc) {
    return false;
  }

  // check root element
  xmlNode *root = xmlDocGetRootElement(doc);
  if (xmlStrcmp(root->name, BAD_CAST ARGOS_ROOT_ELEMENT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "XML root element not %s", ARGOS_ROOT_ELEMENT);
    return false;
  }

  return true;
}

static void add_properties_from_children(GHashTable *props,
                                         xmlNode *node, const char *prefix) {
  for (xmlNode *child = node->children; child; child = child->next) {
    g_autofree char *key =
      g_strdup_printf("%s.%s", prefix, (char *) child->name);
    add_properties_from_children(props, child, key);
    // add properties from nodes with no attributes and only one text child
    if (!child->properties &&
        child->children &&
        child->children->type == XML_TEXT_NODE &&
        !child->children->next) {
      g_hash_table_insert(props,
                          g_steal_pointer(&key),
                          g_strdup((char *) child->children->content));
    }
  }
}

static bool argos_parse_xml(openslide_t *osr, const char *xml,
                            int64_t *z_index, GError **err) {
  // parse XML
  g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
  if (!doc) {
    return false;
  }

  // add properties
  add_properties_from_children(osr->properties,
                               xmlDocGetRootElement(doc),
                               "argos");
  _openslide_duplicate_int_prop(osr,
                                "argos.ObjectiveMagnification",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_str_prop(osr,
                                "argos.Barcode",
                                OPENSLIDE_PROPERTY_NAME_BARCODE);

  // find index of middle Z-stack
  const char *minzs = g_hash_table_lookup(osr->properties, "argos.MinZ");
  const char *maxzs = g_hash_table_lookup(osr->properties, "argos.MaxZ");
  if (!minzs || !maxzs) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read focal plane indices");
    return false;
  }
  int64_t minz, maxz;
  if (!_openslide_parse_int64(minzs, &minz) ||
      !_openslide_parse_int64(maxzs, &maxz)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't parse focal plane indices: %s, %s", minzs, maxzs);
    return false;
  }
  *z_index = (maxz - minz) / 2;
  return true;
}

static bool argos_open(openslide_t *osr,
                       const char *filename,
                       struct _openslide_tifflike *tl,
                       struct _openslide_hash *quickhash1,
                       GError **err) {
  // parse metadata
  const char *xml =
    _openslide_tifflike_get_buffer(tl, 0, ARGOS_METADATA_TAG, err);
  if (!xml) {
    return false;
  }
  int64_t z_stack_skip;
  if (!argos_parse_xml(osr, xml, &z_stack_skip, err)) {
    g_prefix_error(err, "Parsing metadata XML: ");
    return false;
  }
  //g_debug("skipping %"PRId64" stacks", z_stack_skip);

  // open TIFF
  g_autoptr(_openslide_tiffcache) tc = _openslide_tiffcache_create(filename);
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  if (!ct.tiff) {
    return false;
  }

  // walk directories
  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func(OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(destroy_level));
  tdir_t dir_count = TIFFNumberOfDirectories(ct.tiff);
  int64_t prev_width = INT64_MAX;
  do {
    tdir_t dir = TIFFCurrentDirectory(ct.tiff);

    // verify that we can read this compression
    uint16_t compression;
    if (!TIFFGetField(ct.tiff, TIFFTAG_COMPRESSION, &compression)) {
      _openslide_tiff_error(err, ct.tiff, "Can't read compression scheme");
      return false;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported TIFF compression: %u", compression);
      return false;
    }

    if (TIFFIsTiled(ct.tiff)) {
      // pyramid level

      // skip early if we're past our Z-stack
      if (z_stack_skip < 0) {
        //g_debug("dir %u early skip", dir);
        continue;
      }

      // see if this is the next Z-stack, maybe skip
      uint32_t width;
      if (!TIFFGetField(ct.tiff, TIFFTAG_IMAGEWIDTH, &width)) {
        _openslide_tiff_error(err, ct.tiff, "Can't read image width");
        return false;
      }
      if (width >= prev_width) {
        z_stack_skip--;
      }
      prev_width = width;
      if (z_stack_skip) {
        //g_debug("dir %u skip, %"PRId64" stacks remaining", dir, z_stack_skip);
        continue;
      }

      // add level
      g_autoptr(level) l = g_new0(struct level, 1);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      if (!_openslide_tiff_level_init(ct.tiff, dir, &l->base, tiffl, err)) {
        return false;
      }
      l->grid = _openslide_grid_create_simple(osr,
                                              tiffl->tiles_across,
                                              tiffl->tiles_down,
                                              tiffl->tile_w,
                                              tiffl->tile_h,
                                              read_tile);
      if (!_openslide_tiff_missing_tiles_to_simple_grid(tiffl, ct.tiff,
                                                        l->grid, err)) {
        return false;
      }
      g_ptr_array_add(level_array, g_steal_pointer(&l));

    } else {
      // stripped; check for known associated image

      const char *name = NULL;
      // how many directories from the end?
      switch (dir_count - dir) {
      case 2:
        name = "thumbnail";
        break;
      case 1:
        name = "macro";
        break;
      }
      if (name &&
          !_openslide_tiff_add_associated_image(osr, name,
                                                tc, dir, NULL,
                                                err)) {
        return false;
      }
    }
  } while (TIFFReadDirectory(ct.tiff));

  // verify we didn't skip every level
  if (level_array->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No pyramid levels found");
    return false;
  }

  // set hash and properties
  struct level *bottom_level = level_array->pdata[0];
  struct level *top_level = level_array->pdata[level_array->len - 1];
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    top_level->tiffl.dir,
                                                    bottom_level->tiffl.dir,
                                                    err)) {
    return false;
  }
  _openslide_hash_string(quickhash1, xml);
  _openslide_tifflike_set_resolution_props(osr, tl, bottom_level->tiffl.dir);
  _openslide_set_bounds_props_from_grid(osr, &bottom_level->base,
                                        bottom_level->grid);

  // allocate private data
  struct argos_ops_data *data = g_new0(struct argos_ops_data, 1);
  data->tc = g_steal_pointer(&tc);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &argos_ops;

  return true;
}

const struct _openslide_format _openslide_format_argos = {
  .name = "argos",
  .vendor = "argos",
  .detect = argos_detect,
  .open = argos_open,
};
