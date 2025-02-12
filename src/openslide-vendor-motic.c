/*
 * Motic (mdsx) support
 */

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <math.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

static const char MDSX_EXT[] = ".mdsx";

struct motic_ops_data {
  char *filename;
};

struct image {
  int64_t start_in_file;
  int32_t length;
  int32_t imageno; // used only for cache lookup
  int32_t width;
  int32_t height;
  int refcount;
};

struct tile {
  struct image *image;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  int32_t tiles_across;
  int32_t tiles_down;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr) {
  struct motic_ops_data *data = osr->data;

  // levels
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
  }
  g_free(osr->levels);

  // the ops data
  g_free(data->filename);
  g_free(data);
}

static void image_unref(struct image *image) {
  if (!--image->refcount) {
    g_free(image);
  }
}

typedef struct image image;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(image, image_unref)

static void tile_free(gpointer data) {
  struct tile *tile = data;
  image_unref(tile->image);
  g_free(tile);
}

static uint32_t *read_image(openslide_t *osr,
                            struct image *image,
                            int w, int h,
                            GError **err) {
  struct motic_ops_data *data = osr->data;
  bool result = false;

  g_autofree uint32_t *dest = g_malloc(w * h * 4);

  g_autoptr(_openslide_file) f = _openslide_fopen(data->filename, err);
  if (f == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File is NULL");
    return NULL;
  }

  if (image->length == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Length is zero");
    return NULL;
  }

  if (image->start_in_file && !_openslide_fseek(f, image->start_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Cannot seek to offset: ");
    return NULL;
  }

  char buf[image->length];
  if (_openslide_fread(f, buf, sizeof(buf), err) != sizeof(buf)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read tile data");
    return NULL;
  }

  result = _openslide_jpeg_decode_buffer(buf,
                                        image->length,
                                        dest,
                                        w, h,
                                        err);

  if (!result) {
    return NULL;
  }
  return g_steal_pointer(&dest);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col G_GNUC_UNUSED,
                      int64_t tile_row G_GNUC_UNUSED,
                      void *data,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {
  struct tile *tile = data;
  bool success = true;

  int iw = tile->image->width;
  int ih = tile->image->height;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level,
                                            tile->image->imageno,
                                            0,
                                            &cache_entry);

  if (!tiledata) {
    tiledata = read_image(osr, tile->image, iw, ih, err);
    if (tiledata == NULL) {
      return false;
    }
    _openslide_cache_put(osr->cache,
                         level, tile->image->imageno, 0,
                         tiledata,
                         iw * ih * 4,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_RGB24,
                                        iw, ih, iw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return success;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct level *l = (struct level *) level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops motic_ops = {
    .paint_region = paint_region,
    .destroy = destroy,
};

static bool motic_mdsx_detect(const char *filename G_GNUC_UNUSED,
                              struct _openslide_tifflike *tl,
                              GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, MDSX_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", MDSX_EXT);
    return false;
  }

  // verify existence
  GError *tmp_err = NULL;
  if (!_openslide_fexists(filename, &tmp_err)) {
    if (tmp_err != NULL) {
      g_propagate_prefixed_error(err, tmp_err, "Testing whether file exists: ");
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "File does not exist");
    }
    return false;
  }

  return true;
}

static char *read_string_from_file(struct _openslide_file *f, int len) {
  g_autofree char *str = g_malloc(len + 1);
  str[len] = '\0';

  if (!_openslide_fread_exact(f, str, len, NULL)) {
    return NULL;
  }
  return g_steal_pointer(&str);
}

static bool read_le_int32_from_file_with_result(struct _openslide_file *f,
                                                int32_t *OUT) {
  if (!_openslide_fread_exact(f, OUT, 4, NULL)) {
    return false;
  }

  *OUT = GINT32_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static int32_t read_le_int32_from_file(struct _openslide_file *f) {
  int32_t i;

  if (!read_le_int32_from_file_with_result(f, &i)) {
    // -1 means error
    i = -1;
  }

  return i;
}

static int get_length_without_trailing_zeros(char *in, int inlen) {
	int outlen = 0;
	while (inlen > 0) {
		if (*in == 0 && *(in+1) == 0)
			return outlen;
		outlen+=2;
		inlen-=2;
		in+=2;
	}
	return outlen;
}

static void insert_tile(struct level *l,
                        struct image *image,
                        double pos_x, double pos_y,
                        int tile_x, int tile_y,
                        int tile_w, int tile_h,
                        int zoom_level) {
  // increment image refcount
  image->refcount++;

  // generate tile
  struct tile *tile = g_new0(struct tile, 1);
  tile->image = image;

  // compute offset
  double offset_x = pos_x - (tile_x * l->base.tile_w);
  double offset_y = pos_y - (tile_y * l->base.tile_h);

  // printf("pos_x: %f pos_y: %f offset_x: %f offset_y: %f tile_x: %d tile_y: %d \n", pos_x, pos_y, offset_x, offset_y, tile_x, tile_y);

  // insert
  _openslide_grid_tilemap_add_tile(l->grid,
                                   tile_x, tile_y,
                                   offset_x, offset_y,
                                   tile_w, tile_h,
                                   tile);

  if (!true) {
    g_debug("zoom %d, tile %d %d, pos %.10g %.10g, offset %.10g %.10g",
            zoom_level, tile_x, tile_y, pos_x, pos_y, offset_x, offset_y);
  }
}

static bool process_tiles_info_from_header(struct _openslide_file *f,
                                           int64_t seek_location,
                                           int zoom_level,
                                           int tile_count,
                                           int32_t *image_number,
                                           struct level *l,
                                           GError **err) {
  if (!_openslide_fseek(f, seek_location + 4, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  // int32_t image_number = 0;

  // read all the data into the list
  for (int i = 0; i < tile_count; i++) {
    if (!_openslide_fseek(f, 2, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    int32_t offset = read_le_int32_from_file(f);
    int32_t length = read_le_int32_from_file(f);

    if (offset < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "offset < 0");
      return false;
    }
    if (length < 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "length < 0");
      return false;
    }

    int64_t tile_w = l->base.tile_w;
    int64_t tile_h = l->base.tile_h;

    // position in this level
    int32_t tile_col = i % l->tiles_across;
    int32_t tile_row = i / l->tiles_across;
    int32_t pos_x = tile_w * tile_col;
    int32_t pos_y = tile_h * tile_row;

    // populate the image structure
    g_autoptr(image) image = g_new0(struct image, 1);
    image->start_in_file = offset;
    image->length = length;
    image->imageno = (*image_number)++;
    image->refcount = 1;
    image->width = tile_w;
    image->height = tile_h;

    // printf("zoom_level : %d ~ i : %d  ~ tile_col : %d ~ tile_row : %d ~ pos_x : %d ~ pos_y : %d\n", zoom_level, i, tile_col, tile_row, pos_x, pos_y);

    // start processing 1 image into 1 tile
    // increments image refcount
    insert_tile(l, image,
                pos_x, pos_y,
                pos_x / l->base.tile_w,
                pos_y / l->base.tile_h,
                tile_w, tile_h,
                zoom_level);
  }

  return true;
}

static bool parse_slide_image_xml(openslide_t *osr,
                                  const char *xml, int length, GError **err) {
  // try to parse the xml
  g_autoptr(xmlDoc) slide_image_doc = _openslide_xml_parse(xml, length, err);
  if (!slide_image_doc) {
    return false;
  }

  xmlNode *slide_image_root = xmlDocGetRootElement(slide_image_doc);
  if (!slide_image_root) {
    return false;
  }

  // ImageMatrix
  for (xmlNode *image_matrix_node = slide_image_root->last->children; image_matrix_node; image_matrix_node = image_matrix_node->next) {
    if (image_matrix_node->type == XML_ELEMENT_NODE) {
      g_autoptr(xmlChar) pvalue = xmlGetProp(image_matrix_node, BAD_CAST "value");
      if (pvalue && *pvalue) {
        const char *value = (const char *) pvalue;
        g_hash_table_insert(osr->properties,
                            g_strdup_printf("motic.%s", image_matrix_node->name),
                            g_strdup((char *) value));
      } else {
        for (xmlNode *layer_node = image_matrix_node->children; layer_node; layer_node = layer_node->next) {
          if (image_matrix_node->type == XML_ELEMENT_NODE) {
            g_autoptr(xmlChar) pvalue = xmlGetProp(layer_node, BAD_CAST "value");
            if (pvalue && *pvalue) {
              const char *value = (const char *) pvalue;
              g_hash_table_insert(osr->properties,
                                  g_strdup_printf("motic.%s.%s", image_matrix_node->name, layer_node->name),
                                  g_strdup((char *) value));
            }
          }
        }
      }
    }
  }

  return true;
}

static bool process_slide_image_xml_from_base64(openslide_t *osr, struct _openslide_file *f,
                                                int32_t seek_location, int32_t len, GError **err) {
  if (!_openslide_fseek(f, seek_location, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  g_autofree char *slide_image_xml_base64 = read_string_from_file(f, len);
  gsize slide_image_xml_length;
  g_autofree guchar *slide_image_xml = g_base64_decode(slide_image_xml_base64, &slide_image_xml_length);
  if (!slide_image_xml) {
    return false;
  }

  return parse_slide_image_xml(osr, (const char *) slide_image_xml, (int) slide_image_xml_length, err);
}

static bool process_slide_image_xml(openslide_t *osr, struct _openslide_file *f,
                                    int32_t seek_location, int32_t len, GError **err) {
  if (!_openslide_fseek(f, seek_location, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  g_autofree char *slide_image_xml = read_string_from_file(f, len);
  len = get_length_without_trailing_zeros(slide_image_xml, len);

  return parse_slide_image_xml(osr, (const char *) slide_image_xml, len, err);
}

static bool parse_property_xml(openslide_t *osr,
                               const char *xml, int length, GError **err) {
  // try to parse the xml
  g_autoptr(xmlDoc) property_doc = _openslide_xml_parse(xml, length, err);
  if (!property_doc) {
    return false;
  }

  xmlNode *property_root = xmlDocGetRootElement(property_doc);
  if (!property_root) {
    return false;
  }

  // copy all motic attributes to vendor properties
  for (xmlNode *property_node = property_root->children; property_node; property_node = property_node->next) {
    if (property_node->type == XML_ELEMENT_NODE) {
      g_autoptr(xmlChar) value = xmlGetProp(property_node, BAD_CAST "value");
      if (value && *value) {
        g_hash_table_insert(osr->properties,
                            g_strdup_printf("motic.%s", property_node->name),
                            g_strdup((char *) value));
      }
    }
  }

  return true;
}

static bool process_property_xml_from_base64(openslide_t *osr, struct _openslide_file *f,
                                             int64_t seek_location,
                                             int len, GError **err) {
  if (!_openslide_fseek(f, seek_location, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  g_autofree char *property_xml_base64 = read_string_from_file(f, len);
  gsize property_xml_length;
  g_autofree guchar *property_xml = g_base64_decode(property_xml_base64, &property_xml_length);
  if (!property_xml) {
    return false;
  }

  return parse_property_xml(osr, (const char *) property_xml, (int) property_xml_length, err);
}

static bool process_property_xml(openslide_t *osr, struct _openslide_file *f,
                                 int64_t seek_location,
                                 int len, GError **err) {
  if (!_openslide_fseek(f, seek_location, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  g_autofree char *property_xml = read_string_from_file(f, len);
  len = get_length_without_trailing_zeros(property_xml, len);

  return parse_property_xml(osr, (const char *) property_xml, len, err);
}

static bool motic_mdsx_open(openslide_t *osr, const char *filename,
                            struct _openslide_tifflike *tl G_GNUC_UNUSED,
                            struct _openslide_hash *quickhash1 G_GNUC_UNUSED, GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  // Check MDSX marker
  char buf[4];
  if (!_openslide_fread_exact(f, buf, sizeof(buf), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read version within header");
    return false;
  }

  if (buf[0] != 'B' || buf[1] != 'K' || buf[2] != 'I' || buf[3] != 'O') {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported file: %c%c%c%c", buf[0], buf[1], buf[2], buf[3]);
    return false;
  }

  // add properties
  if (!_openslide_fseek(f, 80, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int32_t associated_images_info_in_file = -1;
  for (int i = 0; i < 5; i++) {
    // skip marker 64, 65, 66, 67, 68 ??
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    int32_t in_file = read_le_int32_from_file(f);
    if (i == 0)
      associated_images_info_in_file = in_file;
    // skip marker
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
  }

  if (!_openslide_fseek(f, associated_images_info_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  if (!_openslide_fseek(f, 6, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  // skip slide xml for now
  if (!_openslide_fseek(f, 14, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t property_xml_base64_in_file = read_le_int32_from_file(f);
  int32_t property_xml_base64_length = read_le_int32_from_file(f);
  if (!_openslide_fseek(f, 6, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t preview_data_in_file = read_le_int32_from_file(f);
  int32_t preview_length = read_le_int32_from_file(f);
  g_assert(preview_length > 0);
  if (!_openslide_fseek(f, 6, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t label_data_in_file = read_le_int32_from_file(f);
  int32_t label_length = read_le_int32_from_file(f);
  g_assert(label_length > 0);
  if (!_openslide_fseek(f, 6, SEEK_CUR, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t slide_image_xml_base64_in_file = read_le_int32_from_file(f);
  int32_t slide_image_xml_base64_length = read_le_int32_from_file(f);

  if (!_openslide_fseek(f, slide_image_xml_base64_in_file, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  char xml[1];
  if (!_openslide_fread_exact(f, xml, sizeof(xml), err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read version within header");
    return false;
  }

  // check if xml is encoded as base64 string
  // if xml starts with '<', then it is not encoded
  bool xml_is_base64 = xml[0] != '<';
  if (xml_is_base64){
    // read slide image XML in base64
    if (!process_slide_image_xml_from_base64(osr, f, slide_image_xml_base64_in_file, slide_image_xml_base64_length, err)) {
      return false;
    }

    // read property XML in base64
    if (!process_property_xml_from_base64(osr, f, property_xml_base64_in_file, property_xml_base64_length, err)) {
      return false;
    }
  } else{
    // read slide image XML
    if (!process_slide_image_xml(osr, f, slide_image_xml_base64_in_file, slide_image_xml_base64_length, err)) {
      return false;
    }

    // read property XML
    if (!process_property_xml(osr, f, property_xml_base64_in_file, property_xml_base64_length, err)) {
      return false;
    }
  }

  char *bg_str = g_hash_table_lookup(osr->properties, "motic.BackgroundColor");
  int64_t bg;
  if (_openslide_parse_int64(bg_str, &bg)) {
    _openslide_set_background_color_prop(osr,
                                         (bg >> 16) & 0xFF,
                                         (bg >> 8) & 0xFF,
                                         bg & 0xFF);
  }

  _openslide_duplicate_double_prop(osr, "motic.ScanObjective",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(osr, "motic.Scale",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "motic.Scale",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

  // set base dimensions
  char *width_str = g_hash_table_lookup(osr->properties, "motic.Width");
  char *height_str = g_hash_table_lookup(osr->properties, "motic.Height");
  int64_t base_w = 0;
  int64_t base_h = 0;
  if (!_openslide_parse_int64(width_str, &base_w)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid width");
    return false;
  }
  if (!_openslide_parse_int64(height_str, &base_h)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid height");
    return false;
  }

  char *layer_count_str = g_hash_table_lookup(osr->properties, "motic.LayerCount");
  int64_t layer_count = 0;
  if (!_openslide_parse_int64(layer_count_str, &layer_count)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid layer count");
    return false;
  }
  int32_t zoom_levels = layer_count;

  char *cell_width_str = g_hash_table_lookup(osr->properties, "motic.CellWidth");
  char *cell_height_str = g_hash_table_lookup(osr->properties, "motic.CellHeight");
  int64_t cell_width = 0;
  int64_t cell_height = 0;
  if (!_openslide_parse_int64(cell_width_str, &cell_width)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid cell width");
    return false;
  }
  if (!_openslide_parse_int64(cell_height_str, &cell_height)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid cell height");
    return false;
  }
  g_assert(cell_width == cell_height);

  int32_t tile_size = cell_width;

  // add associated images
  if (label_data_in_file > 0) {
    if (!_openslide_jpeg_add_associated_image(osr, "label", filename, label_data_in_file, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", "label");
      return false;
    }
  }

  if (preview_data_in_file > 0) {
    if (!_openslide_jpeg_add_associated_image(osr, "preview", filename, preview_data_in_file, err)) {
      g_prefix_error(err, "Couldn't read associated image: %s", "preview");
      return false;
    }
  }

  // set up level dimensions and such
  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_level);
  int64_t downsample = 1;
  for (int i = 0; i < zoom_levels; i++) {
    // ensure downsample is > 0 and a power of 2
    if (downsample <= 0 || (downsample & (downsample - 1))) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Invalid downsample %" PRId64, downsample);
      return false;
    }

    if (i == 0)
      downsample = 1;
    else
      downsample *= 2;

    struct level *l = g_new0(struct level, 1);
    g_ptr_array_add(level_array, l);

    l->base.downsample = downsample;
    l->base.tile_w = (double) tile_size;
    l->base.tile_h = (double) tile_size;

    l->base.w = base_w / l->base.downsample;
    if (l->base.w == 0)
      l->base.w = 1;
    l->base.h = base_h / l->base.downsample;
    if (l->base.h == 0)
      l->base.h = 1;

    char tile_rows_key[] = "motic.Layer0.Rows";
    tile_rows_key[11] = (char) ('0' + i);
    char tile_cols_key[] = "motic.Layer0.Cols";
    tile_cols_key[11] = (char) ('0' + i);
    char *tile_rows_str = g_hash_table_lookup(osr->properties, tile_rows_key);
    char *tile_cols_str = g_hash_table_lookup(osr->properties, tile_cols_key);
    int64_t tile_rows = 0;
    int64_t tile_cols = 0;
    if (!_openslide_parse_int64(tile_rows_str, &tile_rows)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Invalid tile rows");
      return false;
    }
    if (!_openslide_parse_int64(tile_cols_str, &tile_cols)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Invalid tile cols");
      return false;
    }
    g_assert(tile_rows >= 1);
    g_assert(tile_cols >= 1);
    l->tiles_across = tile_cols;
    l->tiles_down = tile_rows;

    l->grid = _openslide_grid_create_tilemap(osr,
                                             tile_size,
                                             tile_size,
                                             read_tile, tile_free);
  }

  int32_t tiles_info_in_file = 164;

  // verify level count
  g_assert((associated_images_info_in_file - tiles_info_in_file) / 16 == zoom_levels);
  int32_t image_number = 0;
  for (int zoom_level = 0; zoom_level < zoom_levels; zoom_level++) {
    if (!_openslide_fseek(f, tiles_info_in_file + zoom_level * 16, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    // skip marker 6900, 6901, ... , 6907, 6908 ??
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    if (!_openslide_fseek(f, 4, SEEK_CUR, err)) {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    int32_t seek_location = read_le_int32_from_file(f); // tiles_info_in_file in current level
    int32_t tile_count = (read_le_int32_from_file(f) - 4) / 10; // tile count in current level
    struct level *l = level_array->pdata[zoom_level];
    g_assert(tile_count == l->tiles_across * l->tiles_down);
    // load the position map and build up the tiles
    if (!process_tiles_info_from_header(f,
                                        seek_location,
                                        zoom_level,
                                        tile_count,
                                        &image_number,
                                        l,
                                        err)) {
      return false;
    }
  }

  // build ops data
  struct motic_ops_data *data = g_new0(struct motic_ops_data, 1);
  data->filename = g_strdup(filename);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = zoom_levels;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &motic_ops;

  return true;
}

const struct _openslide_format _openslide_format_motic = {
  .name = "motic-mdsx",
  .vendor = "motic",
  .detect = motic_mdsx_detect,
  .open = motic_mdsx_open,
};
