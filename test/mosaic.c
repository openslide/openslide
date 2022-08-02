/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2014 Carnegie Mellon University
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <cairo.h>
#include "openslide.h"
#include "openslide-common.h"

#define TILE_WIDTH 256
#define TILE_HEIGHT 256
#define TILES_PER_ROW 4
#define TEXT_MARGIN 5
#define TEXT_BACKDROP_MARGIN 2
#define COLOR_BACKGROUND 0.6, 0.75, 0.9
#define COLOR_EMPTY 0.4, 0.4, 0.4
#define COLOR_ERROR 0.9, 0.5, 0.5
#define COLOR_GRID 0, 0, 0
#define COLOR_TEXT 0.6, 0, 0
#define COLOR_TEXT_BACKDROP 1, 1, 1, 0.75

static const char KEY_BASE[] = "base";
static const char KEY_SLIDE[] = "slide";
static const char KEY_LEVEL[] = "level";
static const char KEY_X[] = "x";
static const char KEY_Y[] = "y";

static void render_text(cairo_t *cr, const char *text) {
  // get top-left corner
  double x, y;
  cairo_get_current_point(cr, &x, &y);
  // calculate extents
  cairo_font_extents_t font;
  cairo_font_extents(cr, &font);
  cairo_text_extents_t extents;
  cairo_text_extents(cr, text, &extents);
  // draw backdrop
  cairo_set_source_rgba(cr, COLOR_TEXT_BACKDROP);
  cairo_rectangle(cr,
                  x - TEXT_BACKDROP_MARGIN,
                  y - TEXT_BACKDROP_MARGIN,
                  extents.width + 2 * TEXT_BACKDROP_MARGIN,
                  font.height + 2 * TEXT_BACKDROP_MARGIN);
  cairo_fill(cr);
  // draw text
  cairo_set_source_rgb(cr, COLOR_TEXT);
  cairo_move_to(cr, x, y + font.ascent);
  cairo_show_text(cr, text);
  // move to start of next line
  cairo_move_to(cr, x, y + font.height);
}

static void render_tile(cairo_t *cr, const char *name, const char *path,
                        int64_t x, int64_t y, int32_t level) {
  // read and draw tile
  g_autoptr(openslide_t) osr = openslide_open(path);
  g_autofree char *error = NULL;
  if (osr) {
    error = g_strdup(openslide_get_error(osr));
    if (!error) {
      uint32_t *buf = g_slice_alloc(TILE_WIDTH * TILE_HEIGHT * 4);
      openslide_read_region(osr, buf, x, y, level, TILE_WIDTH, TILE_HEIGHT);
      error = g_strdup(openslide_get_error(osr));
      if (!error) {
        // draw background
        cairo_set_source_rgb(cr, COLOR_BACKGROUND);
        cairo_rectangle(cr, 0, 0, TILE_WIDTH, TILE_HEIGHT);
        cairo_fill(cr);

        // draw tile
        cairo_surface_t *surface =
          cairo_image_surface_create_for_data((unsigned char *) buf,
                                              CAIRO_FORMAT_ARGB32,
                                              TILE_WIDTH, TILE_HEIGHT,
                                              TILE_WIDTH * 4);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_surface_destroy(surface);
        cairo_paint(cr);
      }
      g_slice_free1(TILE_WIDTH * TILE_HEIGHT * 4, buf);
    }
  } else {
    error = g_strdup("File not recognized");
  }

  // draw solid tile on error
  if (error) {
    cairo_set_source_rgb(cr, COLOR_ERROR);
    cairo_rectangle(cr, 0, 0, TILE_WIDTH, TILE_HEIGHT);
    cairo_fill(cr);
  }

  // draw grid lines
  cairo_set_source_rgb(cr, COLOR_GRID);
  cairo_save(cr);
  cairo_set_line_width(cr, 1);
  cairo_translate(cr, 0.5, 0.5);
  cairo_move_to(cr, TILE_WIDTH, 0);
  cairo_line_to(cr, TILE_WIDTH, TILE_HEIGHT);
  cairo_line_to(cr, 0, TILE_HEIGHT);
  cairo_stroke(cr);
  cairo_restore(cr);

  // draw text
  cairo_rectangle(cr, 0, 0, TILE_WIDTH, TILE_HEIGHT);
  cairo_clip(cr);
  cairo_move_to(cr, TEXT_MARGIN, TEXT_MARGIN);
  render_text(cr, name);
  if (error) {
    render_text(cr, error);
  }
}

int main(int argc, char **argv) {
  GError *tmp_err = NULL;

  common_fix_argv(&argc, &argv);
  if (argc != 4) {
    common_fail("Usage: %s <base-dir> <index-file> <out-file>", argv[0]);
  }
  const char *base_dir = argv[1];
  const char *index_file = argv[2];
  const char *out_file = argv[3];

  // read index file
  g_autoptr(GKeyFile) kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, index_file, G_KEY_FILE_NONE, &tmp_err)) {
    common_fail("Loading index file: %s", tmp_err->message);
  }
  gsize num_tiles;
  g_auto(GStrv) tile_names = g_key_file_get_groups(kf, &num_tiles);

  // calculate image size
  int cols = MIN(TILES_PER_ROW, num_tiles);
  int rows = (num_tiles + cols - 1) / cols;
  int width = cols * (TILE_WIDTH + 1) - 1;
  int height = rows * (TILE_HEIGHT + 1) - 1;

  // create cairo context
  cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(surface);

  // fill
  cairo_set_source_rgb(cr, COLOR_EMPTY);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw tiles
  for (int tile_num = 0; tile_num < (int) num_tiles; tile_num++) {
    int col = tile_num % cols;
    int row = tile_num / cols;

    cairo_save(cr);
    cairo_translate(cr,
                    col * (TILE_WIDTH + 1),
                    row * (TILE_HEIGHT + 1));

    char *name = tile_names[tile_num];
    g_autofree char *base = g_key_file_get_string(kf, name, KEY_BASE, NULL);
    if (!base) {
      common_fail("No base path specified for %s", name);
    }
    g_autofree char *slide = g_key_file_get_string(kf, name, KEY_SLIDE, NULL);
    if (!slide) {
      slide = g_path_get_basename(base);
    }
    int level = g_key_file_get_integer(kf, name, KEY_LEVEL, NULL);
    int x = g_key_file_get_integer(kf, name, KEY_X, NULL);
    int y = g_key_file_get_integer(kf, name, KEY_Y, NULL);
    g_autofree char *path = g_build_filename(base_dir, base, slide, NULL);
    render_tile(cr, name, path, x, y, level);

    cairo_restore(cr);
  }

  // check status
  cairo_status_t status = cairo_status(cr);
  if (status) {
    common_fail("cairo error: %s", cairo_status_to_string(status));
  }

  // write png
  status = cairo_surface_write_to_png(surface, out_file);
  if (status) {
    common_fail("writing PNG: %s", cairo_status_to_string(status));
  }

  // clean up
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  return 0;
}
