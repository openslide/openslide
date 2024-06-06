/*
 * KFBio (kfb) support
 */

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"

#include <math.h>

static const char KFB_EXT[] = ".kfb";

struct image
{
  int64_t start_in_file;
  int32_t length;
  int32_t imageno; // used only for cache lookup
  int32_t width;
  int32_t height;
  int refcount;
};

struct tile
{
  struct image *image;
};

struct level
{
  struct _openslide_level base;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l)
{
  _openslide_grid_destroy(l->grid);
  g_free(l);
}

typedef struct level level;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(level, destroy_level)

static void destroy(openslide_t *osr)
{
  struct kfbio_ops_data *data = osr->data;

  // levels
  for (int32_t i = 0; i < osr->level_count; i++)
  {
    destroy_level((struct level *)osr->levels[i]);
  }
  g_free(osr->levels);

  // the ops data
  g_free(data);
}

struct kfbio_ops_data
{
  char *filename;
  int32_t tile_size;
};

static void image_unref(struct image *image)
{
  if (!--image->refcount)
  {
    g_free(image);
  }
}

typedef struct image image;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(image, image_unref)

static void tile_free(gpointer data)
{
  struct tile *tile = data;
  image_unref(tile->image);
  g_free(tile);
}

static uint32_t *read_image(openslide_t *osr,
                            struct image *image,
                            int w, int h,
                            GError **err)
{
  struct kfbio_ops_data *data = osr->data;
  bool result = false;

  g_autofree uint32_t *dest = g_malloc(w * h * 4);

  result = _openslide_jpeg_read_2(data->filename,
                                  image->start_in_file,
                                  image->length,
                                  dest, w, h,
                                  err);

  if (!result)
  {
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
                      GError **err)
{
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

  if (!tiledata)
  {
    tiledata = read_image(osr, tile->image, iw, ih, err);
    if (tiledata == NULL)
    {
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
      cairo_image_surface_create_for_data((unsigned char *)tiledata,
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
                         GError **err)
{
  struct level *l = (struct level *)level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops kfbio_ops = {
    .paint_region = paint_region,
    .destroy = destroy,
};

static bool kfbio_kfb_detect(const char *filename G_GNUC_UNUSED,
                             struct _openslide_tifflike *tl,
                             GError **err)
{
  // reject TIFFs
  if (tl)
  {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, KFB_EXT))
  {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", KFB_EXT);
    return false;
  }

  // verify existence
  GError *tmp_err = NULL;
  if (!_openslide_fexists(filename, &tmp_err))
  {
    if (tmp_err != NULL)
    {
      g_propagate_prefixed_error(err, tmp_err, "Testing whether file exists: ");
    }
    else
    {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "File does not exist");
    }
    return false;
  }

  return true;
}

static bool read_le_int32_from_file_with_result(struct _openslide_file *f,
                                                int32_t *OUT)
{
  if (!_openslide_fread_exact(f, OUT, 4, NULL))
  {
    return false;
  }

  *OUT = GINT32_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static int32_t read_le_int32_from_file(struct _openslide_file *f)
{
  int32_t i;

  if (!read_le_int32_from_file_with_result(f, &i))
  {
    // -1 means error
    i = -1;
  }

  return i;
}

static bool read_le_uint32_from_file_with_result(struct _openslide_file *f,
                                                 u_int32_t *OUT)
{
  if (!_openslide_fread_exact(f, OUT, 4, NULL))
  {
    return false;
  }

  *OUT = GUINT32_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static u_int32_t read_le_uint32_from_file(struct _openslide_file *f)
{
  u_int32_t i;

  if (!read_le_uint32_from_file_with_result(f, &i))
  {
    // -1 means error
    i = -1;
  }

  return i;
}

static bool read_le_int64_from_file_with_result(struct _openslide_file *f,
                                                int64_t *OUT)
{
  if (!_openslide_fread_exact(f, OUT, 8, NULL))
  {
    return false;
  }

  *OUT = GINT64_FROM_LE(*OUT);
  // g_debug("%d", i);

  return true;
}

static int64_t read_le_int64_from_file(struct _openslide_file *f)
{
  int64_t i;

  if (!read_le_int64_from_file_with_result(f, &i))
  {
    // -1 means error
    i = -1;
  }

  return i;
}

static void insert_tile(struct level *l,
                        struct image *image,
                        double pos_x, double pos_y,
                        int tile_x, int tile_y,
                        int tile_w, int tile_h,
                        int zoom_level)
{
  // increment image refcount
  image->refcount++;

  // generate tile
  struct tile *tile = g_new0(struct tile, 1);
  tile->image = image;

  // compute offset
  double offset_x = pos_x - (tile_x * l->base.tile_w);
  double offset_y = pos_y - (tile_y * l->base.tile_h);

  // insert
  _openslide_grid_tilemap_add_tile(l->grid,
                                   tile_x, tile_y,
                                   offset_x, offset_y,
                                   tile_w, tile_h,
                                   tile);

  if (!true)
  {
    g_debug("zoom %d, tile %d %d, pos %.10g %.10g, offset %.10g %.10g",
            zoom_level, tile_x, tile_y, pos_x, pos_y, offset_x, offset_y);
  }
}

static bool process_tiles_info_from_header(struct _openslide_file *f,
                                           int64_t seek_location,
                                           int zoom_levels,
                                           int total_tile_count,
                                           struct level **levels,
                                           GError **err)
{
  if (!_openslide_fseek(f, seek_location, SEEK_SET, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int32_t image_number = 0;
  int64_t u_int32_t_max_value_plus_1 = 4294967295 + 1;
  int zoom_level = -1;
  int32_t base_level_id = -1;
  // read all the data into the list
  for (int i = 0; i < total_tile_count; i++)
  {
    if (!_openslide_fseek(f, 4, SEEK_CUR, err))
    {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }
    // position in this level
    int32_t pos_x = read_le_int32_from_file(f);
    int32_t pos_y = read_le_int32_from_file(f);
    int32_t tile_w = read_le_int32_from_file(f);
    int32_t tile_h = read_le_int32_from_file(f);

    int32_t id = read_le_int32_from_file(f);
    if (i == 0)
      base_level_id = id;
    // TODO figure out why id and zoom_level has such mapping?
    zoom_level = (base_level_id - id) / 8388608;
    if (zoom_level < 0)
    {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "zoom level < 0");
      return false;
    } 
    else if (zoom_level>= zoom_levels)
    {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "zoom level >= zoom levels");
      return false;
    }

    struct level *l = levels[zoom_level];
    g_assert(tile_w <= l->base.tile_w);
    g_assert(tile_h <= l->base.tile_h);

    if (!_openslide_fseek(f, 8, SEEK_CUR, err))
    {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }

    int32_t length = read_le_int32_from_file(f);
    u_int32_t offset_from_file = read_le_uint32_from_file(f);
    int64_t offset = seek_location - u_int32_t_max_value_plus_1 + offset_from_file;
    if (!_openslide_fseek(f, 24, SEEK_CUR, err))
    {
      g_prefix_error(err, "Couldn't seek within header: ");
      return false;
    }

    if (offset < 0)
    {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "offset < 0");
      return false;
    }
    if (length < 0)
    {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "length < 0");
      return false;
    }

    // populate the image structure
    g_autoptr(image) image = g_new0(struct image, 1);
    image->start_in_file = offset;
    image->length = length;
    image->imageno = image_number++;
    image->refcount = 1;
    image->width = tile_w;
    image->height = tile_h;

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

static bool kfbio_kfb_open(openslide_t *osr, const char *filename,
                           struct _openslide_tifflike *tl G_GNUC_UNUSED,
                           struct _openslide_hash *quickhash1 G_GNUC_UNUSED, GError **err)
{
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f)
  {
    return false;
  }

  // read version
  if (!_openslide_fseek(f, 4, SEEK_SET, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  char buf[4];
  if (!_openslide_fread_exact(f, buf, sizeof(buf), err))
  {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read version within header");
    return false;
  }

  if (buf[0] != 'K' || buf[1] != 'F' || buf[2] != 'B')
  {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported file: %c%c%c", buf[0], buf[1], buf[2]);
    return false;
  }

  // add properties

  if (!_openslide_fseek(f, 8, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int32_t tile_count = read_le_int32_from_file(f);
  // read base dimensions
  int64_t base_h = read_le_int32_from_file(f);
  int64_t base_w = read_le_int32_from_file(f);
  // calculate level count
  int32_t zoom_levels = (int)ceil(log2(MAX(base_h, base_w))) + 1;
  double ScanScale = read_le_int32_from_file(f); // scanning scale factor e.g. 20X or 40X

  char jpgbuf[4];
  if (!_openslide_fread_exact(f, jpgbuf, sizeof(jpgbuf), err))
  {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read version within header");
    return false;
  }

  if (jpgbuf[0] != 'J' || jpgbuf[1] != 'P' || jpgbuf[2] != 'G')
  {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "file compression: %c%c%c", jpgbuf[0], jpgbuf[1], jpgbuf[2]);
    return false;
  }

  if (!_openslide_fseek(f, 4, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int32_t SpendTime = read_le_int32_from_file(f);
  int64_t ScanTime = read_le_int64_from_file(f);

  int32_t macro_info_in_file = read_le_int32_from_file(f);
  int32_t label_info_in_file = read_le_int32_from_file(f);
  int32_t preview_info_in_file = read_le_int32_from_file(f);

  if (!_openslide_fseek(f, 4, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int32_t tiles_info_in_file = read_le_int32_from_file(f);

  if (!_openslide_fseek(f, 4, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  // add properties
  g_hash_table_insert(osr->properties,
                      g_strdup("kfbio.SpendTime"),
                      _openslide_format_double(SpendTime));
  g_hash_table_insert(osr->properties,
                      g_strdup("kfbio.ScanTime"),
                      _openslide_format_double(ScanTime));

  unsigned char ImageCapResBuf[4];
  if (!_openslide_fread_exact(f, ImageCapResBuf, sizeof(ImageCapResBuf), err))
  {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ImageCapRes within header");
    return false;
  }
  // set MPP and objective power
  float* ImageCapRes = (float*)ImageCapResBuf;
  g_hash_table_insert(osr->properties,
                      g_strdup("kfbio.ScanScale"),
                      _openslide_format_double(ScanScale));
  g_hash_table_insert(osr->properties,
                      g_strdup("kfbio.ImageCapRes"),
                      _openslide_format_double(*ImageCapRes));
  g_hash_table_insert(osr->properties,
                      g_strdup("kfbio.ImageCapRes"),
                      _openslide_format_double(*ImageCapRes));

  _openslide_duplicate_double_prop(osr, "kfbio.ScanScale",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(osr, "kfbio.ImageCapRes",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "kfbio.ImageCapRes",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

  if (!_openslide_fseek(f, 8, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }

  int32_t tile_size = read_le_int32_from_file(f);
  // g_debug("tile size : %d\n", tile_size);

  // add associated images
  // read all associated image information entries
  if (!_openslide_fseek(f, macro_info_in_file, SEEK_SET, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  if (!_openslide_fseek(f, 8, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t macro_h = read_le_int32_from_file(f);
  g_assert(macro_h > 0);
  int32_t macro_w = read_le_int32_from_file(f);
  g_assert(macro_w > 0);
  if (!_openslide_fseek(f, 4, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t macro_length = read_le_int32_from_file(f);
  g_assert(macro_length > 0);
  if (!_openslide_fseek(f, 28, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int64_t macro_data_in_file = macro_info_in_file + 52;
  if (!_openslide_jpeg_add_associated_image(osr, "macro", filename, macro_data_in_file, err))
  {
    g_prefix_error(err, "Couldn't read associated image: %s", "macro");
    return false;
  }

  if (!_openslide_fseek(f, label_info_in_file, SEEK_SET, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  if (!_openslide_fseek(f, 8, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t label_h = read_le_int32_from_file(f);
  g_assert(label_h > 0);
  int32_t label_w = read_le_int32_from_file(f);
  g_assert(label_w > 0);
  if (!_openslide_fseek(f, 4, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t label_length = read_le_int32_from_file(f);
  g_assert(label_length > 0);
  if (!_openslide_fseek(f, 28, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int64_t label_data_in_file = label_info_in_file + 52;
  if (!_openslide_jpeg_add_associated_image(osr, "label", filename, label_data_in_file, err))
  {
    g_prefix_error(err, "Couldn't read associated image: %s", "label");
    return false;
  }

  if (!_openslide_fseek(f, preview_info_in_file, SEEK_SET, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  if (!_openslide_fseek(f, 8, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t preview_h = read_le_int32_from_file(f);
  g_assert(preview_h > 0);
  int32_t preview_w = read_le_int32_from_file(f);
  g_assert(preview_w > 0);
  if (!_openslide_fseek(f, 4, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int32_t preview_length = read_le_int32_from_file(f);
  g_assert(preview_length > 0);
  if (!_openslide_fseek(f, 28, SEEK_CUR, err))
  {
    g_prefix_error(err, "Couldn't seek within header: ");
    return false;
  }
  int64_t preview_data_in_file = preview_info_in_file + 52;
  if (!_openslide_jpeg_add_associated_image(osr, "preview", filename, preview_data_in_file, err))
  {
    g_prefix_error(err, "Couldn't read associated image: %s", "preview");
    return false;
  }

  // set up level dimensions and such
  g_autoptr(GPtrArray) level_array =
      g_ptr_array_new_with_free_func((GDestroyNotify)destroy_level);
  int64_t downsample = 1;
  for (int i = 0; i < zoom_levels; i++)
  {
    // ensure downsample is > 0 and a power of 2
    if (downsample <= 0 || (downsample & (downsample - 1)))
    {
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
    l->base.tile_w = (double)tile_size;
    l->base.tile_h = (double)tile_size;

    l->base.w = base_w / l->base.downsample;
    if (l->base.w == 0)
      l->base.w = 1;
    l->base.h = base_h / l->base.downsample;
    if (l->base.h == 0)
      l->base.h = 1;

    l->grid = _openslide_grid_create_tilemap(osr,
                                             tile_size,
                                             tile_size,
                                             read_tile, tile_free);
  }

  // load the position map and build up the tiles
  if (!process_tiles_info_from_header(f,
                                      tiles_info_in_file,
                                      zoom_levels,
                                      tile_count,
                                      (struct level **)level_array->pdata,
                                      err))
  {
    return false;
  }

  for (int i = 0; i < zoom_levels; i++)
  {
    struct level *l = level_array->pdata[i];
    if (!l->base.tile_w || !l->base.tile_h)
    {
      // invalidate
      for (i = 0; i < zoom_levels; i++)
      {
        struct level *l = level_array->pdata[i];
        l->base.tile_w = 0;
        l->base.tile_h = 0;
      }
      break;
    }
  }

  // build ops data
  struct kfbio_ops_data *data = g_new0(struct kfbio_ops_data, 1);
  data->filename = g_strdup(filename);
  data->tile_size = tile_size;

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = zoom_levels;
  osr->levels = (struct _openslide_level **)
      g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &kfbio_ops;

  return true;
}

const struct _openslide_format _openslide_format_kfbio = {
    .name = "kfbio-kfb",
    .vendor = "kfbio",
    .detect = kfbio_kfb_detect,
    .open = kfbio_kfb_open,
};