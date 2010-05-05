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
    for ( int i = 0; i < data->file_count; i++ ) {

	g_free( data->files[i]->filename );
	g_free( data->files[i]->chunk_table[0] );
	g_free( data->files[i]->chunk_table );
	g_free( data->files[i] );

    }
    g_slice_free(struct _openslide_vmuopsdata, data);
}

static void destroy(openslide_t *osr) {
    struct _openslide_vmuopsdata *data = osr->data;
    destroy_data(data);
}


static void get_dimensions_unlocked(openslide_t *osr, int32_t layer,
				    int64_t *w, int64_t *h) {
    
    struct _openslide_vmuopsdata *data = osr->data;
    struct _openslide_vmu_file *vmu_file = data->files[layer];
    
    *w = vmu_file->w;
    *h = vmu_file->h;

}

static void get_dimensions(openslide_t *osr, int32_t layer,
			   int64_t *w, int64_t *h) {
    struct _openslide_vmuopsdata *data = osr->data;
    
    g_mutex_lock(data->vmu_mutex);
    get_dimensions_unlocked(osr, layer, w, h);
    g_mutex_unlock(data->vmu_mutex);
}

static void paint_region_unlocked( openslide_t *osr, cairo_t *cr,
				   int64_t x, int64_t y,
				   int32_t layer,
				   int32_t w, int32_t h )

{

    struct _openslide_vmuopsdata *data = osr->data;
    struct _openslide_vmu_file *vmu_file = data->files[layer];
    int64_t **chunk_table = vmu_file->chunk_table;
    
    int f = open( vmu_file->filename, O_RDONLY );

    int pagesize = getpagesize();

    // Get the initial offset.

    int64_t xc = x / vmu_file->chunksize;
    int64_t offset = chunk_table[y][xc];

    // Get the page boundary.

    int64_t page_offset = ( offset / pagesize ) * pagesize;
    int64_t length = vmu_file->end_in_file - page_offset;

    // If the length is greater than the maximum allowable memory
    // mapping, then clamp the length to that maximum.  Note that this
    // value is set to half of that allowed by the address space, to
    // allow for other data.  There's still a possiblity that this can
    // blow up if we're dealing with a very large file on a 32 bit
    // system.  This probably requires a bit of thought to get right.

    if ( length > ( int64_t ) MAX_MMAP )
	length = MAX_MMAP;

    unsigned char *map = ( unsigned char *) mmap( 0, ( size_t ) length,
						  PROT_READ, MAP_PRIVATE, 
						  f, ( off_t ) page_offset );

    unsigned char *imagedata = 
	g_slice_alloc( ( int64_t ) w * ( int64_t ) h * ( int64_t ) 4 );
    
    for ( int64_t j = y, J = 0; J < h; j++, J++ ) {

	for ( int64_t i = x, I = 0; I < w; i++, I++ ) {

	    int64_t image_offset = ( ( J * w ) + I ) * 4;

	    if ( ( 0 <= i ) && ( i < vmu_file->w ) &&
		 ( 0 <= j ) && ( j < vmu_file->h ) ) {

		int64_t ic = i / vmu_file->chunksize;
		int64_t o = chunk_table[j][ic];
		
		if ( o < page_offset ) {
		    
		    munmap( map, length ); // Drop the old page chunk.
		    
		    page_offset = ( o / pagesize ) * pagesize;
		    length = vmu_file->end_in_file - page_offset;
		    if ( length > ( int64_t ) MAX_MMAP )
			length = MAX_MMAP;
		    
		    map = ( unsigned char *) mmap( 0, ( size_t ) length,
						   PROT_READ, MAP_PRIVATE, 
						   f, ( off_t ) page_offset );
		    
		}
		int64_t loc = 
		    ( o - page_offset ) +
		    ( ( i % vmu_file->chunksize ) * sizeof( unsigned short ) * 3 );
		
		/*
		 * Note: we can't just assign the pointer since we don't
		 * know whether it's on a short integer boundary or not
		 * and references to an odd address may cause a
		 * segmentation violation on most architectures.  Thus, we
		 * must copy the data to an array.
		 */
		
		unsigned short rgb[3];
		memcpy( ( void *) &rgb[0],
			map + loc,
			sizeof( unsigned short ) * 3 );
	    
		imagedata[image_offset + 0] = rgb[2] >> 4;
		imagedata[image_offset + 1] = rgb[1] >> 4;
		imagedata[image_offset + 2] = rgb[0] >> 4;
		imagedata[image_offset + 3] = 0xff;

	    } else
		imagedata[image_offset + 0] =
		    imagedata[image_offset + 1] =
		    imagedata[image_offset + 2] =
		    imagedata[image_offset + 3] = 0x00;
    
	}

    }
    munmap( map, length );	/* Drop the mapping. */
    close( f );

    cairo_surface_t *surface = 
	cairo_image_surface_create_for_data( ( unsigned char *) imagedata,
					     CAIRO_FORMAT_ARGB32,
					     w, h, w * 4 );
    cairo_save(cr);
    cairo_translate(cr, 0, 0 );
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_surface_destroy(surface);
    cairo_paint(cr);
    
    cairo_restore(cr);

    g_slice_free( unsigned char *, imagedata );

}

static void paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 int32_t layer,
			 int32_t w, int32_t h) {
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

void _openslide_add_vmu_ops( openslide_t *osr,
			     struct _openslide_hash *quickhash1,
			     int32_t file_count,
			     struct _openslide_vmu_file **files )

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
    _openslide_hash_file( quickhash1, files[0]->filename );
    
    // store vmu-specific data into osr
    g_assert(osr->data == NULL);
    
    // general osr data
    osr->layer_count = file_count;
    osr->data = data;
    osr->ops = &_openslide_vmu_ops;
}
