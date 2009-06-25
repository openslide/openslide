/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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

static const char MRXS_EXT[] = ".mrxs";
static const char SLIDEDAT_INI[] = "Slidedat.ini";

static const char GROUP_GENERAL[] = "GENERAL";
static const char KEY_SLIDE_VERSION[] = "SLIDE_VERSION";
static const char KEY_SLIDE_ID[] = "SLIDE_ID";
static const char KEY_IMAGENUMBER_X[] = "IMAGENUMBER_X";
static const char KEY_IMAGENUMBER_Y[] = "IMAGENUMBER_Y";

static const char GROUP_HIERARCHICAL[] = "HIERARCHICAL";
static const char KEY_HIER_COUNT[] = "HIER_COUNT";
static const char KEY_NONHIER_COUNT[] = "NONHIER_COUNT";
static const char KEY_INDEXFILE[] = "INDEXFILE";
static const char KEY_HIER_d_NAME[] = "HIER_%d_NAME";
static const char KEY_HIER_d_COUNT[] = "HIER_%d_COUNT";
static const char KEY_HIER_d_VAL_d_SECTION[] = "HIER_%d_VAL_%d_SECTION";
static const char KEY_NONHIER_d_NAME[] = "NONHIER_%d_NAME";
static const char KEY_NONHIER_d_COUNT[] = "NONHIER_%d_COUNT";
static const char VALUE_VIMSLIDE_POSITION_BUFFER[] = "VIMSLIDE_POSITION_BUFFER";
static const char VALUE_SLIDE_ZOOM_LEVEL[] = "Slide zoom level";

static const char GROUP_NONHIERLAYER_d_SECTION[] = "NONHIERLAYER_%d_SECTION";
static const char KEY_VIMSLIDE_POSITION_DATA_FORMAT_VERSION[] =
  "VIMSLIDE_POSITION_DATA_FORMAT_VERSION";
static const int VALUE_VIMSLIDE_POSITION_DATA_FORMAT_VERSION = 257;

static const char GROUP_DATAFILE[] = "DATAFILE";
static const char KEY_FILE_COUNT[] = "FILE_COUNT";
static const char KEY_d_FILE[] = "FILE_%d";

static const char KEY_OVERLAP_X[] = "OVERLAP_X";
static const char KEY_OVERLAP_Y[] = "OVERLAP_Y";
static const char KEY_IMAGE_FORMAT[] = "IMAGE_FORMAT";
static const char KEY_IMAGE_FILL_COLOR_BGR[] = "IMAGE_FILL_COLOR_BGR";
static const char KEY_DIGITIZER_WIDTH[] = "DIGITIZER_WIDTH";
static const char KEY_DIGITIZER_HEIGHT[] = "DIGITIZER_HEIGHT";
static const char KEY_IMAGE_CONCAT_FACTOR[] = "IMAGE_CONCAT_FACTOR";

#define READ_KEY_OR_FAIL(TARGET, KEYFILE, GROUP, KEY, TYPE, FAIL_MSG)	\
  do {									\
    GError *err = NULL;							\
    TARGET = g_key_file_get_ ## TYPE(KEYFILE, GROUP, KEY, &err);	\
    if (err != NULL) {							\
      g_warning(FAIL_MSG); g_error_free(err); goto FAIL;		\
    }									\
  } while(0)

struct slide_zoom_level_section {
  double overlap_x;
  double overlap_y;

  uint32_t fill_argb;

  int tile_w;
  int tile_h;
};

struct mirax_hier_page_entry {
  int32_t x;
  int32_t y;
  int32_t offset;
  int32_t length;
  int32_t fileno;
  int zoom_level;
};

struct mirax_nonhier_page_entry {
  int32_t offset;
  int32_t length;
  int32_t fileno;
};

static gint hier_page_entry_compare(gconstpointer a, gconstpointer b) {
  const struct mirax_hier_page_entry *pa = (const struct mirax_hier_page_entry *) a;
  const struct mirax_hier_page_entry *pb = (const struct mirax_hier_page_entry *) b;

  if (pa->y != pb->y) {
    return pa->y - pb->y;
  }

  return pa->x - pb->x;
}

static void hier_page_entry_delete(gpointer data, gpointer user_data) {
  g_slice_free(struct mirax_hier_page_entry, data);
}

static bool verify_string_from_file(FILE *f, const char *str) {
  bool result;
  int len = strlen(str);

  char *possible_str = g_malloc(len + 1);
  possible_str[len] = '\0';
  size_t size = fread(possible_str, len, 1, f);

  g_debug("\"%s\" == \"%s\" ?", str, possible_str);

  result = (size == 1) && (strcmp(str, possible_str) == 0);

  g_free(possible_str);
  return result;
}

static int32_t read_le_int32_from_file(FILE *f) {
  int32_t i;

  if (fread(&i, 4, 1, f) != 1) {
    return -1;
  }

  i = GINT32_FROM_LE(i);
  //  g_debug("%d", i);

  return i;
}

static bool read_hier_data_pages_from_indexfile(GList **list, FILE *f,
						int zoom_level,
						int tiles_across) {
  // read initial 0
  if (read_le_int32_from_file(f) != 0) {
    g_warning("Expected 0 value at beginning of data page");
    return false;
  }

  // read pointer
  int32_t ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_warning("Can't read initial data page pointer");
    return false;
  }

  // seek to offset
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_warning("Can't seek to initial data page");
    return false;
  }

  int32_t next_ptr;
  do {
    // read length
    int32_t page_len = read_le_int32_from_file(f);
    if (page_len == -1) {
      g_warning("Can't read page length");
      return false;
    }

    //    g_debug("page_len: %d", page_len);

    // read "next" pointer
    next_ptr = read_le_int32_from_file(f);
    if (next_ptr == -1) {
      g_warning("Cannot read \"next\" pointer");
      return false;
    }

    // read all the data into the list
    for (int i = 0; i < page_len; i++) {
      int32_t tile_index = read_le_int32_from_file(f);
      int32_t offset = read_le_int32_from_file(f);
      int32_t length = read_le_int32_from_file(f);
      int32_t fileno = read_le_int32_from_file(f);

      if (tile_index < 0) {
	g_warning("tile_index < 0");
	return false;
      }
      if (offset < 0) {
	g_warning("offset < 0");
	return false;
      }
      if (length < 0) {
	g_warning("length < 0");
	return false;
      }
      if (fileno < 0) {
	g_warning("fileno < 0");
	return false;
      }

      // we have only encountered images with exactly power-of-two scale
      // factors, and there appears to be no clear way to specify otherwise,
      // so require it
      int32_t x = tile_index % tiles_across;
      int32_t y = tile_index / tiles_across;

      if (x % (1 << zoom_level)) {
	g_warning("x (%d) not correct multiple for zoom level (%d)",
		  x, zoom_level);
	return false;
      }
      if (y % (1 << zoom_level)) {
	g_warning("y (%d) not correct multiple for zoom level (%d)",
		  y, zoom_level);
	return false;
      }

      struct mirax_hier_page_entry *entry = g_slice_new(struct mirax_hier_page_entry);
      entry->offset = offset;
      entry->length = length;
      entry->fileno = fileno;
      entry->zoom_level = zoom_level;

      // store x and y in layer coordinates (not layer0)
      entry->x = x >> zoom_level;
      entry->y = y >> zoom_level;

      //      print_page_entry(entry, NULL);

      *list = g_list_prepend(*list, entry);
    }
  } while (next_ptr != 0);

  // check for empty list
  if (*list == NULL) {
    g_warning("Empty page");
    return false;
  }

  return true;
}

static void file_table_fclose(gpointer key, gpointer value, gpointer user_data) {
  fclose(value);
}


static void process_indexfile(const char *slideversion,
			      const char *uuid,
			      const char *dirname,
			      int datafile_count,
			      char **datafile_names,
			      int slideposition_offset,
			      int zoom_levels,
			      int tiles_x,
			      int tiles_y,
			      struct slide_zoom_level_section *slide_zoom_level_sections,
			      FILE *indexfile,
			      struct _openslide_jpeg_layer **layers,
			      int *file_count_out,
			      struct _openslide_jpeg_file ***files_out) {
  // init out parameters
  *file_count_out = 0;
  *files_out = NULL;

  struct _openslide_jpeg_file **jpegs = NULL;
  bool success = false;

  int64_t hier_root;
  int64_t nonhier_root;

  GList *hier_page_entry_list = NULL;
  GHashTable *file_table = NULL;

  rewind(indexfile);

  // verify slideversion and uuid
  if (!(verify_string_from_file(indexfile, slideversion) &&
	verify_string_from_file(indexfile, uuid))) {
    g_warning("Indexfile doesn't start with expected values");
    goto OUT;
  }

  // save root positions
  hier_root = ftello(indexfile);
  nonhier_root = hier_root + 4;

  int32_t ptr = read_le_int32_from_file(indexfile);
  if (ptr == -1) {
    g_warning("Can't read initial pointer");
    goto OUT;
  }

  // jump to start of interesting data
  //  g_debug("seek %d", ptr);
  if (fseeko(indexfile, ptr, SEEK_SET) == -1) {
    g_warning("Cannot seek to start of interesting data");
    goto OUT;
  }

  // read all zoom level data
  off_t seek_location = ptr;
  for (int i = 0; i < zoom_levels; i++) {
    g_debug("reading zoom_level %d", i);

    if (fseeko(indexfile, seek_location, SEEK_SET) == -1) {
      g_warning("Cannot seek to zoom level pointer %d", i + 1);
      goto OUT;
    }

    int32_t ptr = read_le_int32_from_file(indexfile);
    if (ptr == -1) {
      g_warning("Can't read zoom level pointer");
      goto OUT;
    }
    if (fseeko(indexfile, ptr, SEEK_SET) == -1) {
      g_warning("Cannot seek to start of data pages");
      goto OUT;
    }

    // read these pages in, make sure they are sorted, and add to the master list
    GList *tmp_list = NULL;
    bool success = read_hier_data_pages_from_indexfile(&tmp_list, indexfile, i,
						       tiles_x);
    tmp_list = g_list_sort(tmp_list, hier_page_entry_compare);
    g_debug(" length: %d", g_list_length(tmp_list));
    hier_page_entry_list = g_list_concat(hier_page_entry_list, tmp_list);

    if (!success) {
      g_warning("Cannot read some data pages from indexfile");
      goto OUT;
    }

    // advance for next zoom level
    seek_location += 4;
  }

  file_table = g_hash_table_new_full(g_int_hash, g_int_equal,
				     g_free, NULL);


  // build up the file/tile/layer structs
  int jpeg_count = g_list_length(hier_page_entry_list);

  for (int i = 0; i < zoom_levels; i++) {
    
  }

  jpegs = g_new(struct _openslide_jpeg_file *, jpeg_count);

  int cur_file = 0;
  for (GList *iter = hier_page_entry_list; iter != NULL; iter = iter->next) {
    struct mirax_hier_page_entry *entry = iter->data;
    // open file if necessary
    FILE *f = g_hash_table_lookup(file_table, &entry->fileno);
    if (!f) {
      if (entry->fileno >= datafile_count) {
	g_warning("Invalid fileno");
	goto OUT;
      }
      const char *name = datafile_names[entry->fileno];
      char *tmp = g_build_filename(dirname, name, NULL);
      f = fopen(tmp, "rb");
      g_free(tmp);

      if (!f) {
	g_warning("Can't open file for fileno %d", entry->fileno);
	goto OUT;
      }

      int *key = g_new(int, 1);
      *key = entry->fileno;
      g_hash_table_insert(file_table, key, f);
    }

    // file is open

    struct _openslide_jpeg_file *jpeg = g_slice_new0(struct _openslide_jpeg_file);
    struct slide_zoom_level_section *section = slide_zoom_level_sections + entry->zoom_level;

    // populate the file structure
    jpeg->f = f;
    jpeg->start_in_file = entry->offset;
    jpeg->end_in_file = jpeg->start_in_file + entry->length;
    jpeg->tw = section->tile_w;
    jpeg->th = section->tile_h;
    jpeg->w = section->tile_w;
    jpeg->h = section->tile_h;

    cur_file++;
  }

  g_assert(cur_file == jpeg_count);

  g_hash_table_unref(file_table);
  file_table = NULL;
  success = true;

 OUT:
  // deallocate
  g_list_foreach(hier_page_entry_list, hier_page_entry_delete, NULL);
  g_list_free(hier_page_entry_list);

  if (file_table) {
    g_hash_table_foreach(file_table, file_table_fclose, NULL);
    g_hash_table_unref(file_table);
  }

  if (success) {
    *file_count_out = jpeg_count;
    *files_out = jpegs;
  }
  // XXX leaking memory
}


bool _openslide_try_mirax(openslide_t *osr, const char *filename) {
  struct _openslide_jpeg_file **jpegs = NULL;
  int num_jpegs = 0;
  struct _openslide_jpeg_layer **layers = NULL;

  char *dirname = NULL;

  GKeyFile *slidedat = NULL;

  bool success = false;
  char *tmp = NULL;

  // info about this slide
  char *slide_version = NULL;
  char *slide_id = NULL;
  int tiles_x = 0;
  int tiles_y = 0;

  char *index_filename = NULL;
  int zoom_levels = 0;
  int hier_count = 0;
  int nonhier_count = 0;
  int position_nonhier_offset = -1;

  int slide_zoom_level_value = -1;
  char *key_slide_zoom_level_name = NULL;
  char *key_slide_zoom_level_count = NULL;
  char **slide_zoom_level_section_names = NULL;
  struct slide_zoom_level_section *slide_zoom_level_sections = NULL;

  int datafile_count = 0;
  char **datafile_names = NULL;

  FILE *indexfile = NULL;

  // start reading

  // verify filename
  if (!g_str_has_suffix(filename, MRXS_EXT)) {
    goto FAIL;
  }

  // get directory from filename
  dirname = g_strndup(filename, strlen(filename) - strlen(MRXS_EXT));

  // first, check slidedat
  tmp = g_build_filename(dirname, SLIDEDAT_INI, NULL);
  slidedat = g_key_file_new();
  if (!g_key_file_load_from_file(slidedat, tmp, G_KEY_FILE_NONE, NULL)) {
    g_warning("Can't load Slidedat file");
    goto FAIL;
  }
  g_free(tmp);
  tmp = NULL;

  // load general stuff
  if (!g_key_file_has_group(slidedat, GROUP_GENERAL)) {
    g_warning("Can't find %s group", GROUP_GENERAL);
    goto FAIL;
  }

  READ_KEY_OR_FAIL(slide_version, slidedat, GROUP_GENERAL,
		   KEY_SLIDE_VERSION, value, "Can't read slide version");
  READ_KEY_OR_FAIL(slide_id, slidedat, GROUP_GENERAL,
		   KEY_SLIDE_ID, value, "Can't read slide id");
  READ_KEY_OR_FAIL(tiles_x, slidedat, GROUP_GENERAL,
		   KEY_IMAGENUMBER_X, integer, "Can't read tiles across");
  READ_KEY_OR_FAIL(tiles_y, slidedat, GROUP_GENERAL,
		   KEY_IMAGENUMBER_Y, integer, "Can't read tiles down");

  // load hierarchical stuff
  if (!g_key_file_has_group(slidedat, GROUP_HIERARCHICAL)) {
    g_warning("Can't find %s group", GROUP_HIERARCHICAL);
    goto FAIL;
  }

  READ_KEY_OR_FAIL(hier_count, slidedat, GROUP_HIERARCHICAL,
		   KEY_HIER_COUNT, integer, "Can't read hier count");
  READ_KEY_OR_FAIL(nonhier_count, slidedat, GROUP_HIERARCHICAL,
		   KEY_NONHIER_COUNT, integer, "Can't read nonhier count");

  // find key for slide zoom level
  for (int i = 0; i < hier_count; i++) {
    char *key = g_strdup_printf(KEY_HIER_d_NAME, i);
    char *value = g_key_file_get_value(slidedat, GROUP_HIERARCHICAL,
				       key, NULL);
    g_free(key);

    if (!value) {
      g_warning("Can't read value for hier name");
      goto FAIL;
    }

    if (strcmp(VALUE_SLIDE_ZOOM_LEVEL, value) == 0) {
      g_free(value);
      slide_zoom_level_value = i;
      key_slide_zoom_level_name = g_strdup_printf(KEY_HIER_d_NAME, i);
      key_slide_zoom_level_count = g_strdup_printf(KEY_HIER_d_COUNT, i);
      break;
    }
    g_free(value);
  }

  if (slide_zoom_level_value == -1) {
    g_warning("Can't find slide zoom level");
    goto FAIL;
  }

  // TODO allow slide_zoom_level_value to be at another hierarchy value
  if (slide_zoom_level_value != 0) {
    g_warning("Slide zoom level not HIER_0");
    goto FAIL;
  }

  READ_KEY_OR_FAIL(index_filename, slidedat, GROUP_HIERARCHICAL,
		   KEY_INDEXFILE, value, "Can't read index filename");
  READ_KEY_OR_FAIL(zoom_levels, slidedat, GROUP_HIERARCHICAL,
		   key_slide_zoom_level_count, integer, "Can't read zoom levels");

  slide_zoom_level_section_names = g_new0(char *, zoom_levels + 1);
  for (int i = 0; i < zoom_levels; i++) {
    tmp = g_strdup_printf(KEY_HIER_d_VAL_d_SECTION, slide_zoom_level_value, i);

    READ_KEY_OR_FAIL(slide_zoom_level_section_names[i], slidedat, GROUP_HIERARCHICAL,
		     tmp, value, "Can't read section name");

    g_free(tmp);
    tmp = NULL;
  }

  // load datafile stuff
  if (!g_key_file_has_group(slidedat, GROUP_DATAFILE)) {
    g_warning("Can't find %s group", GROUP_DATAFILE);
    goto FAIL;
  }

  READ_KEY_OR_FAIL(datafile_count, slidedat, GROUP_DATAFILE,
		   KEY_FILE_COUNT, integer, "Can't read datafile count");

  datafile_names = g_new0(char *, datafile_count + 1);
  for (int i = 0; i < datafile_count; i++) {
    tmp = g_strdup_printf(KEY_d_FILE, i);

    READ_KEY_OR_FAIL(datafile_names[i], slidedat, GROUP_DATAFILE,
		     tmp, value, "Can't read datafile name");

    g_free(tmp);
    tmp = NULL;
  }

  // load data from all slide_zoom_level_section_names sections
  slide_zoom_level_sections = g_new0(struct slide_zoom_level_section, zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;

    int bgr;

    char *group = slide_zoom_level_section_names[i];
    if (!g_key_file_has_group(slidedat, group)) {
      g_warning("Can't find %s group", group);
      goto FAIL;
    }

    READ_KEY_OR_FAIL(hs->overlap_x, slidedat, group, KEY_OVERLAP_X,
		     double, "Can't read overlap X");
    READ_KEY_OR_FAIL(hs->overlap_y, slidedat, group, KEY_OVERLAP_Y,
		     double, "Can't read overlap Y");
    READ_KEY_OR_FAIL(bgr, slidedat, group, KEY_IMAGE_FILL_COLOR_BGR,
		     integer, "Can't read image fill color");
    READ_KEY_OR_FAIL(hs->tile_w, slidedat, group, KEY_DIGITIZER_WIDTH,
		     integer, "Can't read tile width");
    READ_KEY_OR_FAIL(hs->tile_h, slidedat, group, KEY_DIGITIZER_HEIGHT,
		     integer, "Can't read tile height");

    // convert fill color bgr into argb
    hs->fill_argb =
      0xFF000000 |
      ((bgr << 16) & 0x00FF0000) |
      (bgr & 0x0000FF00) |
      ((bgr >> 16) & 0x000000FF);

    // verify we are JPEG
    READ_KEY_OR_FAIL(tmp, slidedat, group, KEY_IMAGE_FORMAT,
		     value, "Can't read image format");
    if (strcmp(tmp, "JPEG") != 0) {
      g_warning("Level %d not JPEG", i);
      goto FAIL;
    }
    g_free(tmp);
    tmp = NULL;

    // verify IMAGE_CONCAT_FACTOR == 1 for all but the first layer
    int ic_factor;
    READ_KEY_OR_FAIL(ic_factor, slidedat, group, KEY_IMAGE_CONCAT_FACTOR,
		     integer, "Can't read image concat factor");
    if ((i == 0) && (ic_factor != 0)) {
      g_warning("Level 0 has non-zero image concat factor: %d", ic_factor);
      goto FAIL;
    }
    if ((i != 0) && (ic_factor != 1)) {
      g_warning("Level %d has non-unity image concat factor: %d", i, ic_factor);
      goto FAIL;
    }
  }


  // load position stuff
  // find key for position
  int offset_tmp = 0;
  for (int i = 0; i < nonhier_count; i++) {
    char *key = g_strdup_printf(KEY_NONHIER_d_NAME, i);
    char *value = g_key_file_get_value(slidedat, GROUP_HIERARCHICAL,
				       key, NULL);
    g_free(key);

    if (!value) {
      g_warning("Can't read value for nonhier name");
      goto FAIL;
    }

    if (strcmp(VALUE_VIMSLIDE_POSITION_BUFFER, value) == 0) {
      g_free(value);
      position_nonhier_offset = offset_tmp;
      break;
    }

    // otherwise, increase offset
    g_free(value);
    key = g_strdup_printf(KEY_NONHIER_d_COUNT, i);
    int count = g_key_file_get_integer(slidedat, GROUP_HIERARCHICAL,
				       key, NULL);
    g_free(key);
    if (!count) {
      g_warning("Can't read nonhier val count");
      goto FAIL;
    }
    offset_tmp += count;
  }

  if (position_nonhier_offset == -1) {
    g_warning("Can't figure out where the position file is");
    goto FAIL;
  }

  g_debug("dirname: %s", dirname);
  g_debug("slide_version: %s", slide_version);
  g_debug("slide_id: %s", slide_id);
  g_debug("tiles (%d,%d)", tiles_x, tiles_y);
  g_debug("index_filename: %s", index_filename);
  g_debug("zoom_levels: %d", zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    g_debug(" section name %d: %s", i, slide_zoom_level_section_names[i]);
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;
    g_debug("  overlap_x: %g", hs->overlap_x);
    g_debug("  overlap_y: %g", hs->overlap_y);
    g_debug("  fill_argb: %" PRIu32, hs->fill_argb);
    g_debug("  tile_w: %d", hs->tile_w);
    g_debug("  tile_h: %d", hs->tile_h);
  }
  g_debug("datafile_count: %d", datafile_count);
  for (int i = 0; i < datafile_count; i++) {
    g_debug(" datafile name %d: %s", i, datafile_names[i]);
  }
  g_debug("position_nonheir_offset: %d", position_nonhier_offset);


  // read indexfile
  tmp = g_build_filename(dirname, index_filename, NULL);
  indexfile = fopen(tmp, "rb");
  g_free(tmp);
  tmp = NULL;

  if (!indexfile) {
    g_warning("Cannot open index file");
    goto FAIL;
  }

  // compute dimensions in stupid but clear way
  int64_t base_w = 0;
  int64_t base_h = 0;

  for (int i = 0; i < tiles_x; i++) {
    if (((i % 2) == 0) || (i == tiles_x - 1)) {
      // full size
      base_w += slide_zoom_level_sections[0].tile_w;
    } else {
      // size minus overlap
      base_w += slide_zoom_level_sections[0].tile_w - slide_zoom_level_sections[0].overlap_x;
    }
  }
  for (int i = 0; i < tiles_y; i++) {
    if (((i % 2) == 0) || (i == tiles_y - 1)) {
      // full size
      base_h += slide_zoom_level_sections[0].tile_h;
    } else {
      // size minus overlap
      base_h += slide_zoom_level_sections[0].tile_h - slide_zoom_level_sections[0].overlap_y;
    }
  }


  // set up layers
  layers = g_new(struct _openslide_jpeg_layer *, zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    struct _openslide_jpeg_layer *l = g_slice_new0(struct _openslide_jpeg_layer);
    layers[i] = l;
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;

    int divisor = 1 << i;

    l->tiles = _openslide_jpeg_create_tiles_table();
    l->layer_w = base_w / divisor;
    l->layer_h = base_h / divisor;
    l->tiles_across = tiles_x;
    l->tiles_down = tiles_y;
    l->raw_tile_width = hs->tile_w;
    l->raw_tile_height = hs->tile_h;

    // use half the overlap, so that our tile correction will flip between
    // positive and negative values typically
    // this is because only every other tile overlaps

    // overlaps are concatenated within physical tiles, so our virtual tile
    // size must shrink
    l->tile_advance_x = (((double) hs->tile_w) / ((double) divisor)) -
      ((double) hs->overlap_x / 2.0);
    l->tile_advance_y = (((double) hs->tile_h) / ((double) divisor)) -
      ((double) hs->overlap_y / 2.0);
  }

  // TODO load the position map and build up the tiles, using subtiles

  /*
  if (!(num_jpegs = process_indexfile(&jpegs,
						   slide_version,
						   slide_id,
						   position_nonhier_offset,
						   zoom_levels,
						   tiles_x,
						   tiles_y,
						   slide_zoom_level_sections,
						   dirname,
						   datafile_count,
						   datafile_names,
						   indexfile))) {
    goto FAIL;
  }
  */

  if (osr) {
    osr->fill_color_argb = slide_zoom_level_sections[0].fill_argb;
  }

  _openslide_add_jpeg_ops(osr, num_jpegs, jpegs, zoom_levels, layers);
  success = true;
  goto DONE;

 FAIL:
  for (int i = 0; i < num_jpegs; i++) {
    //    g_slice_free(struct _openslide_jpeg_fragment, jpegs[i]);
  }
  g_free(jpegs);

  success = false;

 DONE:
  g_free(dirname);
  g_free(tmp);
  g_free(slide_version);
  g_free(slide_id);
  g_free(index_filename);
  g_strfreev(datafile_names);
  g_strfreev(slide_zoom_level_section_names);
  g_free(slide_zoom_level_sections);
  g_free(key_slide_zoom_level_name);
  g_free(key_slide_zoom_level_count);

  if (slidedat) {
    g_key_file_free(slidedat);
  }
  if (indexfile) {
    fclose(indexfile);
  }

  return success;
}
