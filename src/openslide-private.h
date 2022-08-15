/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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

#ifndef OPENSLIDE_OPENSLIDE_PRIVATE_H_
#define OPENSLIDE_OPENSLIDE_PRIVATE_H_

#include <config.h>

#include "openslide.h"
#include "openslide-hash.h"

#include <glib.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include <cairo.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(cairo_t, cairo_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(cairo_surface_t, cairo_surface_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(openslide_t, openslide_close)

/* the associated image structure */
struct _openslide_associated_image {
  const struct _openslide_associated_image_ops *ops;

  int64_t w;
  int64_t h;
};

/* associated image operations */
struct _openslide_associated_image_ops {
  // must fail if stored width or height doesn't match the image
  bool (*get_argb_data)(struct _openslide_associated_image *img,
                        uint32_t *dest,
                        GError **err);
  void (*destroy)(struct _openslide_associated_image *img);
};

/* the main structure */
struct _openslide {
  const struct _openslide_ops *ops;
  struct _openslide_level **levels;
  void *data;
  int32_t level_count;

  // associated images
  GHashTable *associated_images;  // created automatically
  const char **associated_image_names; // filled in automatically from hashtable

  // metadata
  GHashTable *properties; // created automatically
  const char **property_names; // filled in automatically from hashtable

  // cache
  struct _openslide_cache_binding *cache;

  // error handling, NULL if no error
  gpointer error; // must use g_atomic_pointer!
};

struct _openslide_level {
  double downsample;  // zero value is filled in automatically from dimensions

  int64_t w;
  int64_t h;

  // only for tile geometry properties; 0 to omit.
  // all levels must set these, or none
  int64_t tile_w;
  int64_t tile_h;
};

/* the function pointer structure for backends */
struct _openslide_ops {
  bool (*paint_region)(openslide_t *osr, cairo_t *cr,
		       int64_t x, int64_t y,
		       struct _openslide_level *level,
		       int32_t w, int32_t h,
		       GError **err);
  void (*destroy)(openslide_t *osr);
};

struct _openslide_tifflike;

/* vendor detection and parsing */

/*
 * A note on quickhash1: this should be a hash of data that
 * will not change with revisions to the openslide library. It should
 * also be quick to generate. It should be a way to uniquely identify
 * a particular slide by content, but does not need to be sensitive
 * to file corruption.
 *
 * It is called "quickhash1" so that we can create a "quickhash2" if needed.
 * The hash is stored in a property, it is expected that we will store
 * more hash properties if needed.
 *
 * Suggested data to hash:
 * easily available image metadata + raw compressed lowest resolution image
 */
struct _openslide_format {
  const char *name;
  const char *vendor;
  bool (*detect)(const char *filename, struct _openslide_tifflike *tl,
                 GError **err);
  bool (*open)(openslide_t *osr, const char *filename,
               struct _openslide_tifflike *tl,
               struct _openslide_hash *quickhash1, GError **err);
};

extern const struct _openslide_format _openslide_format_aperio;
extern const struct _openslide_format _openslide_format_generic_tiff;
extern const struct _openslide_format _openslide_format_hamamatsu_ndpi;
extern const struct _openslide_format _openslide_format_hamamatsu_vms_vmu;
extern const struct _openslide_format _openslide_format_leica;
extern const struct _openslide_format _openslide_format_mirax;
extern const struct _openslide_format _openslide_format_philips;
extern const struct _openslide_format _openslide_format_sakura;
extern const struct _openslide_format _openslide_format_synthetic;
extern const struct _openslide_format _openslide_format_trestle;
extern const struct _openslide_format _openslide_format_ventana;

/* GHashTable utils */
void _openslide_int64_free(gpointer data);

/* g_key_file_new() + g_key_file_load_from_file() wrapper */
GKeyFile *_openslide_read_key_file(const char *filename, int32_t max_size,
                                   GKeyFileFlags flags, GError **err);

void *_openslide_inflate_buffer(const void *src, int64_t src_len,
                                int64_t dst_len,
                                GError **err);

/* Parse string to double, returning NAN on failure.  Accept both comma
   and period as decimal separator. */
double _openslide_parse_double(const char *value);

/* Serialize double to string */
char *_openslide_format_double(double d);

/* Duplicate OpenSlide properties */
void _openslide_duplicate_int_prop(openslide_t *osr, const char *src,
                                   const char *dest);
void _openslide_duplicate_double_prop(openslide_t *osr, const char *src,
                                      const char *dest);

// background color helper
void _openslide_set_background_color_prop(openslide_t *osr,
                                          uint8_t r, uint8_t g, uint8_t b);

// clip right/bottom edges of tile
bool _openslide_clip_tile(uint32_t *tiledata,
                          int64_t tile_w, int64_t tile_h,
                          int64_t clip_w, int64_t clip_h,
                          GError **err);


// Slice allocator wrapper for g_auto
struct _openslide_slice {
  void *p;
  gsize len;
};

struct _openslide_slice _openslide_slice_alloc(gsize len);

void *_openslide_slice_steal(struct _openslide_slice *box);

void _openslide_slice_free(struct _openslide_slice *box);

typedef struct _openslide_slice _openslide_slice;
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(_openslide_slice, _openslide_slice_free)


// File handling
struct _openslide_file;

struct _openslide_file *_openslide_fopen(const char *path, GError **err);
size_t _openslide_fread(struct _openslide_file *file, void *buf, size_t size);
bool _openslide_fseek(struct _openslide_file *file, off_t offset, int whence,
                      GError **err);
off_t _openslide_ftell(struct _openslide_file *file, GError **err);
off_t _openslide_fsize(struct _openslide_file *file, GError **err);
void _openslide_fclose(struct _openslide_file *file);
bool _openslide_fexists(const char *path, GError **err);

typedef struct _openslide_file _openslide_file;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(_openslide_file, _openslide_fclose)


// Grid helpers
struct _openslide_grid;

typedef bool (*_openslide_grid_simple_read_fn)(openslide_t *osr,
                                               cairo_t *cr,
                                               struct _openslide_level *level,
                                               int64_t tile_col, int64_t tile_row,
                                               void *arg,
                                               GError **err);

typedef bool (*_openslide_grid_tilemap_read_fn)(openslide_t *osr,
                                                cairo_t *cr,
                                                struct _openslide_level *level,
                                                int64_t tile_col, int64_t tile_row,
                                                void *tile,
                                                void *arg,
                                                GError **err);

typedef bool (*_openslide_grid_range_read_fn)(openslide_t *osr,
                                              cairo_t *cr,
                                              struct _openslide_level *level,
                                              int64_t tile_unique_id,
                                              void *tile,
                                              void *arg,
                                              GError **err);

struct _openslide_grid *_openslide_grid_create_simple(openslide_t *osr,
                                                      int64_t tiles_across,
                                                      int64_t tiles_down,
                                                      int32_t tile_w,
                                                      int32_t tile_h,
                                                      _openslide_grid_simple_read_fn read_tile);

struct _openslide_grid *_openslide_grid_create_tilemap(openslide_t *osr,
                                                       double tile_advance_x,
                                                       double tile_advance_y,
                                                       _openslide_grid_tilemap_read_fn read_tile,
                                                       GDestroyNotify destroy_tile);

void _openslide_grid_tilemap_add_tile(struct _openslide_grid *grid,
                                      int64_t col, int64_t row,
                                      double offset_x, double offset_y,
                                      double w, double h,
                                      void *data);

struct _openslide_grid *_openslide_grid_create_range(openslide_t *osr,
                                                     int typical_tile_width,
                                                     int typical_tile_height,
                                                     _openslide_grid_range_read_fn read_tile,
                                                     GDestroyNotify destroy_tile);

void _openslide_grid_range_add_tile(struct _openslide_grid *_grid,
                                    double x, double y,
                                    double w, double h,
                                    void *data);

void _openslide_grid_range_finish_adding_tiles(struct _openslide_grid *_grid);

void _openslide_grid_get_bounds(struct _openslide_grid *grid,
                                double *x, double *y,
                                double *w, double *h);

bool _openslide_grid_paint_region(struct _openslide_grid *grid,
                                  cairo_t *cr,
                                  void *arg,
                                  double x, double y,
                                  struct _openslide_level *level,
                                  int32_t w, int32_t h,
                                  GError **err);

void _openslide_grid_draw_tile_info(cairo_t *cr, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

void _openslide_grid_destroy(struct _openslide_grid *grid);


/* Bounds properties helper */
void _openslide_set_bounds_props_from_grid(openslide_t *osr,
                                           struct _openslide_grid *grid);


/* Cache */
struct _openslide_cache_binding;
struct _openslide_cache_entry;

// create/release
openslide_cache_t *_openslide_cache_create(uint64_t capacity_in_bytes);

void _openslide_cache_release(openslide_cache_t *cache);

// binding a cache to an openslide_t
struct _openslide_cache_binding *_openslide_cache_binding_create(void);

void _openslide_cache_binding_set(struct _openslide_cache_binding *cb,
                                  openslide_cache_t *cache);

void _openslide_cache_binding_destroy(struct _openslide_cache_binding *cb);

// put and get
void _openslide_cache_put(struct _openslide_cache_binding *cb,
			  void *plane,  // coordinate plane (level or grid)
			  int64_t x,
			  int64_t y,
			  void *data,
			  uint64_t size_in_bytes,
			  struct _openslide_cache_entry **entry);

void *_openslide_cache_get(struct _openslide_cache_binding *cb,
			   void *plane,
			   int64_t x,
			   int64_t y,
			   struct _openslide_cache_entry **entry);

// value unref
void _openslide_cache_entry_unref(struct _openslide_cache_entry *entry);

typedef struct _openslide_cache_entry _openslide_cache_entry;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(_openslide_cache_entry,
                              _openslide_cache_entry_unref)


/* Internal error propagation */
enum OpenSlideError {
  // generic failure
  OPENSLIDE_ERROR_FAILED,
  // cairo error
  OPENSLIDE_ERROR_CAIRO_ERROR,
  // no such value (e.g. for tifflike accessors)
  OPENSLIDE_ERROR_NO_VALUE,
};
#define OPENSLIDE_ERROR _openslide_error_quark()
GQuark _openslide_error_quark(void);

bool _openslide_check_cairo_status(cairo_t *cr, GError **err);

/* Debug flags */
enum _openslide_debug_flag {
  OPENSLIDE_DEBUG_DETECTION,
  OPENSLIDE_DEBUG_JPEG_MARKERS,
  OPENSLIDE_DEBUG_PERFORMANCE,
  OPENSLIDE_DEBUG_SQL,
  OPENSLIDE_DEBUG_SYNTHETIC,
  OPENSLIDE_DEBUG_TILES,
};

void _openslide_debug_init(void);

bool _openslide_debug(enum _openslide_debug_flag flag);

#define _openslide_performance_warn(...) \
      _openslide_performance_warn_once(NULL, __VA_ARGS__)

void _openslide_performance_warn_once(gint *warned_flag,
                                      const char *str, ...)
                                      G_GNUC_PRINTF(2, 3);

// private properties, for now
#define _OPENSLIDE_PROPERTY_NAME_LEVEL_COUNT "openslide.level-count"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_WIDTH "openslide.level[%d].width"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_HEIGHT "openslide.level[%d].height"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_DOWNSAMPLE "openslide.level[%d].downsample"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_TILE_WIDTH "openslide.level[%d].tile-width"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_TILE_HEIGHT "openslide.level[%d].tile-height"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_X "openslide.region[%d].x"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_Y "openslide.region[%d].y"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_WIDTH "openslide.region[%d].width"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_HEIGHT "openslide.region[%d].height"

/* Tables */
// YCbCr -> RGB chroma contributions
extern const int16_t _openslide_R_Cr[256];
extern const int32_t _openslide_G_Cb[256];
extern const int32_t _openslide_G_Cr[256];
extern const int16_t _openslide_B_Cb[256];

// deprecated prefetch stuff (maybe we'll undeprecate it someday),
// still needs these declarations for ABI compat
// TODO: remove if soname bump
#undef openslide_give_prefetch_hint
OPENSLIDE_PUBLIC()
int openslide_give_prefetch_hint(openslide_t *osr,
				 int64_t x, int64_t y,
				 int32_t level,
				 int64_t w, int64_t h);
#undef openslide_cancel_prefetch_hint
OPENSLIDE_PUBLIC()
void openslide_cancel_prefetch_hint(openslide_t *osr, int prefetch_id);

/* Prevent use of dangerous functions and functions with mandatory wrappers.
   Every @p replacement must be unique to avoid conflicting-type errors. */
#define _OPENSLIDE_POISON(replacement) error__use_ ## replacement ## _instead
#define fopen _OPENSLIDE_POISON(_openslide_fopen)
#define fread _OPENSLIDE_POISON(_openslide_fread)
#define fseek _OPENSLIDE_POISON(_openslide_fseek)
#define ftell _OPENSLIDE_POISON(_openslide_ftell)
#define fclose _OPENSLIDE_POISON(_openslide_fclose)
#define g_file_test _OPENSLIDE_POISON(_openslide_fexists)
#define strtod _OPENSLIDE_POISON(_openslide_parse_double)
#define g_ascii_strtod _OPENSLIDE_POISON(_openslide_parse_double_)
#define sqlite3_open _OPENSLIDE_POISON(_openslide_sqlite_open)
#define sqlite3_open_v2 _OPENSLIDE_POISON(_openslide_sqlite_open_)
#define sqlite3_close _OPENSLIDE_POISON(_openslide_sqlite_close)
#define TIFFClientOpen _OPENSLIDE_POISON(_openslide_tiffcache_get)
#define TIFFFdOpen _OPENSLIDE_POISON(_openslide_tiffcache_get_)
#define TIFFOpen _OPENSLIDE_POISON(_openslide_tiffcache_get__)
#define TIFFSetDirectory _OPENSLIDE_POISON(_openslide_tiff_set_dir)

#ifndef NO_POISON_FSEEKO
// openslide-file.c needs the original macros
#undef fseeko
#undef ftello
#define fseeko _OPENSLIDE_POISON(_openslide_fseek_)
#define ftello _OPENSLIDE_POISON(_openslide_ftell_)
#endif

#endif
