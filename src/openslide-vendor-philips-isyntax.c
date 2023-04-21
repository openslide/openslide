/*
 *  Copyright (C) 2022  Alexandr Virodov
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
*/

#include <assert.h>
#include <math.h>
#include <glib.h>
#include <string.h>
#include <tiffio.h>
#include "libisyntax.h"

#define IS_DEBUG_ANNOTATE_TILE false
#if IS_DEBUG_ANNOTATE_TILE
#include "font8x8_basic.h" // From https://github.com/dhepper/font8x8/blob/8e279d2d864e79128e96188a6b9526cfa3fbfef9/font8x8_basic.h
#endif

// This header "poisons" some functions, so must be included after system headers that use the poisoned functions (eg fclose in wchar.h).
#include "openslide-private.h"

// TODO(avirodov): better logging.
#define LOG(msg, ...) printf(msg, ##__VA_ARGS__)
#define LOG_VAR(fmt, var) printf("%s: %s=" fmt "\n", __FUNCTION__, #var, var)

// TODO(avirodov): better error handling. OpenSlide provides an error handling framework, should use it.
#define ASSERT_OK(_libisyntax_expression) assert(_libisyntax_expression == LIBISYNTAX_OK);

static const struct _openslide_ops philips_isyntax_ops;

typedef struct philips_isyntax_level {
    struct _openslide_level base;
    const isyntax_level_t* isyntax_level;
    struct _openslide_grid *grid;
} philips_isyntax_level;

typedef struct philips_isyntax_cache_t {
    // TODO(avirodov): this is clumsy (many "cache->cache" expressions). Keeping it this way in case I need a refcount.
    isyntax_cache_t* cache;
    // int refcount;
} philips_isyntax_cache_t;

typedef struct philips_isyntax_t {
    isyntax_t* isyntax;
    philips_isyntax_cache_t* cache;
} philips_isyntax_t;

// Global cache, shared between all opened files (if enabled). Thread-safe initialization in open().
philips_isyntax_cache_t* philips_isyntax_global_cache_ptr = NULL;

static void draw_horiz_line(uint32_t* tile_pixels, int32_t tile_width, int32_t y, int32_t start, int32_t end, uint32_t color) {
    for (int x = start; x < end; ++x) {
        tile_pixels[y*tile_width + x] = color;
    }
}

static void draw_vert_line(uint32_t* tile_pixels, int32_t tile_width, int32_t x, int32_t start, int32_t end, uint32_t color) {
    for (int y = start; y < end; ++y) {
        tile_pixels[y*tile_width + x] = color;
    }
}

static void draw_text(uint32_t* tile_pixels, int32_t tile_width, int32_t x_pos, int32_t y_pos, uint32_t color, const char* text) {
#if IS_DEBUG_ANNOTATE_TILE
    const int font_size = 8;
    for (const char* ch = text; *ch != 0; ++ch) {
        for (int y = 0; y < font_size; ++y) {
            uint8_t bit_line = font8x8_basic[*((u8*)ch)][y];
            for (int x = 0; x < font_size; ++x) {
                if (bit_line & (1u << x)) {
                    tile_pixels[(y + y_pos) * tile_width + x + x_pos] = color;
                }
            }
        }
        x_pos += font_size;
    }
#else
    // Unused variable warning suppression.
    (void)tile_pixels;
    (void)tile_width;
    (void)x_pos;
    (void)y_pos;
    (void)color;
    (void)text;
#endif
}

static void annotate_tile(uint32_t* tile_pixels, int32_t scale, int32_t tile_col, int32_t tile_row, int32_t tile_width, int32_t tile_height) {
    // TODO(avirodov): maybe move this to libisyntax.
    if (IS_DEBUG_ANNOTATE_TILE) {
        // OpenCV in C is hard... the core_c.h includes types_c.h which includes cvdef.h which is c++.
        // But we don't need much. Axis-aligned lines, and some simple text.
        int pad = 1;
        uint32_t color = 0xff0000ff; // ARGB
        draw_horiz_line(tile_pixels, tile_width, /*y=*/pad, /*start=*/pad, /*end=*/tile_width - pad, color);
        draw_horiz_line(tile_pixels, tile_width, /*y=*/tile_height - pad, /*start=*/pad, /*end=*/tile_width - pad, color);

        draw_vert_line(tile_pixels, tile_width, /*x=*/pad, /*start=*/pad, /*end=*/tile_height - pad, color);
        draw_vert_line(tile_pixels, tile_width, /*x=*/tile_width - pad, /*start=*/pad, /*end=*/tile_height - pad, color);

        char buf[128];
        sprintf(buf, "x=%d,y=%d,s=%d", tile_row, tile_col, scale);
        draw_text(tile_pixels, tile_width, 10, 10, color, buf);
    }
}

static bool philips_isyntax_detect(
        const char *filename,
        struct _openslide_tifflike *tl G_GNUC_UNUSED,
        GError **err G_GNUC_UNUSED) {
    LOG("got filename %s", filename);
    LOG_VAR("%p", tl);
    // reject TIFFs
    if (tl) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Is a TIFF file");
        return false;
    }

    g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
    if (f == NULL) {
        LOG("Failed to open file");
        return false;
    }

    const int num_chars_to_read = 256;
    g_autofree char *buf = g_malloc(num_chars_to_read);
    size_t num_read = _openslide_fread(f, buf, num_chars_to_read-1);
    buf[num_chars_to_read-1] = 0;
    LOG_VAR("%ld", num_read);
    LOG_VAR("%s", buf);

    // TODO(avirodov): probably a more robust XML parsing is needed.
    if (strstr(buf, "<DataObject ObjectType=\"DPUfsImport\">") != NULL) {
        LOG("got isyntax.");
        return true;
    }

    LOG("not isyntax.");
    return false;
}

static bool philips_isyntax_read_tile(
        openslide_t *osr,
        cairo_t *cr,
        struct _openslide_level *osr_level,
        int64_t tile_col, int64_t tile_row,
        void *arg G_GNUC_UNUSED,
        GError **err G_GNUC_UNUSED) {
    philips_isyntax_t *data = osr->data;
    isyntax_t* isyntax = data->isyntax;

    philips_isyntax_level* pi_level = (philips_isyntax_level*)osr_level;

    // LOG("level=%d tile_col=%ld tile_row=%ld", pi_level->level_idx, tile_col, tile_row);
    // tile size
    int64_t tw = libisyntax_get_tile_width(isyntax);
    int64_t th = libisyntax_get_tile_height(isyntax);

    // Openslide cache
    g_autoptr(_openslide_cache_entry) cache_entry = NULL;
    uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                              pi_level, tile_col, tile_row,
                                              &cache_entry);
    if (!tiledata) {
        int scale = libisyntax_level_get_scale(pi_level->isyntax_level);
        ASSERT_OK(libisyntax_tile_read(isyntax, data->cache->cache, scale, tile_col, tile_row, &tiledata));
        annotate_tile(tiledata, scale, tile_col, tile_row, tw, th);

        _openslide_cache_put(osr->cache, pi_level, tile_col, tile_row,
                             tiledata, tw * th * 4,
                             &cache_entry);
    }

    // draw it
    g_autoptr(cairo_surface_t) surface =
            cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                CAIRO_FORMAT_ARGB32,
                                                tw, th, tw * 4);
    cairo_set_source_surface(cr, surface, 0, 0);
    // https://lists.cairographics.org/archives/cairo/2012-June/023206.html
    // Those are the operators that are observed to work:
    //    w CAIRO_OPERATOR_SATURATE (current_cairo_operator, aka default OpenSlide)
    //    w CAIRO_OPERATOR_OVER ("This operator is cairo's default operator."),
    //    w CAIRO_OPERATOR_DEST_OVER,
    // SATURATE takes ~12sec to read a dummy slide (forcing all tiles to not exist), OVER & DEST_OVER take 3.5 sec
    // for same setup. Selecting OVER as the Cairo's default operator, the three outputs are identical.
    cairo_operator_t current_cairo_operator = cairo_get_operator(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_paint(cr);
    cairo_set_operator(cr, current_cairo_operator);
    return true;
}

static void add_float_property(openslide_t *osr, const char* property_name, float value) {
    g_hash_table_insert(osr->properties, g_strdup(property_name),
                        _openslide_format_double(value));
}

static bool philips_isyntax_open(
        openslide_t *osr,
        const char *filename,
        struct _openslide_tifflike *tl G_GNUC_UNUSED,
        struct _openslide_hash *quickhash1 G_GNUC_UNUSED,
        GError **err) {
    // Do not allow multithreading in opening.
    // https://docs.gtk.org/glib/method.Mutex.init.html:
    //   "It is not necessary to initialize a mutex that has been statically allocated."
    static GMutex static_open_mutex;
    g_mutex_lock(&static_open_mutex);
    static bool threadmemory_initialized = false;
    if (!threadmemory_initialized) {
        // TODO(avirodov): review libisyntax_init() thread safety, if thread safe, remove the mutex here.
        libisyntax_init();
        threadmemory_initialized = true;
    }
    g_mutex_unlock(&static_open_mutex);
    LOG("Opening file %s", filename);

    philips_isyntax_t* data = malloc(sizeof(philips_isyntax_t));

    osr->data = data;
    isyntax_error_t open_result = libisyntax_open(filename, /*is_init_allocators=*/0, &data->isyntax);
    LOG_VAR("%d", (int)open_result);
    // LOG_VAR("%d", data->isyntax->image_count); // TODO(avirodov): getter.
    if (open_result != LIBISYNTAX_OK) {
        free(data);
        g_prefix_error(err, "Can't open file.");
        return false;
    }

    // Initialize the cache (global, if requested).
    bool is_global_cache = true;
    int cache_size = 2000;
    const char* str_is_global_cache = g_environ_getenv(g_get_environ(), "OPENSLIDE_ISYNTAX_GLOBAL_CACHE");
    const char* str_cache_size = g_environ_getenv(g_get_environ(), "OPENSLIDE_ISYNTAX_CACHE_SIZE");
    if (str_is_global_cache && *str_is_global_cache == '0') {
        is_global_cache = false;
    }
    if (str_cache_size) {
        cache_size = atoi(str_cache_size);
    }
    /* TODO(avirodov): make debug api.
    {
        uint64_t memory_count = 0;
        isyntax_image_t* wsi = &data->isyntax->images[data->isyntax->wsi_image_index];
        for (int level_idx = 0; level_idx < wsi->level_count; ++level_idx) {
            memory_count += wsi->levels[level_idx].tile_count * sizeof(isyntax_tile_t);
        }
        memory_count += wsi->codeblock_count * sizeof(isyntax_codeblock_t);
        memory_count += wsi->data_chunk_count * sizeof(isyntax_data_chunk_t);
        printf("philips_isyntax_open is_global_cache=%d cache_size=%d sizeof_structs=%'lld\n", (int)is_global_cache, cache_size, memory_count);
    } */
    if (is_global_cache) {
        g_mutex_lock(&static_open_mutex);
        if (philips_isyntax_global_cache_ptr == NULL) {
            // Note: this requires that all opened files have the same block size. If that is not true, we
            // will need to have allocator per size. Alternatively, implement allocator freeing after
            // all tiles have been freed, and track isyntax_t per tile so we can access allocator.
            philips_isyntax_global_cache_ptr = malloc(sizeof(*philips_isyntax_global_cache_ptr));
            ASSERT_OK(libisyntax_cache_create("global_cache_list", cache_size, &philips_isyntax_global_cache_ptr->cache));
        }
        data->cache = philips_isyntax_global_cache_ptr;
        g_mutex_unlock(&static_open_mutex);
    } else {
        ASSERT_OK(libisyntax_cache_create("cache_list", cache_size, &data->cache->cache));
    }
    // Link the cache (local or global) to the isyntax file.
    libisyntax_cache_inject(data->cache->cache, data->isyntax);

    LOG_VAR("%d", libisyntax_get_is_mpp_known(data->isyntax));
    if (libisyntax_get_is_mpp_known(data->isyntax)) {
        double mpp_x = libisyntax_get_mpp_x(data->isyntax);
        double mpp_y = libisyntax_get_mpp_y(data->isyntax);
        LOG_VAR("%f", mpp_x);
        LOG_VAR("%f", mpp_y);
        add_float_property(osr, OPENSLIDE_PROPERTY_NAME_MPP_X, mpp_x);
        add_float_property(osr, OPENSLIDE_PROPERTY_NAME_MPP_Y, mpp_y);
        const float float_equals_tolerance = 1e-5;
        if (fabs(mpp_x - mpp_y) < float_equals_tolerance) {
            // Compute objective power from microns-per-pixel, see e.g. table in "Scan Performance" here:
            // https://www.microscopesinternational.com/blog/20170928-whichobjective.aspx
            float objective_power = 10.0f / mpp_x;
            LOG_VAR("%f", objective_power);
            add_float_property(osr, OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER, objective_power);
        }
    }

    // Find wsi image. Extracting other images not supported. Assuming only one wsi.
    int wsi_image_idx = libisyntax_get_wsi_image_index(data->isyntax);
    LOG_VAR("%d", wsi_image_idx);
    const isyntax_image_t* wsi_image = libisyntax_get_image(data->isyntax, wsi_image_idx);

    // Store openslide information about each level.
    osr->level_count = libisyntax_image_get_level_count(wsi_image);
    osr->levels = malloc(sizeof(philips_isyntax_level*) * osr->level_count);
    for (int i = 0; i < osr->level_count; ++i) {
        philips_isyntax_level* level = malloc(sizeof(philips_isyntax_level));
        level->isyntax_level = libisyntax_image_get_level(wsi_image, i);
        level->base.downsample = libisyntax_level_get_downsample_factor(level->isyntax_level);
        level->base.tile_w = libisyntax_get_tile_width(data->isyntax);
        level->base.tile_h = libisyntax_get_tile_height(data->isyntax);
        level->base.w = libisyntax_level_get_width_in_tiles(level->isyntax_level) * level->base.tile_w;
        level->base.h = libisyntax_level_get_height_in_tiles(level->isyntax_level) * level->base.tile_h;
        osr->levels[i] = (struct _openslide_level*)level;
        level->grid = _openslide_grid_create_simple(
                osr,
                libisyntax_level_get_width_in_tiles(level->isyntax_level),
                libisyntax_level_get_height_in_tiles(level->isyntax_level),
                level->base.tile_w,
                level->base.tile_h,
                philips_isyntax_read_tile);

        // TODO(avirodov): log levels? Right now too noisy.
        // LOG_VAR("%d", i);
        // LOG_VAR("%d", libisyntax_level_get_scale(level->isyntax_level));
        // LOG_VAR("%d", libisyntax_level_get_width_in_tiles(level->isyntax_level));
        // LOG_VAR("%d", libisyntax_level_get_height_in_tiles(level->isyntax_level));
        // LOG_VAR("%f", libisyntax_level_get_downsample_factor(level->isyntax_level));
        // TODO(avirodov): getters in API, if needed.
        // LOG_VAR("%f", levels[i].um_per_pixel_x);
        // LOG_VAR("%f", levels[i].um_per_pixel_y);
        // LOG_VAR("%f", levels[i].x_tile_side_in_um);
        // LOG_VAR("%f", levels[i].y_tile_side_in_um);
        // LOG_VAR("%lld", levels[i].tile_count);
        // LOG_VAR("%f", levels[i].origin_offset_in_pixels);
        // LOG_VAR("%f", levels[i].origin_offset.x);
        // LOG_VAR("%f", levels[i].origin_offset.y);
        // LOG_VAR("%d", (int)levels[i].is_fully_loaded);
    }
    osr->ops = &philips_isyntax_ops;
    return true;
}

static bool philips_isyntax_paint_region(
        openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
        int64_t x, int64_t y,
        struct _openslide_level *osr_level,
        int32_t w, int32_t h,
        GError **err) {
    philips_isyntax_level* level = (philips_isyntax_level*)osr_level;

    // LOG("x=%ld y=%ld level=%d w=%d h=%d", x, y, level->level_idx, w, h);
    // Note: round is necessary to avoid producing resampled (and thus blurry) images on higher levels.
    double origin_offset_in_pixels = libisyntax_level_get_origin_offset_in_pixels(level->isyntax_level);
    return _openslide_grid_paint_region(level->grid, cr, NULL,
                                        round((x - origin_offset_in_pixels) / level->base.downsample),
                                        round((y - origin_offset_in_pixels) / level->base.downsample),
                                        osr_level, w, h,
                                        err);
}

static void philips_isyntax_destroy(openslide_t *osr) {
    philips_isyntax_t *data = osr->data;

    for (int i = 0; i < osr->level_count; ++i) {
        philips_isyntax_level* level = (philips_isyntax_level*)osr->levels[i];
        _openslide_grid_destroy(level->grid);
        free(level);
    }
    // Flush cache (especially if global).
    // TODO(avirodov): if we track for each tile (or cache entry) which isyntax_t* it came from, we can remove
    //  only those entries from global cache.
    if (data->cache == philips_isyntax_global_cache_ptr) {
        libisyntax_cache_flush(data->cache->cache, data->isyntax);
    } else {
        libisyntax_cache_destroy(data->cache->cache);
    }

    free(osr->levels);
    libisyntax_close(data->isyntax);
    free(data);
}

const struct _openslide_format _openslide_format_philips_isyntax = {
        .name = "philips-isyntax",
        .vendor = "philips-isyntax",
        .detect = philips_isyntax_detect,
        .open = philips_isyntax_open,
};

static const struct _openslide_ops philips_isyntax_ops = {
        .paint_region = philips_isyntax_paint_region,
        .destroy = philips_isyntax_destroy,
};
