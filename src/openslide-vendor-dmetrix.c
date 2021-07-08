/*
 * openslide-vendor-dmetrix.c
 *
 *  Created on: Sep 27, 2018
 *      Author: lmq
 *
 *  Dmetrix(.dmetrix) support
 *
 */
#define BUFSIZE (64 << 10)
#include "openslide-private.h"
#include "openslide-decode-gdkpixbuf.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

struct load_state {
  int32_t w;
  int32_t h;
  GdkPixbuf *pixbuf;  // NULL until validated, then a borrowed ref
  GError *err;
};

static const char DMETRIX_EXT[] = ".dmetrix";

static const int D_HEAD_LEN = 392; // fix head len
static const int FIX_LAYER_COUNT = 20; // fix layer count
static const int FIX_LAYER_INDEX_LEN = 14; // fix layer index len
static const int FIX_IMAGE_INDEX_LEN = 22; // fix layer index len
static const int FIX_TILE_WIDTH = 256;
static const int FIX_TILE_HEIGHT = 256;
static const int FIX_DOWNSAMPLE_BASE = 2; // fix downsample base is 2

struct index_of_image{
    short int layerId;
	int col;
	int row;
	long long image_pos;
	unsigned int len;
};

struct index_of_index{
	short int layerId;
	int maxCol;
	int maxRow;
	unsigned int target_layer_pos;

	struct index_of_image **image_map;
};

struct dmetrix_info{
	int max_layer;
	struct index_of_index *index_map;

	char* filePath;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  int index_in_dmetrix;
};

struct associated_image {
  struct _openslide_associated_image base;
  uint32_t *img;
};

static bool read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      int64_t tile_col, int64_t tile_row,
		      void *arg,
		      GError **err) {
	struct level *l = (struct level *)level;
	struct dmetrix_info *d_info = (struct dmetrix_info *)osr->data;

	// get image
	struct index_of_index indexIndex = d_info->index_map[l->index_in_dmetrix];
	struct index_of_image indexImage = indexIndex.image_map[tile_row][tile_col];
	long long offset = indexImage.image_pos;
	unsigned int len = indexImage.len;
	int iw,ih;
	void *buffer = g_malloc(len);
	FILE *f = _openslide_fopen(d_info->filePath, "rb", err);
	if (f == NULL) {
		goto FAIL;
	}
	if (offset && fseeko(f, offset, SEEK_SET) == -1) {
	    _openslide_io_error(err, "Cannot seek to offset");
	    fclose(f);
	    goto FAIL;
	  }
	if(!fread(buffer,sizeof(char),len,f))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
												"Get Image buffer from file:%s in offset:%s error", d_info->filePath,offset);
		fclose(f);
		goto FAIL;
	}
	fclose(f);
	if(!_openslide_jpeg_decode_buffer_dimensions(buffer,len,&iw,&ih,err))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
										"Get Image dimensions from file:%s in offset:%s error", d_info->filePath,offset);
		goto FAIL;
	}
	// cache
	struct _openslide_cache_entry *cache_entry;
//	uint32_t *dest = g_slice_alloc(iw * ih * 4);
	uint32_t *dest = _openslide_cache_get(osr->cache,
			level,tile_col,tile_row,
			&cache_entry);
	if(!dest)
	{
		dest = g_slice_alloc(iw * ih * 4);
		bool result = _openslide_jpeg_decode_buffer(buffer,len,dest,iw,ih,err);
		if(!result){
			g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
									"decode Image from file:%s in offset:%s error", d_info->filePath,offset);
			g_slice_free1(iw * ih * 4,dest);
			goto FAIL;
		}
		// put it in the cache
		_openslide_cache_put(osr->cache, level, tile_col, tile_row,
				dest, iw * ih * 4,
				 &cache_entry);
	}
	g_free(buffer);

	// draw it
	cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) dest,
									 CAIRO_FORMAT_ARGB32,
									 iw, ih,
									 iw * 4);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_surface_destroy(surface);
	cairo_paint(cr);

    // done with the cache entry, release it
    _openslide_cache_entry_unref(cache_entry);

	return true;

FAIL:
	g_free(buffer);
	return false;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct level *l = (struct level *) level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h,
                                      err);
}

static void destroy(openslide_t *osr) {
	struct level **levels = (struct level **) osr->levels;
	struct dmetrix_info *d_info = (struct dmetrix_info *)osr->data;
	if (levels) {
	    for (int32_t i = 0; i < osr->level_count; i++) {
	      if (levels[i]) {
	        _openslide_grid_destroy(levels[i]->grid);
	        g_slice_free(struct level, levels[i]);
	      }
	    }
	    g_free(levels);
	  }
	if(d_info)
	{
		for(int i = 0; i < FIX_LAYER_COUNT; i++)
		{
			int rows = d_info->index_map[i].maxRow + 1;
			if(rows > 0 && d_info->index_map[i].image_map){
				for(int r = 0; r < rows; r++)
				{
					g_free(d_info->index_map[i].image_map[r]);
				}
				g_free(d_info->index_map[i].image_map);
			}
		}
		g_free(d_info->index_map);
		g_slice_free(struct dmetrix_info,d_info);
	}
}

static const struct _openslide_ops dmetrix_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool dmetrix_detect(const char *filename G_GNUC_UNUSED,
        struct _openslide_tifflike *tl, GError **err)
{
	// Is a tiff
	if(tl)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
				"Is a TIFF file");
		return false;
	}
	// verify filename
	if(!g_str_has_suffix(filename, DMETRIX_EXT))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
						"File dose not have %s extension", DMETRIX_EXT);
		return false;
	}
	// verify existence
	if(!g_file_test(filename, G_FILE_TEST_EXISTS))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
								"File dose not exist", DMETRIX_EXT);
		return false;
	}
	return true;
}

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dest,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;
  memcpy(dest, img->img, img->base.w * img->base.h * sizeof(uint32_t));
  return true;
}

static void destroy_associated_image(struct _openslide_associated_image *_img) {
  struct associated_image *img = (struct associated_image *) _img;

  g_free(img->img);
  g_slice_free(struct associated_image, img);
}

static const struct _openslide_associated_image_ops jpeg_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

static void area_prepared_dmetrix(GdkPixbufLoader *loader, void *data) {
  struct load_state *state = data;

  if (state->err) {
    return;
  }

  GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);

  // validate image parameters
  // when adding RGBA support, note that gdk-pixbuf does not
  // premultiply alpha
  if (gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
      gdk_pixbuf_get_has_alpha(pixbuf) ||
      gdk_pixbuf_get_n_channels(pixbuf) != 3) {
    g_set_error(&state->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported pixbuf parameters");
    return;
  }
  state->w = gdk_pixbuf_get_width(pixbuf);
  state->h = gdk_pixbuf_get_height(pixbuf);

  // commit
  state->pixbuf = pixbuf;
}

static bool add_associated_image(openslide_t *osr,
		const char *name,
		const char *filename,
		int64_t offset,
		uint32_t length,
		GError **err)
{
	GdkPixbufLoader *loader = NULL;
	uint8_t *buf = g_slice_alloc(BUFSIZE);
	bool success = false;
	struct load_state state = {
			.err = NULL,
	};

	// open and seek
	FILE *f = _openslide_fopen(filename, "rb", err);
	if(!f){
		goto DONE;
	}
	if(fseeko(f, offset, SEEK_SET)){
		_openslide_io_error(err, "Couldn't fssek %s", filename);
		goto DONE;
	}
	// create loader
	loader = gdk_pixbuf_loader_new_with_type("bmp",err);
	if (!loader) {
	    goto DONE;
	}
	g_signal_connect(loader, "area-prepared", G_CALLBACK(area_prepared_dmetrix), &state);
	// read data
	while (length) {
		size_t count = fread(buf, 1, MIN(length, BUFSIZE), f);
		if (!count) {
			g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
					  "Short read loading pixbuf from %s", filename);
		  	goto DONE;
		}
		if (!gdk_pixbuf_loader_write(loader, buf, count, err)) {
			g_prefix_error(err, "gdk-pixbuf error: ");
		  	goto DONE;
		}
		if (state.err) {
			goto DONE;
		}
		length -= count;
	}

	// finish load
	if (!gdk_pixbuf_loader_close(loader, err)) {
	g_prefix_error(err, "gdk-pixbuf error: ");
	goto DONE;
	}
	if (state.err) {
	goto DONE;
	}
	g_assert(state.pixbuf);

	struct associated_image *img = g_slice_new0(struct associated_image);
	img->base.ops = &jpeg_associated_ops;
	img->base.w = state.w;
	img->base.h = state.h;
	img->img = g_malloc(state.w * state.h * 4);

	// copy pixels
	uint8_t *pixels = gdk_pixbuf_get_pixels(state.pixbuf);
	int rowstride = gdk_pixbuf_get_rowstride(state.pixbuf);
	for (int32_t y = 0; y < state.h; y++) {
	  for (int32_t x = 0; x < state.w; x++) {
		  img->img[y * state.w + x] = 0xFF000000 |                              // A
	                      pixels[y * rowstride + x * 3 + 0] << 16 | // R
	                      pixels[y * rowstride + x * 3 + 1] << 8 |  // G
	                      pixels[y * rowstride + x * 3 + 2];        // B
	  }
	}

	g_hash_table_insert(osr->associated_images, g_strdup(name), img);

	success = true;
 DONE:
	// clean up
	if (loader) {
	  gdk_pixbuf_loader_close(loader, NULL);
	  g_object_unref(loader);
	}
	if (f) {
	  fclose(f);
	}
	g_slice_free1(BUFSIZE, buf);

	// now that the loader is closed, we know state.err won't be set
	// behind our back
	if (state.err) {
	  // signal handler validation errors override GdkPixbuf errors
	  g_clear_error(err);
	  g_propagate_error(err, state.err);
	  // signal handler errors should have been noticed before falling through
	  g_assert(!success);
	}
	return success;
}

static bool add_associated_image_jpeg(openslide_t *osr,
		const char *name,
		const char *filename,
		int64_t offset,
		uint32_t length,
		GError **err)
{
	uint8_t *buf = g_slice_alloc(length);
	bool success = false;

	// open and seek
	FILE *f = _openslide_fopen(filename, "rb", err);
	if(!f){
		goto DONE;
	}
	if(fseeko(f, offset, SEEK_SET)){
		_openslide_io_error(err, "Couldn't fssek %s", filename);
		goto DONE;
	}

	// read data
	if(!fread(buf,sizeof(char),length,f))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
							  "failed read loading buf from %s", filename);
				  	goto DONE;
	}

	// uncompress jpeg
	int iw,ih;
	if(!_openslide_jpeg_decode_buffer_dimensions(buf,length,&iw,&ih,err))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
												"Get Image dimensions from file:%s in offset:%s error", filename,offset);
		goto DONE;
	}
	uint32_t *dest = g_malloc(iw * ih * 4);
	if(!_openslide_jpeg_decode_buffer(buf,length,dest,iw,ih,err))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
														"decode Image from file:%s in offset:%s error", filename,offset);
				goto DONE;
	}

	struct associated_image *img = g_slice_new0(struct associated_image);
	img->base.ops = &jpeg_associated_ops;
	img->base.w = iw;
	img->base.h = ih;
	img->img = dest;

	g_hash_table_insert(osr->associated_images, g_strdup(name), img);

	success = true;
 DONE:
	// clean up
	if (f) {
	  fclose(f);
	}
	g_slice_free1(length, buf);
	return success;
}

static bool stream_is_BMP(const char *filename,int64_t offset,GError **err)
{
	char *buf = g_slice_alloc(2);
	bool success = false;
	// open and seek
	FILE *f = _openslide_fopen(filename, "rb", err);
	if(!f){
		goto DONE;
	}
	if(fseeko(f, offset, SEEK_SET)){
		_openslide_io_error(err, "Couldn't fssek %s", filename);
		goto DONE;
	}

	// read data
	if(!fread(buf,sizeof(char),2,f))
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
							  "failed read 2 bytes to judge stream is BMP from %s", filename);
		goto DONE;
	}

	if(buf[0] == '\x42' && buf[1] == '\x4D')
	{
		success = true;
	}
DONE:
	// clean up
	if (f) {
	  fclose(f);
	}
	g_slice_free1(2, buf);
	return success;
}

static bool dmetrix_open(openslide_t *osr, const char *filename,
                       struct _openslide_tifflike *tl G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1, GError **err) {
	struct level **levels = NULL;
	int d_imageHeight;
	int d_imageWidth;
	int d_scanScale;
	double d_micronPrePixel_x;
	double d_micronPrePixel_y;
	int64_t d_label_offset,d_thumb_offset;
	uint32_t d_label_length,d_thumb_length;

	short d_maxLayer;

	FILE *f = _openslide_fopen(filename, "rb", err);
	if(!f)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
		                "Cannot open the dmetrix file");
		return false;
	}
	void *buffer = g_malloc(D_HEAD_LEN);
	if(fread(buffer,sizeof(char),D_HEAD_LEN,f) != (size_t)D_HEAD_LEN)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
		                "Error while reading dmetrix head data");
		g_free(buffer);
		fclose(f);
		return false;
	}
	// analyse head info
	int head_seek = 0;
    // companyName len 7
	head_seek += 7;
	// encrppt len 1;
	head_seek += 1;
	// device name
	head_seek += 10;
	// scan date
	head_seek += 8;
	// image width
	memcpy(&d_imageWidth,buffer + head_seek, 4);
	head_seek += 4;
	// image height
	memcpy(&d_imageHeight,buffer + head_seek, 4);
	head_seek += 4;
	// file head len
	head_seek += 4;
	// file len
	head_seek += 8;
	// max layer
	memcpy(&d_maxLayer,buffer + head_seek, 2);
	head_seek += 2;
	// micron Pre Pixel_x
	memcpy(&d_micronPrePixel_x,buffer + head_seek, 8);
	head_seek += 8;
	// micron Pre Pixel_y
	memcpy(&d_micronPrePixel_y,buffer + head_seek, 8);
	head_seek += 8;
	// scan scale
	memcpy(&d_scanScale,buffer + head_seek, 4);
	head_seek += 4;

	// add property to openslide
	g_hash_table_insert(osr->properties,
			g_strdup("dmetrix.AppMag"),
			g_strdup_printf("%d",d_scanScale));
	g_hash_table_insert(osr->properties,
			g_strdup("dmetrix.MPP_X"),
				g_strdup_printf("%f",d_micronPrePixel_x));
	g_hash_table_insert(osr->properties,
			g_strdup("dmetrix.MPP_Y"),
				g_strdup_printf("%f",d_micronPrePixel_y));
	_openslide_duplicate_int_prop(osr, "dmetrix.AppMag",
	                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
	_openslide_duplicate_double_prop(osr, "dmetrix.MPP_X",
	                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
	_openslide_duplicate_double_prop(osr, "dmetrix.MPP_Y",
	                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

	int d_effectiveLevelsCount = 0;
	int image_count = 0;
	int row_count,col_count;
	struct index_of_index temp_index_of_index;
	struct dmetrix_info *d_info = g_slice_new0(struct dmetrix_info);
	d_info->filePath = g_strdup(filename);
        d_info->max_layer = d_maxLayer;
	d_info->index_map = g_new0(struct index_of_index, FIX_LAYER_COUNT);
	for(int i = 0; i < FIX_LAYER_COUNT; i++)
	{
		memcpy(&temp_index_of_index.layerId,buffer+head_seek,2);
		head_seek += 2;
		memcpy(&temp_index_of_index.maxCol,buffer+head_seek,4);
		head_seek += 4;
		memcpy(&temp_index_of_index.maxRow,buffer+head_seek,4);
		head_seek += 4;
		memcpy(&temp_index_of_index.target_layer_pos,buffer+head_seek,4);
		head_seek += 4;

		if(temp_index_of_index.maxRow > 0 && temp_index_of_index.maxCol > 0)
		{
			d_effectiveLevelsCount += 1;
			row_count = temp_index_of_index.maxRow + 1;
			col_count = temp_index_of_index.maxCol + 1;
			image_count += row_count * col_count;
			d_info->index_map[temp_index_of_index.layerId] = temp_index_of_index;
			d_info->index_map[temp_index_of_index.layerId].image_map = g_new0(struct index_of_image, row_count);
			for(int j = 0; j < row_count; j++)
			{
				d_info->index_map[temp_index_of_index.layerId].image_map[j] = g_new0(struct index_of_image, col_count);
			}
		}
	}
	// label offset
	head_seek = 348;
	head_seek += 10;
	memcpy(&d_label_offset,buffer + head_seek, 8);
	head_seek += 8;
	memcpy(&d_label_length,buffer + head_seek, 4);
	head_seek += 4;
	// thumb offset
	head_seek += 10;
	memcpy(&d_thumb_offset,buffer + head_seek, 8);
	head_seek += 8;
	memcpy(&d_thumb_length,buffer + head_seek, 4);
	head_seek += 4;

	int image_seek = 0;
	int image_indexs_size = image_count * FIX_IMAGE_INDEX_LEN;
	void *buffer_image_indexs = g_malloc(image_indexs_size);
	fseeko(f, D_HEAD_LEN, SEEK_SET);
	if(fread(buffer_image_indexs,sizeof(char),image_indexs_size,f) != (size_t)image_indexs_size)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			             "Error while reading dmetrix image index data");
		g_free(buffer);
		g_free(buffer_image_indexs);
		fclose(f);
		return false;
	}
	struct index_of_image temp_index_of_image;
	for(int i = 0; i < image_count; i++)
	{
		memcpy(&temp_index_of_image.layerId,buffer_image_indexs+image_seek,2);
		image_seek += 2;
		memcpy(&temp_index_of_image.col,buffer_image_indexs+image_seek,4);
		image_seek += 4;
		memcpy(&temp_index_of_image.row,buffer_image_indexs+image_seek,4);
		image_seek += 4;
		memcpy(&temp_index_of_image.image_pos,buffer_image_indexs+image_seek,8);
		image_seek += 8;
		memcpy(&temp_index_of_image.len,buffer_image_indexs+image_seek,4);
		image_seek += 4;
		if(d_maxLayer - 1 - temp_index_of_image.layerId < d_effectiveLevelsCount)
		{
			d_info->index_map[temp_index_of_image.layerId].image_map[temp_index_of_image.row][temp_index_of_image.col] = temp_index_of_image;
		}
	}
	// complete _openslide_t
	int index_in_openslide = 0;
	levels = g_new0(struct level *, d_effectiveLevelsCount);
	for(int i = d_maxLayer - 1; i > d_maxLayer - 1 - d_effectiveLevelsCount ; i--)
	{
		index_in_openslide = d_maxLayer - 1 - i;
		struct level *l = g_slice_new0(struct level);
		l->base.downsample = (index_in_openslide == 0? 1 : pow(FIX_DOWNSAMPLE_BASE, index_in_openslide));
		l->base.w = d_imageWidth / l->base.downsample;
		l->base.h = d_imageHeight / l->base.downsample;
		l->base.tile_w = FIX_TILE_WIDTH;
		l->base.tile_h = FIX_TILE_HEIGHT;
		l->index_in_dmetrix = i;
		levels[index_in_openslide] = l;

		l->grid = _openslide_grid_create_simple(osr,
                (l->base.w + FIX_TILE_WIDTH - 1) / FIX_TILE_WIDTH,
                (l->base.h + FIX_TILE_HEIGHT - 1) / FIX_TILE_HEIGHT,
				FIX_TILE_WIDTH,
				FIX_TILE_HEIGHT,
                read_tile);
	}
	osr->level_count = d_effectiveLevelsCount;
	osr->levels = (struct _openslide_level **) levels;
	osr->data = d_info;
	osr->ops = &dmetrix_ops;
	levels = NULL;
	fclose(f);

	// label and thumb image
	// label
	bool result;
	if(d_label_offset > 0 && d_label_length > 0)
	{
		if(stream_is_BMP(filename,d_label_offset,err))
		{
			result = add_associated_image(osr,
							"label",
							filename,
							d_label_offset,
							d_label_length,
							err);
		}
		else
		{
			result = add_associated_image_jpeg(osr,
							"label",
							filename,
							d_label_offset,
							d_label_length,
							err);
		}
		if(!result)
		{
			g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
									 "Error while add dmetrix label image to hashtable");
		}
	}
	if(d_thumb_offset > 0 && d_thumb_length > 0)
	{
		if(stream_is_BMP(filename,d_thumb_offset,err))
		{
			result = add_associated_image(osr,
							"thumbnail",
							filename,
							d_thumb_offset,
							d_thumb_length,
							err);
		}
		else
		{
			result = add_associated_image_jpeg(osr,
							"thumbnail",
							filename,
							d_thumb_offset,
							d_thumb_length,
							err);
		}
		if(!result)
		{
			g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
									 "Error while add dmetrix thumbnail image to hashtable");
		}
	}

	g_free(buffer);
	g_free(buffer_image_indexs);

    return true;
}

const struct _openslide_format _openslide_format_dmetrix = {
		.name = "dmetrix",
		.vendor = "dmetrix",
		.detect = dmetrix_detect,
		.open = dmetrix_open,
};

