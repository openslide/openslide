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
 * OME TIFF support
 *
 * quickhash comes from properties in full-resolution plane
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

static const char ROOT_OME_ELEMENT[] = "OME";
static const char OMETIFF_ATTR_SIZE_C[] = "SizeC";
static const char OMETIFF_ATTR_SIZE_T[] = "SizeT";
static const char OMETIFF_ATTR_SIZE_X[] = "SizeX";
static const char OMETIFF_ATTR_SIZE_Y[] = "SizeY";
static const char OMETIFF_ATTR_SIZE_Z[] = "SizeZ";
static const char OMETIFF_ATTR_FIRST_C[] = "FirstC";
static const char OMETIFF_ATTR_FIRST_T[] = "FirstT";
static const char OMETIFF_ATTR_FIRST_Z[] = "FirstZ";
static const char OMETIFF_ATTR_PHYSICAL_SIZE_X[] = "PhysicalSizeX";
static const char OMETIFF_ATTR_PHYSICAL_SIZE_Y[] = "PhysicalSizeY";
static const char OMETIFF_ATTR_PHYSICAL_SIZE_X_UNIT[] = "PhysicalSizeXUnit";
static const char OMETIFF_ATTR_PHYSICAL_SIZE_Y_UNIT[] = "PhysicalSizeYUnit";
static const char OMETIFF_ATTR_IFD[] = "IFD";

#define PARSE_INT_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)		\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      g_propagate_error(err, tmp_err);				\
      goto FAIL;						\
    }								\
  } while (0)

#define PARSE_INT_ATTRIBUTE_OR_DEFAULT(NODE, NAME, OUT, DEFAULT)		\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      OUT = DEFAULT;					\
    }								\
  } while (0)

#define PARSE_FLOAT_ATTRIBUTE_OR_DEFAULT(NODE, NAME, OUT, DEFAULT)		\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_double_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      OUT = DEFAULT;					\
    }								\
  } while (0)


struct ome_tiff_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

/* structs representing data parsed from ImageDescription XML */
struct pixels {
  int64_t size_x;
  int64_t size_y;
  int64_t size_z;
  int64_t size_c;
  int64_t size_t;
  double mpp_x;
  double mpp_y;

  GPtrArray* tiffdata;
};

struct tiffdata {
  int64_t ifd;
  int64_t first_z;
  int64_t first_t;
  int64_t first_c;
};

static void destroy(openslide_t *osr) {
  struct ome_tiff_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct ome_tiff_ops_data, data);

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
  struct ome_tiff_ops_data *data = osr->data;
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

static const struct _openslide_ops ome_tiff_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static xmlNode* get_root_xml_ome(xmlDoc* doc, GError** err) {
  xmlNode* root = xmlDocGetRootElement(doc);
  if (!xmlStrcmp(root->name, BAD_CAST ROOT_OME_ELEMENT)) {
    // /OME
    return root;
  }
  else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unrecognized root element in XML");
    return false;
  }
}

static bool ome_tiff_detect(const char *filename G_GNUC_UNUSED,
                            struct _openslide_tifflike *tl,
                            GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // read image description
  const char* xml = _openslide_tifflike_get_buffer(tl, 0,
                                                   TIFFTAG_IMAGEDESCRIPTION,
                                                   err);
  if (!xml) {
      return false;
  }

  // check for plausible XML string before parsing
  if (!strstr(xml, ROOT_OME_ELEMENT)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
          "%s not in ImageDescription", ROOT_OME_ELEMENT);
      return false;
  }

  // parse
  xmlDoc* doc = _openslide_xml_parse(xml, err);
  if (!doc) {
      return false;
  }

  // check for root OME element
  if (!get_root_xml_ome(doc, err)) {
      xmlFreeDoc(doc);
      return false;
  }

  xmlFreeDoc(doc);
  return true;
}

static void pixels_free(struct pixels* pixels) {
  if (!pixels) {
    return;
  }

  for (uint32_t tiffdir_num = 0; tiffdir_num < pixels->tiffdata->len;
       tiffdir_num++) {
    struct tiffdata* tiffdata = pixels->tiffdata->pdata[tiffdir_num];
    g_slice_free(struct tiffdata, tiffdata);
  }
  g_ptr_array_free(pixels->tiffdata, true);
  g_slice_free(struct pixels, pixels);
}

static double convert_to_mpp(double size,
                             const xmlChar* size_unit) {
  if (size > 0.0) {
    if (!size_unit || !xmlStrcmp(size_unit, BAD_CAST "\xc2\xb5m")) { // 'µm' micrometer SI unit
      return size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "Ym")) { // yottameter SI unit
      return 1.0e30 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "Zm")) { // zettameter SI unit
      return 1.0e27 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "Em")) { // exameter SI unit
      return 1.0e24 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "Pm")) { // petameter SI unit
      return 1.0e21 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "Tm")) { // terameter SI unit
      return 1.0e18 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "Gm")) { // gigameter SI unit
      return 1.0e15 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "Mm")) { // megameter SI unit
      return 1.0e12 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "km")) { // kilometer SI unit
      return 1.0e9 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "hm")) { // hectometer SI unit
      return 1.0e8 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "dam")) { // decameter SI unit
      return 1.0e7 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "m")) { // meter SI unit
      return 1.0e6 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "dm")) { // decimeter SI unit
      return 1.0e5 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "cm")) { // centimeter SI unit
      return 1.0e4 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "mm")) { // millimeter SI unit
      return 1.0e3 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "nm")) { // nanometer SI unit
      return 1.0e-3 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "pm")) { // picometer SI unit
      return 1.0e-6 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "fm")) { // femtometer SI unit
      return 1.0e-9 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "am")) { // attometer SI unit
      return 1.0e-12 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "zm")) { // zeptometer SI unit
      return 1.0e-15 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "ym")) { // yoctometer SI unit
      return 1.0e-18 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "\xc3\x85")) { // 'Å' ångström SI-derived unit
      return 1.0e-4 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "thou")) { // thou Imperial unit (or mil, 1/1000 inch)
      return ((1/1000.0) * 2.54e4) * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "li")) { // line Imperial unit (1/12 inch)
      return ((1/12.0) * 2.54e4) * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "in")) { // inch Imperial unit
      return 2.54e4 * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "ft")) { // foot Imperial unit
      return (12 * 2.54e4) * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "yd")) { // yard Imperial unit
      return (3 * 12 * 2.54e4) * size;
    }
    else if (!xmlStrcmp(size_unit, BAD_CAST "mi")) { // terrestrial mile Imperial unit
      return (1760 * 3 * 12 * 2.54e4) * size;
    }
  }

  return 0.0;
}

static void get_pixels_mpp(xmlNode* pixels_node,
                           struct pixels* pixels) {
  // the PhysicalSizeX and PhysicalSizeY attributes are optional
  double physical_size_x, physical_size_y;
  PARSE_FLOAT_ATTRIBUTE_OR_DEFAULT(pixels_node, OMETIFF_ATTR_PHYSICAL_SIZE_X,
                                   physical_size_x, 0.0);
  PARSE_FLOAT_ATTRIBUTE_OR_DEFAULT(pixels_node, OMETIFF_ATTR_PHYSICAL_SIZE_Y,
                                   physical_size_y, 0.0);

  // the PhysicalSizeXUnit and PhysicalSizeYUnit attributes are optional but
  // default to 'µm'
  xmlChar* physical_size_x_unit = xmlGetProp(pixels_node,
                                   BAD_CAST OMETIFF_ATTR_PHYSICAL_SIZE_X_UNIT);
  pixels->mpp_x = convert_to_mpp(physical_size_x, physical_size_x_unit);
  xmlFree(physical_size_x_unit);

  xmlChar* physical_size_y_unit = xmlGetProp(pixels_node,
                                   BAD_CAST OMETIFF_ATTR_PHYSICAL_SIZE_Y_UNIT);
  pixels->mpp_y = convert_to_mpp(physical_size_y, physical_size_y_unit);
  xmlFree(physical_size_y_unit);
}

static struct pixels* parse_xml_description(const char* xml,
                                            GError** err) {
  xmlXPathContext* ctx = NULL;
  xmlXPathObject* tiffdata_result = NULL;
  xmlXPathObject* result = NULL;
  struct pixels* pixels = NULL;
  bool success = false;

  // parse the xml
  xmlDoc* doc = _openslide_xml_parse(xml, err);
  if (doc == NULL) {
    return false;
  }

  // create XPATH context to query the document
  ctx = _openslide_xml_xpath_create(doc);

  // the OME XML schema is defined at
  // https://www.openmicroscopy.org/Schemas/Documentation/Generated/OME-2016-06/ome.html
  // this has a structure for OME TIFF as follows:
  /*
     OME (root node)
       Image
         Pixels
           TiffData (1..n)
  */

  // get the Pixels node
  xmlNode* pixels_node = _openslide_xml_xpath_get_node(ctx,
                                                       "/d:OME/d:Image[1]/d:Pixels");
  if (!pixels_node) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find Pixels element");
    goto FAIL;
  }

  // create pixels struct
  pixels = g_slice_new0(struct pixels);
  pixels->tiffdata = g_ptr_array_new();

  PARSE_INT_ATTRIBUTE_OR_FAIL(pixels_node, OMETIFF_ATTR_SIZE_C,
                              pixels->size_c);
  PARSE_INT_ATTRIBUTE_OR_FAIL(pixels_node, OMETIFF_ATTR_SIZE_T,
                              pixels->size_t);
  PARSE_INT_ATTRIBUTE_OR_FAIL(pixels_node, OMETIFF_ATTR_SIZE_X,
                              pixels->size_x);
  PARSE_INT_ATTRIBUTE_OR_FAIL(pixels_node, OMETIFF_ATTR_SIZE_Y,
                              pixels->size_y);
  PARSE_INT_ATTRIBUTE_OR_FAIL(pixels_node, OMETIFF_ATTR_SIZE_Z,
                              pixels->size_z);
  get_pixels_mpp(pixels_node, pixels);

  // get the TiffData nodes
  ctx->node = pixels_node;
  tiffdata_result = _openslide_xml_xpath_eval(ctx, "d:TiffData");
  if (!tiffdata_result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find any TiffData elements");
    goto FAIL;
  }

  // create tiffdata structs
  for (int i = 0; i < tiffdata_result->nodesetval->nodeNr; i++) {
    xmlNode* tiffdata_node = tiffdata_result->nodesetval->nodeTab[i];
    ctx->node = tiffdata_node;

    // create tiffdata struct
    struct tiffdata* tiffdata = g_slice_new0(struct tiffdata);
    g_ptr_array_add(pixels->tiffdata, tiffdata);

    PARSE_INT_ATTRIBUTE_OR_DEFAULT(tiffdata_node, OMETIFF_ATTR_FIRST_C,
                                   tiffdata->first_c, 0);
    PARSE_INT_ATTRIBUTE_OR_DEFAULT(tiffdata_node, OMETIFF_ATTR_FIRST_T,
                                   tiffdata->first_t, 0);
    PARSE_INT_ATTRIBUTE_OR_DEFAULT(tiffdata_node, OMETIFF_ATTR_FIRST_Z,
                                   tiffdata->first_z, 0);
    PARSE_INT_ATTRIBUTE_OR_DEFAULT(tiffdata_node, OMETIFF_ATTR_IFD,
                                   tiffdata->ifd, 0);
  }

  success = true;
  // fall thru'

FAIL:
  xmlXPathFreeObject(result);
  xmlXPathFreeObject(tiffdata_result);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);

  if (success) {
    return pixels;
  }
  else {
    pixels_free(pixels);
    return NULL;
  }
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

static bool verify_compression(TIFF* tiff,
                               GError** err) {
  uint16_t compression;
  if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read compression scheme");
      return false;
  };
  if (!TIFFIsCODECConfigured(compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported TIFF compression: %u", compression);
    return false;
  }
  return true;
}

static bool create_level(openslide_t* osr,
                         TIFF* tiff,
                         GPtrArray* levels,
                         tdir_t ifd,
                         toff_t offset,
                         GError** err) {
  struct level* l = g_slice_new0(struct level);
  struct _openslide_tiff_level* tiffl = &l->tiffl;
  g_ptr_array_add(levels, l);

  // select and examine TIFF directory
  if (!_openslide_tiff_level_init(tiff, ifd,
                                  offset, (struct _openslide_level*)l,
                                  tiffl, err)) {
    return false;
  }

  // verify that we can read this compression (hard fail if not)
  if (!verify_compression(tiff, err)) {
    return false;
  }

  l->grid = _openslide_grid_create_simple(osr,
                                          tiffl->tiles_across,
                                          tiffl->tiles_down,
                                          tiffl->tile_w,
                                          tiffl->tile_h,
                                          read_tile);

  return true;
}

static bool create_levels_from_pixels(openslide_t* osr,
                                      TIFF* tiff,
                                      struct pixels* pixels,
                                      GPtrArray* levels,
                                      tdir_t* property_dir,
                                      GError** err) {
  *property_dir = -1;

  // find tiffdata with zero-valued first T, Z and C to obtain IFD
  // of full resolution plane
  struct tiffdata *full_res_plane = NULL;
  for (uint32_t tiffdata_num = 0; tiffdata_num < pixels->tiffdata->len;
       tiffdata_num++) {
    struct tiffdata* tiffdata = pixels->tiffdata->pdata[tiffdata_num];

    if (tiffdata->first_c == 0 && tiffdata->first_c == 0 &&
        tiffdata->first_z == 0) {
      full_res_plane = tiffdata;
      break;
    }
  }
  if (full_res_plane == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find full resolution plane");
    return false;
  }

  // create level for full resolution plane
  if (!create_level(osr, tiff, levels, (tdir_t)full_res_plane->ifd,
                   (toff_t)0, err)) {
    return false;
  }

  *property_dir = (tdir_t)full_res_plane->ifd;

  // create levels for pyramid levels (downsampled from full-resolution plane)
  toff_t *offsets;
  uint16_t count;
  if (TIFFGetField(tiff, TIFFTAG_SUBIFD, &count, &offsets)) {
    // local copy offsets to array as directory change invalidates 'offsets'
    GArray* offsets_array = g_array_sized_new(FALSE, FALSE, sizeof(toff_t), count);
    for (uint16_t offset_num = 0; offset_num < count; offset_num++) {
        g_array_append_val(offsets_array, offsets[offset_num]);
    }

    for (uint16_t offset_num = 0; offset_num < count; offset_num++) {
      if (!create_level(osr, tiff, levels, (tdir_t)0,
                        g_array_index(offsets_array, toff_t, offset_num), err)) {
        g_array_free(offsets_array, TRUE);
        return false;
      }
    }
    g_array_free(offsets_array, TRUE);
  }

  // sort tiled levels
  g_ptr_array_sort(levels, width_compare);

  return true;
}

static const char* store_string_property(struct _openslide_tifflike* tl,
                                         int64_t dir,
                                         openslide_t* osr,
                                         const char* name,
                                         int32_t tag) {
  const char* buf = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
  if (!buf) {
    return NULL;
  }
  char* value = g_strdup(buf);
  g_hash_table_insert(osr->properties, g_strdup(name), value);
  return value;
}

static void store_and_hash_string_property(struct _openslide_tifflike* tl,
                                           int64_t dir,
                                           openslide_t* osr,
                                           struct _openslide_hash* quickhash1,
                                           const char* name,
                                           int32_t tag) {
  _openslide_hash_string(quickhash1, name);
  _openslide_hash_string(quickhash1,
                         store_string_property(tl, dir, osr, name, tag));
}

static void store_float_property(struct _openslide_tifflike* tl,
                                 int64_t dir,
                                 openslide_t* osr,
                                 const char* name,
                                 int32_t tag) {
    GError* tmp_err = NULL;
    double value = _openslide_tifflike_get_float(tl, dir, tag, &tmp_err);
    if (!tmp_err) {
        g_hash_table_insert(osr->properties,
                            g_strdup(name),
                            _openslide_format_double(value));
    }
    g_clear_error(&tmp_err);
}

static void store_and_hash_properties(struct _openslide_tifflike* tl,
                                      int64_t dir,
                                      openslide_t* osr,
                                      struct _openslide_hash* quickhash1) {
  GError* tmp_err = NULL;

  // strings to store and hash
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Make", TIFFTAG_MAKE);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Model", TIFFTAG_MODEL);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Software", TIFFTAG_SOFTWARE);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.DateTime", TIFFTAG_DATETIME);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Artist", TIFFTAG_ARTIST);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.HostComputer", TIFFTAG_HOSTCOMPUTER);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.Copyright", TIFFTAG_COPYRIGHT);
  store_and_hash_string_property(tl, dir, osr, quickhash1,
                                 "tiff.DocumentName", TIFFTAG_DOCUMENTNAME);

  // don't hash floats, they might be unstable over time
  store_float_property(tl, dir, osr, "tiff.XResolution", TIFFTAG_XRESOLUTION);
  store_float_property(tl, dir, osr, "tiff.YResolution", TIFFTAG_YRESOLUTION);
  store_float_property(tl, dir, osr, "tiff.XPosition", TIFFTAG_XPOSITION);
  store_float_property(tl, dir, osr, "tiff.YPosition", TIFFTAG_YPOSITION);

  // special
  int64_t resolution_unit =
      _openslide_tifflike_get_uint(tl, dir, TIFFTAG_RESOLUTIONUNIT, &tmp_err);
  if (tmp_err) {
    resolution_unit = RESUNIT_INCH;  // default
    g_clear_error(&tmp_err);
  }
  const char* result;
    switch (resolution_unit) {
    case RESUNIT_NONE:
      result = "none";
      break;
    case RESUNIT_INCH:
      result = "inch";
      break;
    case RESUNIT_CENTIMETER:
      result = "centimeter";
      break;
    default:
      result = "unknown";
    }
    g_hash_table_insert(osr->properties,
                        g_strdup("tiff.ResolutionUnit"),
                        g_strdup(result));
}

static void set_resolution_prop(openslide_t* osr, TIFF* tiff,
                                const char* property_name,
                                ttag_t tag,
                                double fallback) {
  float f;
  uint16_t unit;
  bool inserted = false;

  if (TIFFGetFieldDefaulted(tiff, TIFFTAG_RESOLUTIONUNIT, &unit) &&
      TIFFGetField(tiff, tag, &f)) {
      if (unit == RESUNIT_CENTIMETER) {
        g_hash_table_insert(osr->properties, g_strdup(property_name),
                            _openslide_format_double(10000.0 / f));
        inserted = true;
      }
      else if (unit == RESUNIT_INCH)
      {
        g_hash_table_insert(osr->properties, g_strdup(property_name),
                            _openslide_format_double((25.4 * 1000.0) / f));
        inserted = true;
      }
  }

  if (!inserted && (fallback > 0.0)) {
    g_hash_table_insert(osr->properties, g_strdup(property_name),
                        _openslide_format_double(fallback));
  }
}

static bool ome_tiff_open(openslide_t *osr,
                          const char *filename,
                          struct _openslide_tifflike *tl,
                          struct _openslide_hash *quickhash1,
                          GError **err) {
  char* image_desc = NULL;
  GPtrArray *level_array = g_ptr_array_new();

  // open TIFF
  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  if (!tiff) {
    goto FAIL;
  }

  // get the xml description that contains the OME XML
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "Couldn't read ImageDescription");
    goto FAIL;
  }
  // create local copy of image description
  image_desc = g_strdup(image_desc);

  // read XML
  struct pixels* pixels = parse_xml_description(image_desc, err);
  if (!pixels) {
    goto FAIL;
  }

  // initialize and verify levels
  tdir_t property_dir;
  if (!create_levels_from_pixels(osr, tiff, pixels,
                                 level_array, &property_dir, err)) {
    pixels_free(pixels);
    goto FAIL;
  }

  // store and hash properties, overriding value of 'openslide.comment' and
  // 'tiff.ImageDescription' with OME XML in case full-resolution plane
  // is not in IFD 0
  store_and_hash_properties(tl, property_dir, osr, quickhash1);
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
                      g_strdup(image_desc));
  const char* tiffImageDescription = "tiff.ImageDescription";
  _openslide_hash_string(quickhash1, tiffImageDescription);
  _openslide_hash_string(quickhash1, image_desc);
  g_hash_table_insert(osr->properties,
                      g_strdup(tiffImageDescription), g_strdup(image_desc));

  // set MPP properties
  if (!_openslide_tiff_set_dir(tiff, property_dir, err)) {
    pixels_free(pixels);
    goto FAIL;
  }
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                      TIFFTAG_XRESOLUTION, pixels->mpp_x);
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                      TIFFTAG_YRESOLUTION, pixels->mpp_y);
  pixels_free(pixels);

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct ome_tiff_ops_data *data =
    g_slice_new0(struct ome_tiff_ops_data);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &ome_tiff_ops;

  // put TIFF handle and store tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  // free local copy of image description
  g_free(image_desc);

  return true;

FAIL:
  // free local copy of image description
  g_free(image_desc);

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
  return false;
}

const struct _openslide_format _openslide_format_ome_tiff = {
  .name = "ome-tiff",
  .vendor = "ome-tiff",
  .detect = ome_tiff_detect,
  .open = ome_tiff_open,
};
