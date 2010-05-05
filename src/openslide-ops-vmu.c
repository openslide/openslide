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

#include <config.h>

#include "openslide-private.h"

#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <glib.h>
#include <inttypes.h>
#include <cairo.h>

#include "openslide-hash.h"

#define MAX_MMAP ( ( int64_t ) SIZE_MAX / 2 )

struct _openslide_vmuopsdata {

  GMutex *vmu_mutex;

  int32_t file_count;
  struct _openslide_vmu_file **files;

};


static void destroy_data(struct _openslide_vmuopsdata *data) {
  g_mutex_free(data->vmu_mutex);
  for (int i = 0; i < data->file_count; i++) {

    g_free(data->files[i]->filename);
    g_free(data->files[i]->chunk_table[0]);
    g_free(data->files[i]->chunk_table);
    g_free(data->files[i]);

  }
  g_slice_free(struct _openslide_vmuopsdata, data);
}

static void destroy(openslide_t * osr) {
  struct _openslide_vmuopsdata *data = osr->data;
  destroy_data(data);
}


static void get_dimensions_unlocked(openslide_t * osr, int32_t layer,
				    int64_t * w, int64_t * h) {

  struct _openslide_vmuopsdata *data = osr->data;
  struct _openslide_vmu_file *vmu_file = data->files[layer];

  *w = vmu_file->w;
  *h = vmu_file->h;

}

static void get_dimensions(openslide_t * osr, int32_t layer,
			   int64_t * w, int64_t * h) {
  struct _openslide_vmuopsdata *data = osr->data;

  g_mutex_lock(data->vmu_mutex);
  get_dimensions_unlocked(osr, layer, w, h);
  g_mutex_unlock(data->vmu_mutex);
}

static void paint_region_unlocked(openslide_t * osr, cairo_t * cr,
				  int64_t x, int64_t y,
				  int32_t layer, int32_t w, int32_t h)
{

  struct _openslide_vmuopsdata *data = osr->data;
  struct _openslide_vmu_file *vmu_file = data->files[layer];
  int64_t **chunk_table = vmu_file->chunk_table;

  FILE *f = fopen(vmu_file->filename, "rb");

  // Get the initial offset.

  int64_t xc = x / vmu_file->chunksize;
  int64_t offset = chunk_table[y][xc];

  unsigned char *imagedata =
    g_slice_alloc((int64_t) w * (int64_t) h * (int64_t) 4);
  memset(imagedata, 0x00, (int64_t) w * (int64_t) h * (int64_t) 4);

  int64_t pixelbytes = 3 * sizeof(unsigned short);
  int64_t chunkbytes = vmu_file->chunksize * pixelbytes;
  unsigned short *buffer = (unsigned short *) g_slice_alloc(chunkbytes);

  int64_t ct = -1;
  for (int64_t j = y, J = 0; J < h; j++, J++) {

    for (int64_t i = x, I = 0; I < w; i++, I++) {

      int64_t cto = chunk_table[j][i / vmu_file->chunksize];

      if (cto != ct) {

	fseek(f, cto, SEEK_SET);
	fread(buffer, chunkbytes, 1, f);
	ct = cto;

      }
      int64_t image_offset = ((J * w) + I) * 4;

      if ((0 <= i) && (i < vmu_file->w) && (0 <= j) && (j < vmu_file->h)) {

	int64_t loc = (i % vmu_file->chunksize) * 3;

	imagedata[image_offset + 0] = *(buffer + loc + 2) >> 4;
	imagedata[image_offset + 1] = *(buffer + loc + 1) >> 4;
	imagedata[image_offset + 2] = *(buffer + loc + 0) >> 4;
	imagedata[image_offset + 3] = 0xff;

      }

    }

  }

  fclose(f);

  cairo_surface_t *surface =
    cairo_image_surface_create_for_data((unsigned char *) imagedata,
					CAIRO_FORMAT_ARGB32,
					w, h, w * 4);
  cairo_save(cr);
  cairo_translate(cr, 0, 0);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  cairo_restore(cr);

  g_slice_free(unsigned char *, imagedata);
  g_slice_free(unsigned short *, buffer);

}

static void paint_region(openslide_t * osr, cairo_t * cr,
			 int64_t x, int64_t y,
			 int32_t layer, int32_t w, int32_t h) {
  struct _openslide_vmuopsdata *data = osr->data;

  g_mutex_lock(data->vmu_mutex);
  paint_region_unlocked(osr, cr, x, y, layer, w, h);
  g_mutex_unlock(data->vmu_mutex);
}


static const struct _openslide_ops _openslide_vmu_ops = {
  .get_dimensions = get_dimensions,
  .paint_region = paint_region,
  .destroy = destroy
};

void _openslide_add_vmu_ops(openslide_t * osr,
			    struct _openslide_hash *quickhash1,
			    int32_t file_count,
			    struct _openslide_vmu_file **files)
{

  if (osr == NULL) {
    return;
  }
  // allocate private data
  struct _openslide_vmuopsdata *data =
    g_slice_new(struct _openslide_vmuopsdata);

  data->file_count = file_count;
  data->files = files;

  // populate private data
  data->vmu_mutex = g_mutex_new();

  // generate hash of the smallest layer
  _openslide_hash_file(quickhash1, files[0]->filename);

  // store vmu-specific data into osr
  g_assert(osr->data == NULL);

  // general osr data
  osr->layer_count = file_count;
  osr->data = data;
  osr->ops = &_openslide_vmu_ops;
}
