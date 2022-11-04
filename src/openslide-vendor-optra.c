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
 * Optra (tif, otif) support added by surajoptra
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#define MIN_THUMBNAIL_DIM   500
static const char XML_ROOT_TAG[] = "ScanInfo";

struct optra_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_slice_free(struct level, l);
}

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr) {
  struct optra_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct optra_ops_data, data);

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
    g_auto(_openslide_slice) box = _openslide_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, tiff,
                                   box.p, tile_col, tile_row,
                                   err)) {
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, box.p,
                                   tile_col, tile_row,
                                   err)) {
      return false;
    }

    // put it in the cache
    tiledata = _openslide_slice_steal(&box);
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
  struct optra_ops_data *data = osr->data;
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

static const struct _openslide_ops optra_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static xmlNode* get_initial_root_xml(xmlDoc* doc, GError** err) {
    xmlNode* root = xmlDocGetRootElement(doc);
    if (!xmlStrcmp(root->name, BAD_CAST XML_ROOT_TAG)) {
        // /ScanInfo
        return root;
    }
    else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
            "Unrecognized root element in optrascan XML");
        return false;
    }
}

static bool parse_initial_xml(openslide_t* osr, const char* xml,
    GError** err) {
    // parse
    g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
    if (!doc) {
        return false;
    }

    // get ScanInfo element
    xmlNode* scaninfo = get_initial_root_xml(doc, err);
    if (!scaninfo) {
        return false;
    }

    // copy all ScanInfo attributes to vendor properties
    for (xmlAttr* attr = scaninfo->properties; attr; attr = attr->next) {
        g_autoptr(xmlChar) value = xmlGetNoNsProp(scaninfo, attr->name);
        if (value && *value) {
            g_hash_table_insert(osr->properties,
                g_strdup_printf("ventana.%s", attr->name),
                g_strdup((char*)value));
        }
    }

    // set standard properties
    _openslide_duplicate_int_prop(osr, "optra.Magnification",
        OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
    _openslide_duplicate_double_prop(osr, "optra.PixelResolution",
        OPENSLIDE_PROPERTY_NAME_MPP_X);
    _openslide_duplicate_double_prop(osr, "optra.PixelResolution",
        OPENSLIDE_PROPERTY_NAME_MPP_Y);

    return true;
}

static bool optra_detect(const char *filename G_GNUC_UNUSED,
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

  const char* xml = _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_XMLPACKET,
      err);
  if (!xml) {
      return false;
  }

  // check for plausible XML string before parsing
  if (!strstr(xml, XML_ROOT_TAG)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
          "%s not in XMLPacket", XML_ROOT_TAG);
      return false;
  }

  // parse
  g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
  if (!doc) {
      return false;
  }

  // check for iScan element
  if (!get_initial_root_xml(doc, err)) {
      return false;
  }

  return true;
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

static bool optra_open(openslide_t *osr,
                              const char *filename,
                              struct _openslide_tifflike *tl,
                              struct _openslide_hash *quickhash1,
                              GError **err) {
  // open TIFF
  g_autoptr(_openslide_tiffcache) tc = _openslide_tiffcache_create(filename);
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  if (!ct.tiff) {
    return false;
  }

  // parse initial XML
  const char* xml = _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_XMLPACKET,
      err);
  if (!xml) {
      return false;
  }
  if (!parse_initial_xml(osr, xml, err)) {
      return false;
  }

  tdir_t tn_dir = TIFFCurrentDirectory(ct.tiff); //dir to hold thumbnail level.
  // accumulate tiled levels
  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_level);
  do {
    // confirm that this directory is tiled
    if (!TIFFIsTiled(ct.tiff)) {
      continue;
    }

    // confirm it is either the first image, or reduced-resolution
    if (TIFFCurrentDirectory(ct.tiff) != 0) {
      uint32_t subfiletype;
      if (!TIFFGetField(ct.tiff, TIFFTAG_SUBFILETYPE, &subfiletype)) {
        continue;
      }

      if (!(subfiletype & FILETYPE_REDUCEDIMAGE)) {
          //read image description and add as associated image.
          char* image_desc;
          if (!TIFFGetField(ct.tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
              //g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                //  "reading image description failed.");
              continue; //not an error.
          }
          if (!_openslide_tiff_add_associated_image(osr, image_desc, tc, TIFFCurrentDirectory(ct.tiff), err)) {
              return false;
          }
        continue;
      }
      else
      {
          //check if suitable to be thumbnail directory.
          uint32_t imwidth = 0, imheight = 0;
          if (!TIFFGetField(ct.tiff, TIFFTAG_IMAGEWIDTH, &imwidth)) {
              g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "reading image width failed");
              return false;
          }
          if (!TIFFGetField(ct.tiff, TIFFTAG_IMAGELENGTH, &imheight)) {
              g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "reading image height failed");
              return false;
          }
          if ((imwidth > MIN_THUMBNAIL_DIM) && (imheight > MIN_THUMBNAIL_DIM))
              tn_dir = TIFFCurrentDirectory(ct.tiff);//this will be over-written until last level
      }
    }

    // verify that we can read this compression (hard fail if not)
    uint16_t compression;
    if (!TIFFGetField(ct.tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read compression scheme");
      return false;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported TIFF compression: %u", compression);
      return false;
    }

    // create level
    g_autoptr(level) l = g_slice_new0(struct level);
    struct _openslide_tiff_level *tiffl = &l->tiffl;
    if (!_openslide_tiff_level_init(ct.tiff,
                                    TIFFCurrentDirectory(ct.tiff),
                                    (struct _openslide_level *) l,
                                    tiffl,
                                    err)) {
      return false;
    }
    l->grid = _openslide_grid_create_simple(osr,
                                            tiffl->tiles_across,
                                            tiffl->tiles_down,
                                            tiffl->tile_w,
                                            tiffl->tile_h,
                                            read_tile);

    // add to array
    g_ptr_array_add(level_array, g_steal_pointer(&l));
  } while (TIFFReadDirectory(ct.tiff));

  //add last reduced page as thumbnail image.
  //go to last level page
  if (!_openslide_tiff_set_dir(ct.tiff, tn_dir, err)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
          "adding last level as thumbnail failed");
      return false;
  }
  //add last level page as associated thumbnail image.
  if (!_openslide_tiff_add_associated_image(osr, "thumbnail", tc, TIFFCurrentDirectory(ct.tiff), err)) {
      return false;
  }

  // sort tiled levels
  g_ptr_array_sort(level_array, width_compare);

  // set hash and properties
  struct level *top_level = level_array->pdata[level_array->len - 1];
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    top_level->tiffl.dir,
                                                    0,
                                                    err)) {
    return false;
  }

  // allocate private data
  struct optra_ops_data *data =
    g_slice_new0(struct optra_ops_data);
  data->tc = g_steal_pointer(&tc);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &optra_ops;

  return true;
}

const struct _openslide_format _openslide_format_optra = {
  .name = "optra",
  .vendor = "optra",
  .detect = optra_detect,
  .open = optra_open,
};
