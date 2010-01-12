/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "config.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <jpeglib.h>

#include "openslide-private.h"

static const char GROUP_VMS[] = "Virtual Microscope Specimen";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_NUM_LAYERS[] = "NoLayers";
static const char KEY_NUM_JPEG_COLS[] = "NoJpegColumns";
static const char KEY_NUM_JPEG_ROWS[] = "NoJpegRows";
static const char KEY_OPTIMISATION_FILE[] = "OptimisationFile";
static const char KEY_MACRO_IMAGE[] = "MacroImage";

// returns w and h and tw and th and comment as a convenience
static bool verify_jpeg(FILE *f, int32_t *w, int32_t *h,
			int32_t *tw, int32_t *th,
			char **comment) {
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;

  *w = 0;
  *h = 0;
  *tw = 0;
  *th = 0;

  if (comment) {
    *comment = NULL;
  }

  if (setjmp(env) == 0) {
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);

    int header_result;

    if (comment) {
      // extract comment
      jpeg_save_markers(&cinfo, JPEG_COM, 0xFFFF);
    }

    header_result = jpeg_read_header(&cinfo, TRUE);
    if ((header_result != JPEG_HEADER_OK
	 && header_result != JPEG_HEADER_TABLES_ONLY)
	|| cinfo.num_components != 3 || cinfo.restart_interval == 0) {
      jpeg_destroy_decompress(&cinfo);
      return false;
    }

    jpeg_start_decompress(&cinfo);

    if (comment) {
      if (cinfo.marker_list) {
	// copy everything out
	char *com = g_strndup((const gchar *) cinfo.marker_list->data,
			      cinfo.marker_list->data_length);
	// but only really save everything up to the first '\0'
	*comment = g_strdup(com);
	g_free(com);
      }
      jpeg_save_markers(&cinfo, JPEG_COM, 0);  // stop saving
    }

    *w = cinfo.output_width;
    *h = cinfo.output_height;

    if (cinfo.restart_interval > cinfo.MCUs_per_row) {
      g_warning("Restart interval greater than MCUs per row");
      jpeg_destroy_decompress(&cinfo);
      return false;
    }

    *tw = *w / (cinfo.MCUs_per_row / cinfo.restart_interval);
    *th = *h / cinfo.MCU_rows_in_scan;
    int leftover_mcus = cinfo.MCUs_per_row % cinfo.restart_interval;
    if (leftover_mcus != 0) {
      jpeg_destroy_decompress(&cinfo);
      return false;
    }


    //  g_debug("w: %d, h: %d, restart_interval: %d\n"
    //	 "mcus_per_row: %d, mcu_rows_in_scan: %d\n"
    //	 "leftover mcus: %d",
    //	 cinfo.output_width, cinfo.output_height,
    //	 cinfo.restart_interval,
    //	 cinfo.MCUs_per_row, cinfo.MCU_rows_in_scan,
    //	 leftover_mcus);
  } else {
    // setjmp has returned again
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  jpeg_destroy_decompress(&cinfo);
  return true;
}

static int64_t *extract_one_optimisation(FILE *opt_f,
					 int32_t num_tiles_down,
					 int32_t num_tiles_across,
					 int32_t mcu_starts_count) {
  int64_t *mcu_starts = g_new(int64_t, mcu_starts_count);
  for (int32_t i = 0; i < mcu_starts_count; i++) {
    mcu_starts[i] = -1; // UNKNOWN value
  }

  // optimisation file is in a weird format, it is 32- (or 64- or 320- ?) bit
  // little endian values, giving the file offset into an MCU row,
  // each offset starts at a 40-byte alignment, and the last row (of the
  // entire file, not each image) seems to be missing

  // also, the offsets are all packed into 1 file, even with multiple images

  // we will read the file and verify at least that the markers
  // are valid, if anything is fishy, we will not use it

  // we represent missing data by -1, which we initialize to,
  // so if we run out of opt file, we can just stop

  for (int32_t row = 0; row < num_tiles_down; row++) {
    // read 40 bytes
    union {
      uint8_t buf[40];
      int64_t i64;
    } u;

    if (fread(u.buf, 40, 1, opt_f) != 1) {
      // EOF or error, we've done all we can

      if (row == 0) {
	// if we don't even get the first one, deallocate
	goto FAIL;
      }

      break;
    }

    // get the offset
    int64_t offset = GINT64_FROM_LE(u.i64);

    // record this marker
    mcu_starts[row * num_tiles_across] = offset;
  }

  return mcu_starts;

 FAIL:
  g_free(mcu_starts);
  return NULL;
}

static void add_properties(GHashTable *ht, GKeyFile *kf) {
  g_hash_table_insert(ht,
		      g_strdup(_OPENSLIDE_VENDOR_NAME),
		      g_strdup("hamamatsu"));

  char **keys = g_key_file_get_keys(kf, GROUP_VMS, NULL, NULL);
  if (keys == NULL) {
    return;
  }

  for (char **key = keys; *key != NULL; key++) {
    char *value = g_key_file_get_value(kf, GROUP_VMS, *key, NULL);
    if (value) {
      g_hash_table_insert(ht,
			  g_strdup_printf("hamamatsu.%s", *key),
			  g_strdup(value));
      g_free(value);
    }
  }

  g_strfreev(keys);
}

static void add_macro_associated_image(GHashTable *ht,
				       FILE *f) {
  _openslide_add_jpeg_associated_image(ht, "macro", f);
}

bool _openslide_try_hamamatsu(openslide_t *osr, const char *filename,
			      GChecksum *checksum) {
  char *dirname = g_path_get_dirname(filename);
  char **image_filenames = NULL;
  struct _openslide_jpeg_file **jpegs = NULL;
  int num_jpegs = 0;

  char *optimisation_filename = NULL;
  FILE *optimisation_file = NULL;

  char **all_keys = NULL;

  bool success = false;
  char *tmp;

  // init layers: base image + map
  int32_t layer_count = 2;
  struct _openslide_jpeg_layer **layers =
    g_new0(struct _openslide_jpeg_layer *, layer_count);
  for (int32_t i = 0; i < layer_count; i++) {
    layers[i] = g_slice_new0(struct _openslide_jpeg_layer);
    layers[i]->tiles = _openslide_jpeg_create_tiles_table();
  }

  // first, see if it's a VMS file
  GKeyFile *vms_file = g_key_file_new();
  if (!g_key_file_load_from_file(vms_file, filename, G_KEY_FILE_NONE, NULL)) {
    //    g_debug("Can't load VMS file");
    goto FAIL;
  }
  if (!g_key_file_has_group(vms_file, GROUP_VMS)) {
    //    g_warning("Can't find VMS group");
    goto FAIL;
  }


  // make sure values are within known bounds
  int num_layers = g_key_file_get_integer(vms_file, GROUP_VMS, KEY_NUM_LAYERS,
					  NULL);
  if (num_layers != 1) {
    g_warning("Cannot handle VMS files with NoLayers != 1");
    goto FAIL;
  }

  int num_jpeg_cols = g_key_file_get_integer(vms_file, GROUP_VMS,
					     KEY_NUM_JPEG_COLS,
					     NULL);
  if (num_jpeg_cols < 1) {
    goto FAIL;
  }

  int num_jpeg_rows = g_key_file_get_integer(vms_file,
					     GROUP_VMS,
					     KEY_NUM_JPEG_ROWS,
					     NULL);
  if (num_jpeg_rows < 1) {
    goto FAIL;
  }

  // this format has cols*rows jpeg files, plus the map
  num_jpegs = (num_jpeg_cols * num_jpeg_rows) + 1;
  image_filenames = g_new0(char *, num_jpegs);
  jpegs = g_new0(struct _openslide_jpeg_file *, num_jpegs);

  //  g_debug("vms rows: %d, vms cols: %d, num_jpegs: %d", num_jpeg_rows, num_jpeg_cols, num_jpegs);

  // add properties
  if (osr) {
    add_properties(osr->properties, vms_file);
  }


  // extract MapFile
  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_MAP_FILE,
			      NULL);
  if (tmp) {
    image_filenames[num_jpegs - 1] = g_build_filename(dirname, tmp, NULL);
    struct _openslide_jpeg_file *file =
      g_slice_new0(struct _openslide_jpeg_file);
    jpegs[num_jpegs - 1] = file;

    g_free(tmp);
  } else {
    g_warning("Can't read map file");
    goto FAIL;
  }


  // extract OptimisationFile
  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_OPTIMISATION_FILE,
			      NULL);
  if (tmp) {
    optimisation_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);
  }

  // now each ImageFile
  all_keys = g_key_file_get_keys(vms_file, GROUP_VMS, NULL, NULL);
  char **tmp2;
  for (tmp2 = all_keys; *tmp2 != NULL; tmp2++) {
    char *key = *tmp2;
    char *value = g_key_file_get_string(vms_file, GROUP_VMS, key, NULL);

    //    g_debug("%s", key);

    if (strncmp(KEY_IMAGE_FILE, key, strlen(KEY_IMAGE_FILE)) == 0) {
      // starts with ImageFile
      char *suffix = key + strlen(KEY_IMAGE_FILE);

      int col;
      int row;

      if (suffix[0] != '\0') {
	// parse out the row and col
	char *endptr;

	// skip (
	suffix++;

	// col
	col = g_ascii_strtoll(suffix, &endptr, 10);
	//	g_debug("%d", col);

	// skip ,
	endptr++;

	// row
	row = g_ascii_strtoll(endptr, NULL, 10);
	//	g_debug("%d", row);
      } else {
	col = 0;
	row = 0;
      }

      //      g_debug("col: %d, row: %d", col, row);

      if (col >= num_jpeg_cols || row >= num_jpeg_rows) {
	g_warning("Invalid row or column in VMS file");
	goto FAIL;
      }

      // compute index from x,y
      int i = row * num_jpeg_cols + col;

      // init the file
      if (jpegs[i]) {
	g_warning("Ignoring duplicate image for (%d,%d)", col, row);
      } else {
	image_filenames[i] = g_build_filename(dirname, value, NULL);

	jpegs[i] = g_slice_new0(struct _openslide_jpeg_file);
      }
    }
    g_free(value);
  }

  // check image filenames (the others are sort of optional)
  for (int i = 0; i < num_jpegs; i++) {
    if (!image_filenames[i]) {
      g_warning("Can't read image filename %d", i);
      goto FAIL;
    }
  }

  // open jpegs
  if (optimisation_filename != NULL) {
    optimisation_file = fopen(optimisation_filename, "rb");
  }

  if (optimisation_file == NULL) {
    g_warning("Can't use optimisation file");
  }

  int32_t jpeg0_tw = 0;
  int32_t jpeg0_th = 0;
  int32_t jpeg0_ta = 0;
  int32_t jpeg0_td = 0;

  for (int i = 0; i < num_jpegs; i++) {
    struct _openslide_jpeg_file *jp = jpegs[i];

    // these jpeg files always start at 0
    jp->start_in_file = 0;
    jp->filename = g_strdup(image_filenames[i]);

    FILE *f;
    if ((f = fopen(jp->filename, "rb")) == NULL) {
      g_warning("Can't open JPEG %d", i);
      goto FAIL;
    }

    // comment?
    char *comment = NULL;
    char **comment_ptr = NULL;
    if (i == 0 && osr) {
      comment_ptr = &comment;
    }

    if (!verify_jpeg(f, &jp->w, &jp->h, &jp->tw, &jp->th, comment_ptr)) {
      g_warning("Can't verify JPEG %d", i);
      fclose(f);
      goto FAIL;
    }

    if (comment) {
      g_hash_table_insert(osr->properties,
			  g_strdup(_OPENSLIDE_COMMENT_NAME),
			  comment);
    }

    fseeko(f, 0, SEEK_END);
    jp->end_in_file = ftello(f);
    if (jp->end_in_file == -1) {
      g_warning("Can't read file size for JPEG %d", i);
      fclose(f);
      goto FAIL;
    }

    // file is done now
    fclose(f);

    int32_t num_tiles_across = jp->w / jp->tw;
    int32_t num_tiles_down = jp->h / jp->th;

    // because map file is last, ensure that all tw and th are the
    // same for 0 through num_jpegs-2
    //    g_debug("tile size: %d %d", tw, th);
    if (i == 0) {
      jpeg0_tw = jp->tw;
      jpeg0_th = jp->th;
      jpeg0_ta = num_tiles_across;
      jpeg0_td = num_tiles_down;
    } else if (i != (num_jpegs - 1)) {
      // not map file (still within layer 0)
      g_assert(jpeg0_tw != 0 && jpeg0_th != 0);
      if (jpeg0_tw != jp->tw || jpeg0_th != jp->th) {
	g_warning("Tile size not consistent");
	goto FAIL;
      }
    }

    // use the optimisation file, if present
    int32_t mcu_starts_count = (jp->w / jp->tw) * (jp->h / jp->th); // number of tiles
    int64_t *mcu_starts = NULL;
    if (optimisation_file) {
      mcu_starts = extract_one_optimisation(optimisation_file,
					    num_tiles_down,
					    num_tiles_across,
					    mcu_starts_count);
    }
    if (mcu_starts) {
      jp->mcu_starts = mcu_starts;
    } else if (optimisation_file != NULL) {
      // the optimisation file is useless, close it
      fclose(optimisation_file);
      optimisation_file = NULL;
    }

    // accumulate into some of the fields of the layers
    int32_t layer;
    if (i != num_jpegs - 1) {
      // base (layer 0)
      layer = 0;
    } else {
      // map (layer 1)
      layer = 1;
    }

    struct _openslide_jpeg_layer *l = layers[layer];
    int32_t file_x = 0;
    int32_t file_y = 0;
    if (layer == 0) {
      file_x = i % num_jpeg_cols;
      file_y = i / num_jpeg_cols;
    }
    if (file_y == 0) {
      l->layer_w += jp->w;
      l->tiles_across += num_tiles_across;
    }
    if (file_x == 0) {
      l->layer_h += jp->h;
      l->tiles_down += num_tiles_down;
    }

    // set some values (don't accumulate)
    l->raw_tile_width = jp->tw;
    l->raw_tile_height = jp->th;
    l->tile_advance_x = jp->tw;   // no overlaps or funny business
    l->tile_advance_y = jp->th;
  }

  // at this point, jpeg0_ta and jpeg0_td are set to values from 0,0 in layer 0

  for (int i = 0; i < num_jpegs; i++) {
    struct _openslide_jpeg_file *jp = jpegs[i];

    int32_t layer;
    int32_t file_x;
    int32_t file_y;
    if (i != num_jpegs - 1) {
      // base (layer 0)
      layer = 0;
      file_x = i % num_jpeg_cols;
      file_y = i / num_jpeg_cols;
    } else {
      // map (layer 1)
      layer = 1;
      file_x = 0;
      file_y = 0;
    }

    //g_debug("processing file %d %d %d", file_x, file_y, layer);

    int32_t num_tiles_across = jp->w / jp->tw;

    struct _openslide_jpeg_layer *l = layers[layer];

    int32_t tile_count = (jp->w / jp->tw) * (jp->h / jp->th); // number of tiles

    // add all the tiles
    for (int local_tileno = 0; local_tileno < tile_count; local_tileno++) {
      struct _openslide_jpeg_tile *t = g_slice_new0(struct _openslide_jpeg_tile);

      int32_t local_tile_x = local_tileno % num_tiles_across;
      int32_t local_tile_y = local_tileno / num_tiles_across;

      t->fileno = i;
      t->tileno = local_tileno;
      t->w = jp->tw;
      t->h = jp->th;
      // no dest or src offsets

      // compute key for hashtable (y * w + x)
      int64_t x = file_x * jpeg0_ta + local_tile_x;
      int64_t y = file_y * jpeg0_td + local_tile_y;

      int64_t *key = g_slice_new(int64_t);
      *key = (y * l->tiles_across) + x;

      //g_debug("inserting tile: fileno %d tileno %d, %gx%g, file: %d %d, local: %d %d, global: %" PRId64 " %" PRId64 ", l->tiles_across: %d, key: %" PRId64, t->fileno, t->tileno, t->w, t->h, file_x, file_y, local_tile_x, local_tile_y, x, y, l->tiles_across, *key);
      g_assert(!g_hash_table_lookup(l->tiles, key));
      g_hash_table_insert(l->tiles, key, t);
    }
  }


  // add macro image if present
  if (osr) {
    tmp = g_key_file_get_string(vms_file,
				GROUP_VMS,
				KEY_MACRO_IMAGE,
				NULL);
    if (tmp) {
      char *macro_filename = g_build_filename(dirname, tmp, NULL);
      FILE *macro_f = fopen(macro_filename, "rb");
      if (macro_f) {
	add_macro_associated_image(osr->associated_images, macro_f);
      }
      fclose(macro_f);
      g_free(macro_filename);
      g_free(tmp);
    }
  }

  _openslide_add_jpeg_ops(osr, num_jpegs, jpegs, layer_count, layers);
  success = true;
  goto DONE;

 FAIL:
  if (jpegs) {
    for (int i = 0; i < num_jpegs; i++) {
      g_free(jpegs[i]->filename);
    }
    g_free(jpegs);
  }

  if (layers) {
    for (int i = 0; i < layer_count; i++) {
      g_hash_table_unref(layers[i]->tiles);
      g_slice_free(struct _openslide_jpeg_layer, layers[i]);
    }
    g_free(layers);
  }

  success = false;

 DONE:
  g_strfreev(all_keys);
  g_free(dirname);
  g_free(optimisation_filename);

  if (optimisation_file) {
    fclose(optimisation_file);
  }

  if (image_filenames) {
    for (int i = 0; i < num_jpegs; i++) {
      g_free(image_filenames[i]);
    }
    g_free(image_filenames);
  }
  g_key_file_free(vms_file);

  return success;
}
