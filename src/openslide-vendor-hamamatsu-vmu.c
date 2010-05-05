/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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
 * Hamamatsu (VMU) support
 *
 * Steve Lamont <spl@ncmir.ucsd.edu>
 * National Center for Microscopy and Imaging Research
 * Center for Research in Biological Structure
 * University of California, San Diego
 * La Jolla, CA 92093-0715
 *
 * quickhash comes from VMU file and map2 file
 * 
 */


#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "openslide-hash.h"

static const char GROUP_VMU[] = "Uncompressed Virtual Microscope Specimen";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_NUM_LAYERS[] = "NoLayers";
static const char KEY_MACRO_IMAGE[] = "MacroImage";

static void add_properties(GHashTable *ht, GKeyFile *kf) {
  g_hash_table_insert(ht,
		      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
		      g_strdup("hamamatsu"));

  char **keys = g_key_file_get_keys(kf, GROUP_VMU, NULL, NULL);
  if (keys == NULL) {
    return;
  }

  for (char **key = keys; *key != NULL; key++) {
    char *value = g_key_file_get_value(kf, GROUP_VMU, *key, NULL);
    if (value) {
      g_hash_table_insert(ht,
			  g_strdup_printf("hamamatsu.%s", *key),
			  g_strdup(value));
      g_free(value);
    }
  }

  g_strfreev(keys);
}

static void add_macro_associated_image(GHashTable *ht, FILE *f) {
  _openslide_add_jpeg_associated_image(ht, "macro", f);
}

bool _openslide_try_hamamatsu_vmu(openslide_t *osr, const char *filename,
				  struct _openslide_hash *quickhash1)
{

  char *dirname = g_path_get_dirname(filename);
  char **image_filenames = NULL;
  struct _openslide_vmu_file **files = NULL;

  char **all_keys = NULL;

  bool success = false;
  char *tmp;

  // first, see if it's a VMU file
  GKeyFile *vmu_file = g_key_file_new();
  if (!g_key_file_load_from_file(vmu_file, filename, G_KEY_FILE_NONE, NULL)) {
    //    g_debug( "Can't load VMU file" );
    goto FAIL;
  }
  if (!g_key_file_has_group(vmu_file, GROUP_VMU)) {
    //    g_warning( "Can't find VMU group" );
    goto FAIL;
  }
  // hash in the VMU file
  _openslide_hash_file(quickhash1, filename);

  // this format has cols*rows vmu files, plus the map
  int num_files = 2;
  image_filenames = g_new0(char *, num_files);
  files = g_new0(struct _openslide_vmu_file *, num_files);


  // add properties
  if (osr) {
    add_properties(osr->properties, vmu_file);
  }

  // extract MapFile
  tmp = g_key_file_get_string(vmu_file, GROUP_VMU, KEY_MAP_FILE, NULL);
  if (tmp) {
    char *map_filename = g_build_filename(dirname, tmp, NULL);

    image_filenames[num_files - 1] = map_filename;
    struct _openslide_vmu_file *file =
      g_slice_new0(struct _openslide_vmu_file);
    files[num_files - 1] = file;

    // hash in the map file
    _openslide_hash_file(quickhash1, map_filename);

    g_free(tmp);
  } else {
    g_warning("Can't read map file");
    goto FAIL;
  }


  // now the ImageFile
  all_keys = g_key_file_get_keys(vmu_file, GROUP_VMU, NULL, NULL);
  char **tmp2;
  for (tmp2 = all_keys; *tmp2 != NULL; tmp2++) {
    char *key = *tmp2;
    char *value = g_key_file_get_string(vmu_file, GROUP_VMU, key, NULL);

    //    g_debug( "%s", key );

    if (strncmp(KEY_IMAGE_FILE, key, strlen(KEY_IMAGE_FILE)) == 0) {

      image_filenames[0] = g_build_filename(dirname, value, NULL);

      files[0] = g_slice_new0(struct _openslide_vmu_file);

    }
    g_free(value);
  }

  // check image filenames ( the others are sort of optional )
  for (int i = 0; i < num_files; i++) {
    if (!image_filenames[i]) {
      g_warning("Can't read image filename %d", i);
      goto FAIL;
    }
  }

  // open files

  for (int i = 0; i < num_files; i++) {

    struct _openslide_vmu_file *vf = files[i];

    vf->filename = g_strdup(image_filenames[i]);

    FILE *f;
    if ((f = fopen(vf->filename, "rb")) == NULL) {
      g_warning("Can't open VMU image file %s", vf->filename);
      goto FAIL;
    }


    fseeko(f, 0, SEEK_END);
    vf->end_in_file = ftello(f);
    if (vf->end_in_file == -1) {
      g_warning("Can't read file size for VMU image file %s", vf->filename);
      fclose(f);
      goto FAIL;
    }

    bool flipped;
    u_int16_t key;
    fseeko(f, 0, SEEK_SET);
    if (!fread(&key, sizeof(key), 1, f)) {

      g_warning("Unexpected end of file reading header of %s.", vf->filename);
      fclose(f);
      goto FAIL;

    } else if (memcmp((void *) &key, (void *) "NG", 2) == 0)
      flipped = false;
    else if (memcmp((void *) &key, (void *) "GN", 2) == 0)
      flipped = true;
    else {

      g_warning("%s does not seem to be an NGR file.", vf->filename);
      fclose(f);
      goto FAIL;

    }

    fseeko(f, 4, SEEK_SET);
    if (!fread(&vf->w, sizeof(vf->w), 1, f)) {

      g_warning("Unexpected end of file reading header of %s.", vf->filename);
      fclose(f);
      goto FAIL;

    }

    if (!fread(&vf->h, sizeof(vf->h), 1, f)) {

      g_warning("Unexpected end of file reading header of %s.", vf->filename);
      fclose(f);
      goto FAIL;

    }

    if (!fread(&vf->chunksize, sizeof(vf->chunksize), 1, f)) {

      g_warning("Unexpected end of file reading header of %s.", vf->filename);
      fclose(f);
      goto FAIL;

    }

    int32_t header_size;
    fseeko(f, 24, SEEK_SET);
    if (!fread(&header_size, sizeof(header_size), 1, f)) {

      g_warning("Unexpected end of file reading header of %s.", vf->filename);
      fclose(f);
      goto FAIL;

    }

    vf->start_in_file = header_size;

    // file is done now
    fclose(f);

    int32_t wchunks = vf->w / vf->chunksize;

    vf->chunk_table = (int64_t **) g_malloc(sizeof(int64_t) * vf->h);
    vf->chunk_table[0] =
      (int64_t *) g_malloc(sizeof(int64_t) * vf->h * wchunks);
    for (int32_t o = 1; o < vf->h; o++)
      vf->chunk_table[o] = vf->chunk_table[o - 1] + wchunks;

    int64_t I = 0;
    int64_t J = 0;

    for (int64_t from = 0; from < vf->w * vf->h; from += vf->chunksize) {

      vf->chunk_table[J][I / vf->chunksize] =
	(from * 3 * sizeof(unsigned short)) + vf->start_in_file;

      J++;
      if (J == vf->h) {

	J = 0;
	I += vf->chunksize;

      }

    }

  }

  // add macro image if present
  if (osr) {

    tmp = g_key_file_get_string(vmu_file, GROUP_VMU, KEY_MACRO_IMAGE, NULL);
    if (tmp) {

      char *macro_filename = g_build_filename(dirname, tmp, NULL);
      FILE *macro_f = fopen(macro_filename, "rb");
      if (macro_f) {
	add_macro_associated_image(osr->associated_images, macro_f);
	fclose(macro_f);
      }
      g_free(macro_filename);
      g_free(tmp);

    }

  }

  _openslide_add_vmu_ops(osr, quickhash1, num_files, files);
  success = true;
  goto DONE;

FAIL:
  if (files) {
    for (int i = 0; i < num_files; i++) {
      g_free(files[i]->filename);
    }
    g_free(files);
  }

  success = false;

DONE:
  g_strfreev(all_keys);
  g_free(dirname);

  if (image_filenames) {
    for (int i = 0; i < num_files; i++) {
      g_free(image_filenames[i]);
    }
    g_free(image_filenames);
  }
  g_key_file_free(vmu_file);

  return success;
}
