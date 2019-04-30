/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  VMIC Driver:
 *  Copyright (c) 2016-2017 Markus Pöpping
 *  All rights reserved.
 *
 *  GTIF Driver:
 *  Copyright (c) 2018-2019 Markus Pöpping
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
 * Precipoint (vmic) support
 * vmic consist of a deepzoom pyramid inside a zip container,
 * which itself is in a zip container
 * currently, only jpeg images are supported
 * extension to jp2k and png images is planned long term
 *
 * quickhash comes from the binary data of the xml property file and
 * the raw image data of the image in largest single-tiled level
 *
 */

#define ppdebug(...) fprintf(stderr,__VA_ARGS__)

#define SUPPORTS_GTIF 1

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-xml.h"
#include "openslide-hash.h"
#include "openslide-decode-jpeg.h"
//#include "openslide-decode-png.h"
#include "openslide-decode-zip.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>


#ifdef SUPPORTS_GTIF
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-jp2k.h"
#include <math.h>
#include <tiffio.h>
#endif

/* VMIC constants */
const char _PRECIPOINT_VENDOR[] = "precipoint";
const char _PRECIPOINT_VMICTYPE[] = "M8-VMIC";

// either one of the following two is the name of the inner
const char _PRECIPOINT_INNER_CONTAINER_NAME[] = "Image.vmici";
const char _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME[] = "Image";

const char _PRECIPOINT_MACRO_IMAGE[] = "VMCF/overview.jpg";
const char _PRECIPOINT_PROPS_FILENAME[] = "VMCF/config.osc";
const char _PRECIPOINT_PROPS_OSC_NODE[] = "ObjectScanConfig";
const char _PRECIPOINT_PROPS_PREFIX[] = "PreciPoint";
const char _PRECIPOINT_PROPPATH_MAGNIFICATION[] = "PreciPoint.Objective.Magnification";
const char _PRECIPOINT_PROPPATH_NAME[] = "PreciPoint.ScanData.Name";

// max number of open zip handles
#define VMIC_HC_MAX_QUEUE_COUNT 32

// max accumulated direcory size per queue in MB
#define VMIC_HC_MAX_PARALLEL_SIZE 45
// note: this limits the number of parallel slides opened
// it's an estimation of zip directory memory estimated from the (known) slide size
// the directory size is roughly 0.5% of slide size,
// e.g. 60 MB for a 13 GB slide, plus the malloc overhead

/* deepzoom constants */
//const char _DEEPZOOM_XML_FILE[] = "dzc_output.xml";
//const char _DEEPZOOM_FOLDER[] = "dzc_output_files";
const char _DEEPZOOM_PROP_IMAGE_NODE[] = "Image";
const char _DEEPZOOM_PROP_PPM[] = "PixelPerMicron";
const char _DEEPZOOM_PROP_TILESIZE[] = "TileSize";
const char _DEEPZOOM_PROP_SIZE_NODE[] = "Size";
const char _DEEPZOOM_PROP_WIDTH[] = "Width";
const char _DEEPZOOM_PROP_HEIGHT[] = "Height";
const char _DEEPZOOM_PROP_OVERLAP[] = "Overlap";
const char _DEEPZOOM_PROP_IMAGE_FORMAT[] = "Format";

//#define g_debug(...) fprintf(stderr, __VA_ARGS__)

enum image_format {
  IMAGE_FORMAT_UNKNOWN=0,
  IMAGE_FORMAT_JPG,
  IMAGE_FORMAT_PNG,
  IMAGE_FORMAT_BMP,
  IMAGE_FORMAT_JP2
};

/* deep zoom parameters */
struct dzinfo {
  int32_t dz_level_count;  // total number of levels in DZ structure
  int32_t tilesize;        // DZ XML parameter: size of tiles
  int32_t overlap;         // DZ XML parameter: overlap
  int64_t width, height;   // DZ XML parameter: total dimensions of slide
#define MAX_IFSTR 10
  gchar tile_imgformat_str[MAX_IFSTR+1];  // DZ XML parameter: type of image (jpg/png/bmp)
  enum image_format tile_format_id;       // ID for image type, derived from above

  int32_t os_level_count;    // level count as exposed via openslide after cutoff
  int32_t dz_one_tile_level; // largest level in DZ structure that consists of a single tile

/* file and folder names for deepzoom tile tree, no path included*/
#define MAX_DZFILENAME 50
  gchar key_filename[MAX_DZFILENAME+1];
  gchar folder_name[MAX_DZFILENAME+1];
};

struct vmic_handlecache {
  char *filename;
  zip_int64_t inner_index;
//  char *inner_filename;
  GQueue *cache;
  GMutex *lock;
  GCond *cond;
  int outstanding;
  int instance_count;
  int instance_max;
};

struct vmic_handle {
  struct vmic_handlecache *ref_vhc;
  zip_t *outer;
  zip_t *inner;
  //int64_t offset;
  //int64_t size;
};

/* vmic slide information, includes deep zoom parameters as base class */
struct vmicinfo {
  struct dzinfo dz;
  struct vmic_handlecache *archive;
};

static struct vmic_handle* vmic_handle_new( struct vmic_handlecache *vc,
                                            GError **err) {
  struct vmic_handle* vh = g_slice_new0(struct vmic_handle);
  if (vh) {
    vh->ref_vhc = vc;
    zip_t *zo = _openslide_zip_open_archive(vc->filename, err);
    if (!zo) {
      goto FAIL;
    }
    zip_source_t *zs = zip_source_zip(zo, zo, vc->inner_index, 0, 0, 0);
    if (!zs) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
            "vmic_handle_new: cannot create zip source, filename=%s, index=%i",
            vc->filename, (int)vc->inner_index);
      zip_close(zo);
      goto FAIL;
    }

    vh->inner = _openslide_zip_open_archive_from_source(zs, err);
    if (!vh->inner) {
      g_prefix_error( err, 
                      "vmic_handle_new: cannot open inner archive, reason: ");
      zip_source_close(zs);
      zip_close(zo);
      goto FAIL;
    }
    vh->outer = zo;
  }
  return vh;
  
FAIL:
  g_slice_free(struct vmic_handle, vh);
  return NULL;
}

static void vmic_handle_delete(struct vmic_handle *vh) {
  zip_close(vh->inner);
  zip_close(vh->outer);
  g_slice_free(struct vmic_handle, vh);
}

static struct vmic_handlecache* vmic_handlecache_create(const char *filename,
                                                   zip_int64_t inner_index,
                                                   uint64_t inner_size) {
  struct vmic_handlecache *vc = g_slice_new0(struct vmic_handlecache);
  if (vc) {
    vc->filename = g_strdup(filename);
    vc->inner_index = inner_index;
    vc->cache = g_queue_new();
    vc->instance_count = 0;
    vc->instance_max = 1 + ((uint64_t) VMIC_HC_MAX_PARALLEL_SIZE
                                       * 175 * 1000000 / inner_size );
    vc->outstanding = 0;
    //g_debug("creating vmic_handlecache file=%s, inner index=%i, inner_size=%li, instance_max=%i\n",
    //        filename, (int)inner_index, (long int)inner_size, (int)vc->instance_max);
    vc->lock = g_mutex_new();
    vc->cond = g_cond_new();
  }
  return vc;
}

static struct vmic_handle* vmic_handle_get(struct vmic_handlecache *vc,
                                           GError **err) {
  //g_debug("get vmici zip");
  struct vmic_handle *vh;
  g_mutex_lock(vc->lock);
  // get handle from cache - if none is free, 
  // create new ones till vc->size_meter > VMIC_HC_MAX_ENTRY_COUNT
  // then wait for one to get released
  
  while ( (vh = g_queue_pop_head(vc->cache)) == NULL
       && vc->instance_count >= vc->instance_max ) {
      
      //g_debug("waiting for g_queue_push signal instance_count=%i, outstanding=%i...", (int) vc->instance_count, (int) vc->outstanding);
      g_cond_wait(vc->cond, vc->lock);
      //g_debug("..wait ended\n");
  }
  vc->outstanding++;
  g_mutex_unlock(vc->lock);

  if (vh == NULL) {
    g_mutex_lock(vc->lock);
    vc->instance_count++;
    g_mutex_unlock(vc->lock);

    //g_debug("call to vmic_handle_new, instance_count=%i\n", vc->instance_count);
    vh = vmic_handle_new(vc, err);
  }
  if (vh == NULL) { // fail case
    g_mutex_lock(vc->lock);
    vc->outstanding--;
    g_mutex_unlock(vc->lock);
  }
  
  return vh;
}

static void vmic_handle_put(struct vmic_handlecache *vc,
                            struct vmic_handle *vh) {
  if (vh == NULL || vc == NULL) {
    return;
  }

  //g_debug("put vmic\n");
  g_mutex_lock(vc->lock);
  g_assert(vc->outstanding);
  vc->outstanding--;
  if (g_queue_get_length(vc->cache) < VMIC_HC_MAX_QUEUE_COUNT) {
    g_queue_push_head(vc->cache, vh);
    vh = NULL;
  }
  g_cond_signal(vc->cond);
  g_mutex_unlock(vc->lock);

  if (vh) {
    //g_debug("too many vmic handles in queue, deleting one\n");
    g_mutex_lock(vc->lock);
    vc->instance_count--;
    g_mutex_unlock(vc->lock);
    vmic_handle_delete(vh);
  }
}

static void vmic_handlecache_destroy(struct vmic_handlecache *vc) {
  if (vc == NULL) {
    return;
  }
  g_assert(vc->outstanding == 0);
  g_mutex_lock(vc->lock);
  struct vmic_handle *vh;
  while ((vh = g_queue_pop_head(vc->cache)) != NULL) {
    //g_debug("delete vmic handle\n");
    vc->instance_count--;
    vmic_handle_delete(vh);
  }
  g_mutex_unlock(vc->lock);
  g_assert(vc->instance_count == 0);
  g_queue_free(vc->cache);
  g_mutex_free(vc->lock);
  g_cond_free(vc->cond);
  g_free(vc->filename);
  g_slice_free(struct vmic_handlecache, vc);
}


/* Params of each OS level */
struct dz_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
  int dz_level_id;
  int cols;
  int rows;
};

// calculate expected with or height of a tile
static inline int calc_expected_tile_dim(int full_length, int tile_size,
                                         int overlap, int tile_pos) {
  int count = (full_length + tile_size - 1) / tile_size;

  if (tile_pos >= count) {
    return 0;
  }
  int a = tile_size;
  if (tile_pos == count-1) {
    a = full_length % tile_size;
    if (a == 0) {
      a = tile_size;
    }
  }
  if (tile_pos > 0) {
    a += overlap;
  }
  if (tile_pos < count-1) {
    a += overlap;
  }
  return a;
}

// Helper function: decodes an image from memory into an RGBA buffer
// The dimensions of the image are returned by reference and the buffer with the image data 
// is newly allocated with g_slice_alloc
// Buffer must be freed after use with g_slice_free w*h*4

static bool _openslide_decode_image( gpointer compressed_data,
                                     gsize compressed_size,
                                     enum image_format dzif,
                                     uint32_t **pdestbuf,
                                     int32_t *pw,
                                     int32_t *ph,
                                     GError **err) {

  bool success;
  int32_t dw,dh;
  uint32_t *rgba_buf = NULL; // decoded image data

  *pdestbuf = NULL; // defaults
  *pw = -1; *ph = -1;

  if (dzif==IMAGE_FORMAT_JPG) {
    
    success = _openslide_jpeg_decode_buffer_dimensions(compressed_data,
                                                       compressed_size,
                                                       &dw, &dh, err);
        
    if (success) {
      //g_debug("jpeg size %i,%i\n", (int)dw, (int)dh);

      // note: this must be the g_slice_alloc
      //       for compatibility with _openslide_cache
      rgba_buf = g_slice_alloc((gsize) dw * dh * 4);
          
      if (rgba_buf) {
        success = _openslide_jpeg_decode_buffer(compressed_data,
                                                compressed_size,
                                                rgba_buf,
                                                dw, dh,
                                                err);
        if (success) {
          *pdestbuf = rgba_buf;
          *pw = dw; *ph = dh;
        }
      }
      else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
           "_openslide_decode_image: can't allocate buffer for decoded image");
      }
    }
  }
  else if (dzif==IMAGE_FORMAT_PNG) {
    // to add PNG support, we would require _openslide_png_decode_buffer(buf, buflen, dest, dw, dh, err)
    // until now, PNG is not used by VMIC
    success = false;
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "_openslide_decode_image: no PNG support yet");
  }
  else {
    // BMP is not used. So far only JPG based VMICs exist
    // there's some chance we may need to add JP2K support for future VMICs
    success = false;
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "_openslide_decode_image: unknown image format %i", (int)dzif);
  }
  return success;
}


/* read deepzoom tile and paint it to cairo context */
static bool vmic_read_tile(openslide_t *osr,
                           cairo_t *cr,
                           struct _openslide_level *level,
                           int64_t tile_col, int64_t tile_row,
                           void *arg G_GNUC_UNUSED,
                           GError **err) {

  struct dz_level *lev = (struct dz_level *) level;
  struct vmicinfo *vmic = (struct vmicinfo*) osr->data;

  //get tile size
  int32_t tw = calc_expected_tile_dim(lev->base.w, vmic->dz.tilesize,
                                      vmic->dz.overlap, tile_col);
  int32_t th = calc_expected_tile_dim(lev->base.h, vmic->dz.tilesize,
                                      vmic->dz.overlap, tile_row);

  //g_debug("requested tile: col=%i, row=%i, w=%i, h=%i\n",
  //        (int)tile_col, (int)tile_row, (int)tw, (int)th );

  /* try to fetch from cache */
  struct _openslide_cache_entry *cache_entry;

  uint32_t *tiledata = _openslide_cache_get( osr->cache,
                                             level, 
                                             tile_col, tile_row,
                                             &cache_entry);
  
  if (!tiledata) { // in case of cache miss, retrieve tile from ZIP archive
    gchar *tilefilename = NULL;

    struct vmic_handle *vh = vmic_handle_get(vmic->archive, err);
    if (vh) {
      bool success = true;
      tilefilename = g_strdup_printf("%s/%i/%i_%i.%s",
                            vmic->dz.folder_name, (int)lev->dz_level_id,
                            (int)tile_col, (int)tile_row,
                            vmic->dz.tile_imgformat_str );

      // caution: don't use ZIP_FL_NOCASE when searching by name, because it is slow
      zip_int64_t zipx = _openslide_zip_name_locate(vh->inner,
                                                    tilefilename,
                                                    ZIP_FL_ENC_RAW);

      gpointer cbuf;  // coded image data
      gsize cbufsize;

      if (zipx != -1) {
        // note: getting an index "-1" means the tile is missing,
        //       and must be rendered as a blank area
        
        if (! _openslide_zip_read_file_data( vh->inner, zipx,
                                             &cbuf, &cbufsize, err ) ) {
          g_prefix_error(err, "Error in vmic_read_tile: ");
          success = false;
        } 
      }

      // return handle to queue ASAP, so other threads can access
      vmic_handle_put(vmic->archive, vh);
        
      // note: &tiledata returns a buffer allocated with g_slice_alloc
      //       and needs to be deallocated by the caller, in this case
      //       this will be done by _openslide_cache_entry_unref()

      if (zipx != -1 && success) {
        int32_t w,h;
        success = _openslide_decode_image( cbuf, cbufsize, IMAGE_FORMAT_JPG,
                                           &tiledata, &w, &h, err );

        g_slice_free1(cbufsize, cbuf);

        if (!success) {
          g_prefix_error(err, "Error in vmic_read_tile: ");
        }

        // check if the tile has expected dimensions
        if (success && (tw != w || th != h)) {
          g_slice_free1((gsize) w * h * 4, tiledata);
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
             "vmic_read_tile: size mismatch of tile %s. expected size=(%i,%i),"
             " stored tile=(%i,%i).",
             tilefilename, (int)tw, (int)th, (int)w, (int)h);
          success = false;
        }
      }
      
      g_free(tilefilename);
      
      if (!success) {
        // something has failed during above procedure
        return false;
      }

      // reading tile was successful - put it in the cache
      // note: put creates a cache refcount
      if (tiledata) {
        _openslide_cache_put( osr->cache, level,
                              tile_col, tile_row,
                              tiledata, (int)tw * th * 4,
                              &cache_entry);
      }
    }
    
  }

  // draw it

  if (tiledata)  {
    // use pixel data as image source
    cairo_surface_t *surface = cairo_image_surface_create_for_data (
                                    (unsigned char *) tiledata,
                                    CAIRO_FORMAT_ARGB32,
                                    tw, th, tw * 4 );

    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_surface_destroy(surface);
    cairo_paint(cr);

    // normal tile is always cached --
    // done with the cache entry, release it
    _openslide_cache_entry_unref(cache_entry);
  }
  else {
    // no tile at this position => don't need to do anything
    // because cairo surface is pre-filled with transparent pixels
    // Also, empty tiles are not cached.
  }

  return true;
}


static bool vmic_paint_region( G_GNUC_UNUSED openslide_t *osr, cairo_t *cr,
                               int64_t x, int64_t y,
                               struct _openslide_level *level,
                               int32_t w, int32_t h,
                               GError **err) {
    
  struct dz_level *lev = (struct dz_level *) level;

  //g_debug("call to paint_region: cairo=%p x=%i, y=%i, w=%i, h=%i, level-ds=%lf\n", (void*)cr, (int)x, (int)y, (int)w, (int)h, l->base.downsample);

  return _openslide_grid_paint_region( lev->grid, cr, NULL,
                                       x / lev->base.downsample,
                                       y / lev->base.downsample,
                                       level, w, h,
                                       err);
}

/* Helper function: find node by name in a node chain, returns NULL
 * if not found. This does not recurse branches */
static xmlNode* _openslide_xml_find_node(xmlNode *node, const xmlChar* name) {
  xmlNode* cur = node;
  while (cur != NULL) {
    if(xmlStrcmp(cur->name, name) == 0) {
      break;
    }
    cur = cur->next;
  }
  return cur;
}

/*  Retrieve deep zoom properties from xml doc 

    Expected XML structure to fetch as deepzoom parameters - everything else is ignored.

    (*)Image
        TileSize (int)
        PixelPerMicron (double)
        Overlap (int)
        (*)Size
            Width (int)
            Height (int)
        Format (string), contains "jpg"|"png"|"bmp"
*/
static bool dzz_get_deepzoom_properties(xmlDoc *xmldoc, struct dzinfo *dzi,
                                        GHashTable *properties, GError **err) {
  bool success = false;

  xmlNode *xmlnodeImage = _openslide_xml_find_node( xmldoc->children,
                                         (xmlChar*) _DEEPZOOM_PROP_IMAGE_NODE);
  if (xmlnodeImage == NULL) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: cannot find XML %s Element",_DEEPZOOM_PROP_IMAGE_NODE);
    goto FAIL;
  }

  double ppm = _openslide_xml_parse_double_attr( xmlnodeImage,
                                                 _DEEPZOOM_PROP_PPM, err);
  if (*err) goto FAIL;

  if (ppm == 0) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: Cannot retrieve MPP property");
    goto FAIL;
  }

  success = g_hash_table_insert( properties,
                                 g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                                 _openslide_format_double(1.0 / ppm) );
  if (!success) goto FAIL;

  success = g_hash_table_insert( properties,
                                 g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                                 _openslide_format_double(1.0 / ppm) );
  if (!success) goto FAIL;

  dzi->tilesize = _openslide_xml_parse_int_attr( xmlnodeImage,
                                        g_strdup(_DEEPZOOM_PROP_TILESIZE),
                                        err);
  dzi->overlap = _openslide_xml_parse_int_attr( xmlnodeImage,
                                        g_strdup(_DEEPZOOM_PROP_OVERLAP),
                                        err);

  if (dzi->tilesize <= 0 || dzi->overlap < 0 || dzi->overlap >= dzi->tilesize/2) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: Invalid Overlap %i and/or tilesize %i !",
                 dzi->overlap, dzi->tilesize);
    goto FAIL;
  }

  xmlNode *xmlnodeSize = _openslide_xml_find_node(xmlnodeImage->children, (xmlChar*)_DEEPZOOM_PROP_SIZE_NODE);
  
  if (xmlnodeSize == NULL)  {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: cannot find XML %s Element", _DEEPZOOM_PROP_SIZE_NODE);
    goto FAIL;
  }

  dzi->width = _openslide_xml_parse_int_attr(xmlnodeSize, g_strdup(_DEEPZOOM_PROP_WIDTH), err);
  dzi->height = _openslide_xml_parse_int_attr(xmlnodeSize, g_strdup(_DEEPZOOM_PROP_HEIGHT), err);
  if (dzi->width <= 0 || dzi->height <= 0) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: invalid dimensions (w=%i x h=%i)\n",
                 (int)dzi->width, (int)dzi->height);
    goto FAIL;
  }
  if (*err) {
    goto FAIL;
  }

  xmlChar *str_pictype = xmlGetProp(xmlnodeImage,
                                    (const xmlChar*) _DEEPZOOM_PROP_IMAGE_FORMAT);
  if (!str_pictype) {
    goto FAIL;
  }
  //g_debug("w=%i, h=%i, tsize=%i, ovl=%i, format=%s\n", (int)dzi->width, (int)dzi->height, (int)dzi->tilesize, (int)dzi->overlap, str_pictype);

  g_strlcpy(dzi->tile_imgformat_str, (const gchar*) str_pictype, MAX_IFSTR);
  dzi->tile_format_id = IMAGE_FORMAT_UNKNOWN;
  if (str_pictype != NULL) {
    if ( g_ascii_strcasecmp((const gchar*) str_pictype,"jpg") == 0
         || g_ascii_strcasecmp((const gchar*) str_pictype,"jpeg") == 0 ) {
      dzi->tile_format_id = IMAGE_FORMAT_JPG;
    }
    else if (g_ascii_strcasecmp((const gchar*) str_pictype,"png") == 0) {
      dzi->tile_format_id = IMAGE_FORMAT_PNG;
    }
    else if (g_ascii_strcasecmp((const gchar*)str_pictype,"jp2") == 0) {
      dzi->tile_format_id = IMAGE_FORMAT_JP2;
    }
  }

  if (dzi->tile_format_id == IMAGE_FORMAT_UNKNOWN) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DZC/XML: Cannot recognize Image format \"%s\".", str_pictype);
    xmlFree(str_pictype);
    goto FAIL;
  }
  xmlFree(str_pictype);
  
  if (dzi->tile_format_id != IMAGE_FORMAT_JPG) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: at this stage, only tiles with JPG image format can be processed.");
    goto FAIL;
  }
  /* calculate number of levels of deep zoom pyramid */
  int longside = MAX(dzi->width, dzi->height);
  int cnt_level = 1;
  dzi->os_level_count = 0;
  /* calc by bit shifting (result is similar to formula "ceil(log(longside)/log(2))") */
  g_assert(longside>0);  
  while (longside > 1) {
    if (dzi->os_level_count == 0 && longside <= dzi->tilesize) {
      // also search for largest one-tiled level
      dzi->os_level_count = cnt_level;
    }
    longside = (longside+1) >> 1;
    cnt_level++;
  }
  dzi->dz_level_count = cnt_level;
  dzi->dz_one_tile_level = cnt_level - dzi->os_level_count;
  //g_debug("DZ: dz level count=%i, os level count=%i, tilesize=%i\n", dzi->dz_level_count, dzi->os_level_count, dzi->tilesize);
  return true;

FAIL:
  return false;
}


// This function searches for the .xml or .dzi file that contains
// information about the deepzoom pyramid inside a zip archive.
// if found, "true" is returned and 
// the dzinfo structure receives the zip-index to the XML file and the name of the folder

static bool dzz_find_key_file(zip_t *z, struct dzinfo *dzi) 
{
  zip_int64_t key_file_id = -1;
  zip_int64_t count = zip_get_num_entries(z,0);
  const char *name;

  for (zip_int64_t i=0; i<count; i++) {
    name = zip_get_name(z, i, ZIP_FL_ENC_RAW);
    // search for a file with xml or dzi suffix
    if ( !strchr(name,'\\') && !strchr(name,'/')) {
      if ( g_str_has_suffix(name, ".dzi") || g_str_has_suffix(name, ".xml")
           || g_str_has_suffix(name, ".DZI") || g_str_has_suffix(name, ".XML"))
      {
          //g_debug("found deepzoom key file, name=%s\n", name);
          key_file_id = i;
          break;
      }
    }
  }

  if (key_file_id >= 0) {
    // store results in dz members
    g_strlcpy(dzi->key_filename, name, MAX_DZFILENAME-2);
    // compose corresponding folder name by replacing the ".xml" suffix with "_files"
    g_strlcpy(dzi->folder_name, name, MAX_DZFILENAME-2);
    strcpy(dzi->folder_name + strlen(dzi->folder_name) - 4, "_files");
    return true;
  }
  else {
    return false;
  }
}

/* check if this is a vmic file */
/* returns index of inner container */
/* -1 means no vmic */
static bool vmic_try_init( const char *vmic_filename, zip_int64_t *inner_index,
                           uint64_t *inner_size, GError **err) {
  zip_error_t ze;
  zip_error_init(&ze);
  zip_int64_t vmici_index;
  zip_stat_t zstat;
  zip_stat_init(&zstat);

  // check file ending
  gchar* fn_nocase = g_utf8_casefold(vmic_filename, -1);
  bool has_vmic_suffix = g_str_has_suffix(fn_nocase, ".vmic");
  g_free(fn_nocase);
  if ( !has_vmic_suffix ) {
    return false;
  }
  
  // open outer archive
  // note: zip_open checks the magic bytes "PK34" before doing anything else
  zip_t *zo = _openslide_zip_open_archive(vmic_filename, err);
  if (!zo) {
    return false;
  }

  // look for inner container by filename
  vmici_index = _openslide_zip_name_locate( zo,
                                            _PRECIPOINT_INNER_CONTAINER_NAME,
                                            ZIP_FL_ENC_RAW | ZIP_FL_NOCASE);
  if (vmici_index < 0) {
    vmici_index = _openslide_zip_name_locate( zo,
                                              _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME,
                                              ZIP_FL_ENC_RAW | ZIP_FL_NOCASE);
  }
  //g_debug("found inner container file, name=%s, index=%i\n", zip_get_name(zo, vmici_index, ZIP_FL_ENC_RAW), (int)vmici_index);
  
  if (vmici_index >= 0) {
    //check "magic number" of the file in question by reading first 4 bytes
    zip_file_t *file = zip_fopen_index(zo, vmici_index, 0);
    if (file) {
      uint32_t file_magic=0;
      zip_fread(file, &file_magic, 4);
      zip_fclose(file);
      
      //g_debug("checking magic of inner container=%x\n", (int)file_magic);

      if (file_magic == 0x04034B50) { // "PK34" (The rare "PK00PK" signature cannot occur)
        zip_stat_index(zo, vmici_index, 0, &zstat);        
        //g_debug("magic ok, inner_size=%li\n", (long int)zstat.size);
      }
      else {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
            "A file with the correct name was found but the magic number %x didn't match expectations.", file_magic);
        vmici_index = -1;
        //g_debug("magic not ok, proceeding anyway\n");
      }
    } else {
      g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "Inner image container not found. Name should be \"%s\" or \"%s\".",
              _PRECIPOINT_INNER_CONTAINER_NAME,
              _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME );
      vmici_index = -1;
    }
  }

  // close it, we have enough information now
  // it will be reopened by vmic_handlecache_get
  if (zo) {
    _openslide_zip_close_archive(zo);
  }
  //
  if (inner_index) { *inner_index = vmici_index; }
  if (inner_size) { *inner_size = zstat.size; }
  return (vmici_index >= 0);
}

// Helper function: Loads an XML file from ZIP archive and calls libxml for parsing.
// optionally, the raw data can be mangled into the quickhash.
static xmlDoc *_openslide_zip_parse_xml_file(zip_t *z,
                                      const char *filename,
                                      zip_flags_t flags,
                                      GError **err,
                                      struct _openslide_hash *hash) {

  zip_int64_t xml_file_id;
  bool success;
  xmlDoc *xmldoc = NULL;
  gpointer xmlbuf = NULL;
  gsize xmlsize = 0;

  // fetch & parse XML
  xml_file_id = _openslide_zip_name_locate(z, filename, flags);
  if (xml_file_id == -1) {
    zip_error_t *ze = zip_get_error(z);
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot locate XML description file \"%s\" in container."
                " libzip message:\"%s\"", filename, zip_error_strerror(ze) );
    goto FINISH;
  }

  //g_debug("ready to parse xml_file_id=%i, xmlbuf=%p, xmlsize=%i\n", (int)xml_file_id, (void*)xmlbuf, (int)xmlsize);
  success = _openslide_zip_read_file_data(z, xml_file_id,
                                          &xmlbuf, &xmlsize, err);
  if (!success) {
    g_prefix_error( err, 
                    "Cannot access VMIC XML description file \"%s\" - reason:",
                    filename);
    goto FINISH;
  }
  if (hash) {
    _openslide_hash_data(hash, xmlbuf, xmlsize);
  }
  ppdebug("XML:%s\n",xmlbuf);
  xmldoc = xmlReadMemory( xmlbuf, xmlsize, NULL, NULL,
                          XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET
                        /*| XML_PARSE_COMPACT*/ | XML_PARSE_NOBLANKS);
  if (!xmldoc) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot parse XML description file \"%s\"", filename);
    goto FINISH_AND_FREE;
  }

FINISH_AND_FREE:
  g_slice_free1(xmlsize, xmlbuf);
FINISH:
  return xmldoc;
}

// Helper function: recursively iterate over xml branch & leaves and dump stuff into properties
// output properties are formatted as "SpecialTag.NodeA.NodeB.Attrib = [Value]"
static void vmic_convert_xml_tree_to_properties(xmlNode *node,
                                                GHashTable *os_properties,
                                                const char *propname_prefix) {
  while (node != NULL) {
    if (node->type == XML_ELEMENT_NODE) {
      gchar *elementname = g_strconcat(propname_prefix, ".", node->name, NULL);
      for (xmlAttr* attribute = node->properties;
             attribute != NULL;
               attribute = attribute->next) {
        xmlChar* value = xmlGetProp(node, attribute->name);
        gchar *propname = g_strconcat(elementname, ".", attribute->name, NULL);
        ppdebug("XML_ELEMENT_NODE property=%s value=\"%s\"\n", propname, value);
        g_hash_table_insert(os_properties, propname, g_strdup((gchar*) value));
        xmlFree(value);
      }
      if (node->children) {
        vmic_convert_xml_tree_to_properties(node->children,
                                            os_properties, elementname);
      }
      g_free (elementname);
    }
    else if (node->type == XML_TEXT_NODE) {
      xmlChar *content = xmlNodeGetContent(node);
      ppdebug("XML_TEXT_NODE nodename=%s, content=\"%s\"\n", (char*) node->name, (char*)content);
      // trim CR/spaces from text node in place
#define trimspace(c) ((c)==' '||c=='\r'||c=='\n'||c=='\t')
      xmlChar *tc = content;
      xmlChar c;
        while ( (c=*tc)!='\0' && trimspace(c) ) {
        tc++;
        ppdebug("trimming '%02x', new length=%i\n", c, strlen(tc));
      }
      int e=strlen(tc);
      while (e>0) {
        c=tc[--e];
        if (!trimspace(c)) break;
        tc[e]='\0';
        ppdebug("trimming '%02x', new length=%i\n", c, strlen(tc));
      }
      if (!*tc) {
        ppdebug("nothing left after trimspace\n");
      } else {
        ppdebug("XML_TEXT_NODE property=%s value=\"%s\"\n", (char*)propname_prefix, (char*)tc);
        g_hash_table_insert(os_properties,
                          g_strdup((gchar*) propname_prefix),
                          g_strdup((gchar*) tc));
      }
      xmlFree(content);
    }
    node = node->next;
  }
}

/* parses all properties specific to VMIC file */
static bool vmic_get_properties(openslide_t *osr, zip_t *z,
                                struct _openslide_hash *quickhash,
                                GError **err) {
  bool success;
  xmlDoc *xmldoc;
  struct dzinfo *dzi = (struct dzinfo *) osr->data;
  
  // Parse DEEPZOOM properties
  dzz_find_key_file(z, dzi);
  //g_debug("deepzoom key file=%s, folder=%s\n", dzz->key_filename, dzz->folder_name);
  xmldoc = _openslide_zip_parse_xml_file(z, dzi->key_filename,
                                         ZIP_FL_ENC_RAW, err, quickhash);
  if (!xmldoc) {
    return false;
  }
  success = dzz_get_deepzoom_properties(xmldoc, dzi, osr->properties, err);
  //g_debug("deepzoom levels=%i, one_tile_level=%i\n", dzz->dz_level_count, dzz->dz_one_tile_level);
  xmlFreeDoc(xmldoc);
  if (!success) {
    return false;
  }
  if (dzi->overlap != 0) { 
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: DZ Overlap parameter is %i, but as of now (3/2017), "
    		     "VMIC tiles are not expected to overlap !",
                 dzi->overlap);
    return false;
  }

  // Parse VMIC properties from VMCF/config.osc
  xmldoc = _openslide_zip_parse_xml_file( z, _PRECIPOINT_PROPS_FILENAME, 
                                          ZIP_FL_ENC_RAW, err,
                                          quickhash );
  if (!xmldoc) {
    return false;
  }
  xmlNode *oscconfig = _openslide_xml_find_node(
                                       xmldoc->children,
                                       (xmlChar*) _PRECIPOINT_PROPS_OSC_NODE);
  if (!oscconfig) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "OSC/XML: Can't find OSC node (%s) for params in conf file.",
                 _PRECIPOINT_PROPS_OSC_NODE);
    xmlFreeDoc(xmldoc);
    return false;
  }
  vmic_convert_xml_tree_to_properties( oscconfig->children,
                                       osr->properties,
                                       _PRECIPOINT_PROPS_PREFIX);
  xmlFreeDoc(xmldoc);

  // copy magnification to "openslide.objective-power"
  _openslide_duplicate_double_prop( osr, _PRECIPOINT_PROPPATH_MAGNIFICATION, 
                                    OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER );
  // copy title/name to "openslide.comment"
  const char *value = g_hash_table_lookup( osr->properties,
                                           _PRECIPOINT_PROPPATH_NAME);
  if (value) {
    g_hash_table_insert( osr->properties,
                         g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
                         g_strdup(value) );
  }
  return true;
}


// cleanup function complementary to vmic_create_levels
static void vmic_destroy_levels(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; ++i) {
    struct dz_level *l = (struct dz_level*) osr->levels[i];
    if (l) {    
      _openslide_grid_destroy(l->grid);   
      g_slice_free1(sizeof(struct dz_level), l);
    }
  }
  g_free(osr->levels);
}


// generate slide level data from deep zoom parameters
static bool vmic_create_levels(openslide_t *osr, GError **err) {
  int dz_level_id;
  int os_level_id;

  struct dzinfo *dzi = (struct dzinfo *) osr->data;
  g_assert(osr->levels == NULL);
  osr->level_count = dzi->os_level_count; //capped levelcount
  osr->levels = g_new0(struct _openslide_level *, osr->level_count);
  if (!osr->levels) {
    goto FAIL;
  }
  int w = dzi->width;
  int h = dzi->height;
  int tilesize = dzi->tilesize;
  double downsample = 1;

  os_level_id = 0;
  // openslide full image is at os_level_id 0
  // deepzoom full image is dz_level_id = highest dz level
  for (dz_level_id = dzi->dz_level_count-1; 
         dz_level_id >= dzi->dz_one_tile_level; 
           dz_level_id--) {
    //g_debug("creating deepzoom level=%i, w=%i, h=%i\n", dz_level_id, (int)w, (int)h);

    // we cut off deepzoom pyramid after first largest one-tiled level

    struct dz_level *l = g_slice_new0(struct dz_level);
    if (!l) {
      goto FAIL_AND_DESTROY;
    }
    int tiles_down = (h + tilesize - 1) / tilesize;
    int tiles_across = (w + tilesize - 1) / tilesize;
    //g_debug("tiles down=%i, tiles across=%i\n", (int)tiles_down, (int)tiles_across);
    l->base.tile_h = tilesize; 
    l->base.tile_w = tilesize;
    l->base.w = w;  // total pixel size of level
    l->base.h = h;
    l->base.downsample = downsample; // contains 2^level
    //l->grid = _openslide_grid_create_tilemap(osr, tilesize, tilesize, vmic_read_tile, NULL);
    l->grid = _openslide_grid_create_simple(osr, tiles_across, tiles_down, tilesize, tilesize, vmic_read_tile);
    l->cols = tiles_across;
    l->rows = tiles_down;
    l->dz_level_id = dz_level_id;

    osr->levels[os_level_id++] = (struct _openslide_level*) l;

    w = (w+1) >> 1; // next smaller level
    h = (h+1) >> 1;
    downsample *= 2;
  } // next level

  return true;

FAIL_AND_DESTROY:
  vmic_destroy_levels(osr); // this function CAN handle incompletely allocated levels

FAIL:
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
      "vmic_create_levels: can't allocate level descriptors");
  return false;
}

static void vmic_destroy(openslide_t *osr) {
  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;

  //g_debug("call to vmic_destroy\n");
  vmic_handlecache_destroy(vmic->archive);
  vmic->archive = NULL;
  vmic_destroy_levels(osr);
  g_free(vmic);
}

struct vmic_associated_image {
  struct _openslide_associated_image base;
  struct vmicinfo *ref_vmic;
  zip_uint64_t zipindex;
};

static bool vmic_get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *const dest_buf,
                                      GError **err) {
  struct vmic_associated_image *assoc_img = (struct vmic_associated_image*)_img;

  bool success;
  gpointer cbuf;
  gsize cbufsize;
  
  struct vmic_handlecache *hc = assoc_img->ref_vmic->archive;
  struct vmic_handle *vh = vmic_handle_get(hc, err);
  if (vh==NULL) {
    return false;
  }
  //g_debug("requesting associated image: w=%i, h=%i\n", (int)assoc_img->base.w, (int)assoc_img->base.h);
  
  success = _openslide_zip_read_file_data( vh->inner, assoc_img->zipindex,
                                           &cbuf, &cbufsize, err );
  if (success) {
    int32_t w,h;
    uint32_t *img_buf;
    success = _openslide_decode_image( cbuf, cbufsize, IMAGE_FORMAT_JPG,
                                     &img_buf, &w, &h, err );
    g_slice_free1(cbufsize, cbuf);
    if (success) {
      if (assoc_img->base.w == w && assoc_img->base.h == h) {
        memcpy(dest_buf, img_buf, w * h * 4);
      }
      else {
        g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                     "get_associated_image_data: unexpected size mismatch of image");
      }
    }
    g_slice_free1((gsize)w * (gsize)h * 4, img_buf);
  }
  vmic_handle_put(hc, vh);
  return success;
}

static void vmic_destroy_associated_image(struct _openslide_associated_image *_img) {
  struct vmic_associated_image *img = (struct vmic_associated_image*)_img;
  g_slice_free1(sizeof(struct vmic_associated_image), img);
}

static const struct _openslide_associated_image_ops precipoint_associated_ops = {
  .get_argb_data = vmic_get_associated_image_data,
  .destroy = vmic_destroy_associated_image,
};

static void vmic_collect_associated_images(openslide_t *osr, zip_t *z, GError **err) {
  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;
  bool success;
  // so far (as of January 2017) we have only got a "macro"
  const char *filename = _PRECIPOINT_MACRO_IMAGE;
  const char *qualifier = "macro";

  zip_int64_t file_id = _openslide_zip_name_locate(z, filename,
                                                   ZIP_FL_ENC_RAW|ZIP_FL_NOCASE);
  if (file_id>0) {
    int32_t w,h;
    gpointer cbuf;
    gsize cbufsize;
    success = _openslide_zip_read_file_data(z, file_id, &cbuf, &cbufsize, err);
    if (success) {
      success = _openslide_jpeg_decode_buffer_dimensions(cbuf, cbufsize, &w, &h, err);
      g_slice_free1(cbufsize, cbuf);
    }
    if (success) {
      struct vmic_associated_image *img = g_slice_new0(struct vmic_associated_image);
      img->base.ops = &precipoint_associated_ops;
      img->base.w = w;
      img->base.h = h;
      img->ref_vmic = vmic;
      img->zipindex = file_id;
      //g_debug("adding assoc image: w=%i, h=%i\n", (int)img->base.w, (int)img->base.h);
      g_hash_table_insert(osr->associated_images, g_strdup(qualifier), img);
    }
  }
}

// exported function: vendor_detect
static bool precipoint_vmic_detect(const char *filename,
                              struct _openslide_tifflike *tl,
                              GError **err) {
  if (tl) { // exclude tiff files to speed up detection
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "Is a TIFF file");
    return false;
  }
  // try to open
  return vmic_try_init(filename, NULL, NULL, err);
}

static const struct _openslide_ops precipoint_ops = {
  .paint_region = vmic_paint_region,
  .destroy = vmic_destroy,
};

// exported function: vendor_open 
static bool precipoint_vmic_open( openslide_t *osr, const char *filename,
                             struct _openslide_tifflike *tl G_GNUC_UNUSED,
                             struct _openslide_hash *quickhash1,
                             GError **err) {
  bool success;
  zip_int64_t inner_index;
  uint64_t inner_size;
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  //g_debug("call to vmic_try_init\n");
  success = vmic_try_init(filename, &inner_index, &inner_size, err);
  if (success < 0) {
    return false;
  }
  struct vmicinfo *vmic = g_new0(struct vmicinfo, 1);
  if (!vmic) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "precipoint_open: cannot allocate vmicinfo\n");
    return false;
  }
  //g_debug("call to vmic_handlecache_create\n");
  vmic->archive = vmic_handlecache_create(filename, inner_index, inner_size);
  if (vmic->archive == NULL) {
    g_prefix_error( err,
                    "precipoint_open: creating handle cache failed, reason: ");
    goto FAIL2;
  }
  osr->data = vmic;

  //g_debug("call to vmic_handle_get\n");
  struct vmic_handle *vh = vmic_handle_get(vmic->archive, err);
  if (!vh) {
    g_prefix_error( err,
          "precipoint_open: fetching handle for zip archive failed, reason: ");
    goto FAIL;
  }
  zip_t *zi = vh->inner;
  g_debug("call to vmic_get_properties\n");
  success = vmic_get_properties(osr, zi, quickhash1, err);
  if (!success) goto FAIL;
  g_debug("call to vmic_create_levels\n");
  success = vmic_create_levels(osr, err);
  if (!success) goto FAIL;
  g_debug("call to vmic_collect_associated_images\n");
  vmic_collect_associated_images(osr, zi, err);
  if (*err) goto FAIL;
  gchar *hashfilename = g_strdup_printf( "%s/%i/0_0.%s",
                                         vmic->dz.folder_name,
                                         vmic->dz.dz_one_tile_level,
                                         vmic->dz.tile_imgformat_str);
  //g_debug("quickhash tile=%s\n", hashfilename);
  int64_t hash_tile_x = _openslide_zip_name_locate( zi, hashfilename,
                                                ZIP_FL_ENC_RAW|ZIP_FL_NOCASE );
  if (hash_tile_x == -1) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "precipoint_open: cannot find image for quickhash, name=%s\n",
                 hashfilename);
    g_free(hashfilename);
    goto FAIL;
  }
  gpointer hashbuf;
  gsize bufsize;
  success = _openslide_zip_read_file_data(zi, hash_tile_x,
                                          &hashbuf, &bufsize, err);
  if (!success) {
    g_prefix_error( err, 
                    "precipoint_open: can't file=%s from zip, reason: ",
                    hashfilename);    
    g_free(hashfilename);
    goto FAIL;
  }
  vmic_handle_put(vmic->archive, vh);
  g_free(hashfilename);
  _openslide_hash_data(quickhash1, hashbuf, bufsize);
  g_slice_free1(bufsize, hashbuf);
  osr->ops = &precipoint_ops;
  return true;

FAIL:
  vmic_handle_put(vmic->archive, vh);

  if (vmic->archive) {
    vmic_handlecache_destroy(vmic->archive);
    vmic->archive = NULL;
  }
FAIL2:
  g_free(vmic);
  osr->data=NULL;
  return false;
}
                      
const struct _openslide_format _openslide_format_precipoint_vmic = {
  .name = _PRECIPOINT_VMICTYPE,
  .vendor = _PRECIPOINT_VENDOR,
  .detect = precipoint_vmic_detect,
  .open = precipoint_vmic_open,
};


#ifdef SUPPORTS_GTIF


//static const char GTIF_DESCRIPTION[] = "Aperio";
static const char _PRECIPOINT_GTIFTYPE[] = "GTIFF";
#define APERIO_COMPRESSION_JP2K_YCBCR 33003
#define APERIO_COMPRESSION_JP2K_RGB   33005

struct gtif_ops_data {
  struct _openslide_tiffcache *tc;
};

struct gtif_level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
  struct gtif_level *prev;
  GHashTable *missing_tiles;
  uint16_t compression;
};

static void gtif_destroy_data(struct gtif_ops_data *data,
                         struct gtif_level **levels, int32_t level_count) {
  if (data) {
    _openslide_tiffcache_destroy(data->tc);
    g_slice_free(struct gtif_ops_data, data);
  }

  if (levels) {
    for (int32_t i = 0; i < level_count; i++) {
      if (levels[i]) {
        if (levels[i]->missing_tiles) {
          g_hash_table_destroy(levels[i]->missing_tiles);
        }
        _openslide_grid_destroy(levels[i]->grid);
        g_slice_free(struct gtif_level, levels[i]);
      }
    }
    g_free(levels);
  }
}

static void gtif_destroy(openslide_t *osr) {
  struct gtif_ops_data *data = osr->data;
  struct gtif_level **levels = (struct gtif_level **) osr->levels;
  gtif_destroy_data(data, levels, osr->level_count);
}

static bool render_missing_tile(struct gtif_level *l,
                                TIFF *tiff,
                                uint32_t *dest,
                                int64_t tile_col, int64_t tile_row,
                                GError **err) {
  bool success = true;

  int64_t tw = l->tiffl.tile_w;
  int64_t th = l->tiffl.tile_h;

  // always fill with transparent (needed for SATURATE)
  memset(dest, 0, tw * th * 4);

  if (l->prev) {
    // recurse into previous level
    double relative_ds = l->prev->base.downsample / l->base.downsample;

    cairo_surface_t *surface =
      cairo_image_surface_create_for_data((unsigned char *) dest,
                                          CAIRO_FORMAT_ARGB32,
                                          tw, th, tw * 4);
    cairo_t *cr = cairo_create(surface);
    cairo_surface_destroy(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SATURATE);
    cairo_translate(cr, -1, -1);
    cairo_scale(cr, relative_ds, relative_ds);

    // For the usual case that we are on a tile boundary in the previous
    // level, extend the region by one pixel in each direction to ensure we
    // paint the surrounding tiles.  This reduces the visible seam that
    // would otherwise occur with non-integer downsamples.
    success = _openslide_grid_paint_region(l->prev->grid, cr, tiff,
                                           (tile_col * tw - 1) / relative_ds,
                                           (tile_row * th - 1) / relative_ds,
                                           (struct _openslide_level *) l->prev,
                                           ceil((tw + 2) / relative_ds),
                                           ceil((th + 2) / relative_ds),
                                           err);
    if (success) {
      success = _openslide_check_cairo_status(cr, err);
    }
    cairo_destroy(cr);
  }

  return success;
}

static bool gtif_decode_tile(struct gtif_level *l,
                        TIFF *tiff,
                        uint32_t *dest,
                        int64_t tile_col, int64_t tile_row,
                        GError **err) {
  struct _openslide_tiff_level *tiffl = &l->tiffl;

  // check for missing tile
  int64_t tile_no = tile_row * tiffl->tiles_across + tile_col;
  if (g_hash_table_lookup_extended(l->missing_tiles, &tile_no, NULL, NULL)) {
    //g_debug("missing tile in level %p: (%"PRId64", %"PRId64")", (void *) l, tile_col, tile_row);
    return render_missing_tile(l, tiff, dest,
                               tile_col, tile_row, err);
  }

  // select color space
  enum _openslide_jp2k_colorspace space;
  switch (l->compression) {
  case APERIO_COMPRESSION_JP2K_YCBCR:
    space = OPENSLIDE_JP2K_YCBCR;
    break;
  case APERIO_COMPRESSION_JP2K_RGB:
    space = OPENSLIDE_JP2K_RGB;
    break;
  default:
    // not for us? fallback
    return _openslide_tiff_read_tile(tiffl, tiff, dest,
                                     tile_col, tile_row,
                                     err);
  }

  // read raw tile
  void *buf;
  int32_t buflen;
  if (!_openslide_tiff_read_tile_data(tiffl, tiff,
                                      &buf, &buflen,
                                      tile_col, tile_row,
                                      err)) {
    return false;  // ok, haven't allocated anything yet
  }

  // decompress
  bool success = _openslide_jp2k_decode_buffer(dest,
                                               tiffl->tile_w, tiffl->tile_h,
                                               buf, buflen,
                                               space,
                                               err);

  // clean up
  g_free(buf);

  return success;
}

static bool gtif_read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      int64_t tile_col, int64_t tile_row,
		      void *arg,
		      GError **err) {
  struct gtif_level *l = (struct gtif_level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!gtif_decode_tile(l, tiff, tiledata, tile_col, tile_row, err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
			 tiledata, tw * th * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_ARGB32,
								 tw, th,
								 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool gtif_paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h,
			 GError **err) {
  struct gtif_ops_data *data = osr->data;
  struct gtif_level *l = (struct gtif_level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff == NULL) {
    return false;
  }

  bool success = _openslide_grid_paint_region(l->grid, cr, tiff,
                                              x / l->base.downsample,
                                              y / l->base.downsample,
                                              level, w, h,
                                              err);
  _openslide_tiffcache_put(data->tc, tiff);

  return success;
}

static const struct _openslide_ops gtif_ops = {
  .paint_region = gtif_paint_region,
  .destroy = gtif_destroy,
};

static bool precipoint_gtif_detect(const char *filename G_GNUC_UNUSED,
                          struct _openslide_tifflike *tl, GError **err) {
  // ensure we have a TIFF
  ppdebug("detect gtif 1\n");

  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // ensure TIFF is tiled
  if (!_openslide_tifflike_is_tiled(tl, 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF is not tiled");
    return false;
  }

  ppdebug("detect gtif 2\n");

  // check ImageDescription
  const char *tagval = _openslide_tifflike_get_buffer(tl, 0,
                                                      TIFFTAG_IMAGEDESCRIPTION,
                                                      err);
/*  const char *tagval2 = _openslide_tifflike_get_buffer(tl, 0,
                                                      TIFFTAG_IMAGEDESCRIPTION,
                                                      err);
*/
  if (!tagval) {
    ppdebug("TIFFTAG_IMAGEDESCRIPTION not found\n");
    return false;
  }

  ppdebug("tagval=%s\r\n", tagval);

  if (!g_str_has_prefix(tagval, "{")) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a GTif slide");
    return false;
  }

  return true;
}

//#define parse_maxlen 65534

//char parse_s[parse_maxlen+1];

static char json_getch(const char **spp) {
  char c;
  while ( (c = **spp) != 0 ) {
    ppdebug("parsing char=%c\n", c);
    if (c!=' ' && c!='\t' && c!='\n' && c!='\r') {
      break;
    }
    (*spp)++;
  }
  return c;
}

#define json_getnext(spp) (*spp)++; json_getch(spp);


gchar* json_parse_primitive(const char **pjson);

static bool json_parse(const char **pjson, const gchar *prefix_key, GHashTable *properties, GError **err) {
  char c;
  int count;
  const char *p = *pjson;
  bool success = true;

  ppdebug("parsing prefix=%s at position %p\n", prefix_key, p);

  again:
  c = json_getch(&p);
  switch(c) {
    case '\0' : //ends here: do nothing
      break;

    case '\t' : case '\r' : case '\n' : case ' ' :
      ++p;
      goto again;

    case '{':
      do {
        ++p;
        gchar *key = json_parse_primitive(&p);
        if (!key) {
          break;
        }
        ppdebug("got key=%s\n", key);
        if (json_getch(&p) != ':') {
          success = false;
          break;
        }
        ++p;

        gchar *path = prefix_key ? g_strconcat(prefix_key, ".", key, NULL) : g_strdup(key);
        ppdebug("recursively parse path=%s position=%p\n", path, p);
        success = json_parse(&p, path, properties, err);
        ppdebug("recursively parse path=%s success=%d position=%p\n", path, success, p);
        g_free(path);

        if (!success) {
          break;
        }
        ppdebug("position=%p\n", p);
        c = json_getch(&p);
        ppdebug("position=%p\n", p);
      } while(c == ',');
      if (c != '}') {
          success = false;
          break;
      }
      ++p;
      break;

    case '[':
      count=0;
      do {
        ++p;
        gchar *path = g_strdup_printf("%s[%i]", prefix_key, count);
        success = json_parse(&p, path, properties, err);
        g_free(path);
        if (!success) {
          break;
        }
        count++;
        c = json_getch(&p);
      } while(c==',');
      if (c != ']') {
          success = false;
          break;
      }
      ++p;
      break;

    default:
      ppdebug("parse simple\n");
      {
        gchar *value = json_parse_primitive(&p);
        ppdebug("got simple value=%s\n", value);
        if (value) {
          bool insert_ok = g_hash_table_insert(properties, g_strdup(prefix_key), (gpointer) value);
          ppdebug("insert (%s,%s) insert_ok=%d\n", prefix_key, value, insert_ok);
        }
      }
      break;
  }

  if (success) {
    *pjson = p;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Malformed JSON object at TIFF Tag Imagedescription");
  }
  return success;
}

gchar* json_parse_primitive(const char **spp) {
  char c;
  int len=0;
  c = json_getch(spp);
  const char *gp = *spp;
  const char *bp = *spp;
  bool escaped;

  if (c=='\"') {
    ppdebug("parsing quoted primitive\n");
    gp++;
    bp++;
    escaped=false;
    while ( c = *gp, c != '\0' ) {
      if (!escaped)
      {
        if (c=='\\')
        {  escaped=true;
        }
        else if (c=='\"')
        {  gp++;
           break;
        }
      }
      else
      {
        escaped = false;
      }
      gp++;
      len++;
    }
    if (c=='\0') goto error;

  } else {
    ppdebug("parsing non-quoted primitive\n");
    while ( c = *gp, c != ' ' && c != '\r' && c != '\n' && c != '\t'
            && c != ':' && c != ',' && c != '}' && c != ']') {
      gp++;
      len++;
    }
    if (g_ascii_strncasecmp(gp, "true", len) == 0) {
      bp = "1"; len=1;
    } else if (g_ascii_strncasecmp(gp, "false", len) == 0) {
      bp = "0"; len=1;
    } else if (g_ascii_strncasecmp(gp, "null", len) == 0) {
      bp = ""; len=0;
    }
  }

  ppdebug("primitive len=%d\n", len);

  gchar *gc = g_malloc(len+2); // Escape Sequenzen können den String nur verkürzen
  gchar *pc = gc;
  for (int i=0; i<len; i++)
  {
    c = bp[i];
    if (c=='\\')
    {
      if (++i<len) {
        c = bp[i];
        switch (c)
        {
          case 't' : c = '\t'; break;
          case 'b' : c= '\b'; break;
          case 'f' : c = '\f'; break;
          case 'n': c= '\n'; break;
          case 'r' : c='\r'; break;
          case 'u' :
            if (i+4<len)
            {
              gint unum=0;
              for (int j=0; j<4; j++)
              { c = bp[++i] - '0';
                if (c >= 10) c -= ('A'-'9'+1);
                if (c > 16) c -= 0x20;
                unum *= 16;
                unum += c;
              }
              gint lu = g_unichar_to_utf8(unum, pc);
              ppdebug("unichar=%i utflen=%i\n",unum,lu);
              pc += lu;
              c=0;
            }
            break;
          default:
            // c=c;
            ;
        }
      }
    }

    if (c!=0) {
          *pc++ = c;
    }
  }

  *pc = '\0';

  //gchar *gc = g_strndup(bp, len);
  ppdebug("primitive=%s\n", gc);
  *spp = gp;
  return gc;


error:
  ppdebug("primitive failed\n");
  return NULL;
}


static void gtif_add_properties(openslide_t *osr, GHashTable *hash_props) {
  if (hash_props == NULL) {
    return;
  }

  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, hash_props);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    ppdebug("PROPERTY: key=%s value=%s ", key, value);
    if (g_str_has_prefix(key, "PreciPoint")
        || g_str_has_prefix(key, "ScanConfig")
        || g_str_has_prefix(key, "aperio")
       )
    {
      ppdebug("no prefix\n");
      g_hash_table_insert(osr->properties,
                        g_strdup(key),
                        g_strdup(value));
    }
    else
    {
      ppdebug("+gtif.prefix\n");
      g_hash_table_insert(osr->properties,
                        g_strdup_printf("gtif.%s", key),
                        g_strdup(value));
    }

  }

  if (g_hash_table_lookup(osr->properties, "gtif.objective-power")) {
    _openslide_duplicate_int_prop(osr, "gtif.objective-power", OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  } else if (g_hash_table_lookup(osr->properties, "gtif.magnification")) {
    _openslide_duplicate_int_prop(osr, "gtif.magnification", OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  }

  _openslide_duplicate_double_prop(osr, "gtif.mpp",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "gtif.mpp",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);
}


static void propagate_missing_tile(void *key, void *value G_GNUC_UNUSED,
                                   void *data) {
  const int64_t *tile_no = key;
  struct gtif_level *next_l = data;
  struct gtif_level *l = next_l->prev;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  struct _openslide_tiff_level *next_tiffl = &next_l->tiffl;

  int64_t tile_col = *tile_no % tiffl->tiles_across;
  int64_t tile_row = *tile_no / tiffl->tiles_across;

  int64_t tile_concat_x = round((double) tiffl->tiles_across /
                                next_tiffl->tiles_across);
  int64_t tile_concat_y = round((double) tiffl->tiles_down /
                                next_tiffl->tiles_down);

  int64_t next_tile_col = tile_col / tile_concat_x;
  int64_t next_tile_row = tile_row / tile_concat_y;

  //g_debug("propagating %p (%"PRId64", %"PRId64") to %p (%"PRId64", %"PRId64")", (void *) l, tile_col, tile_row, (void *) next_l, next_tile_col, next_tile_row);

  int64_t *next_tile_no = g_new(int64_t, 1);
  *next_tile_no = next_tile_row * next_tiffl->tiles_across + next_tile_col;
  g_hash_table_insert(next_l->missing_tiles, next_tile_no, NULL);
}

// check for OpenJPEG CVE-2013-6045 breakage
// (see openslide-decode-jp2k.c)
static bool test_tile_decoding(struct gtif_level *l,
                               TIFF *tiff,
                               GError **err) {
  // only for JP2K slides.
  // shouldn't affect RGB, but check anyway out of caution
  if (l->compression != APERIO_COMPRESSION_JP2K_YCBCR &&
      l->compression != APERIO_COMPRESSION_JP2K_RGB) {
    return true;
  }

  int64_t tw = l->tiffl.tile_w;
  int64_t th = l->tiffl.tile_h;

  uint32_t *dest = g_slice_alloc(tw * th * 4);
  bool ok = gtif_decode_tile(l, tiff, dest, 0, 0, err);
  g_slice_free1(tw * th * 4, dest);
  return ok;
}

static bool precipoint_gtif_open(openslide_t *osr,
                        const char *filename,
                        struct _openslide_tifflike *tl,
                        struct _openslide_hash *quickhash1, GError **err) {
  bool success;
  struct gtif_ops_data *data = NULL;
  struct gtif_level **levels = NULL;
  int32_t level_count = 0;

  // open TIFF
  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  if (!tiff) {
    goto FAIL;
  }

  /*
   * http://www.aperio.com/documents/api/Aperio_Digital_Slides_and_Third-party_data_interchange.pdf
   * page 14:
   *
   * The first image in an SVS file is always the baseline image (full
   * resolution). This image is always tiled, usually with a tile size
   * of 240 x 240 pixels. The second image is always a thumbnail,
   * typically with dimensions of about 1024 x 768 pixels. Unlike the
   * other slide images, the thumbnail image is always
   * stripped. Following the thumbnail there may be one or more
   * intermediate "pyramid" images. These are always compressed with
   * the same type of compression as the baseline image, and have a
   * tiled organization with the same tile size.
   *
   * Optionally at the end of an SVS file there may be a slide label
   * image, which is a low resolution picture taken of the slide’s
   * label, and/or a macro camera image, which is a low resolution
   * picture taken of the entire slide. The label and macro images are
   * always stripped.
   */

  do {
    // the tiled directories are the ones we want
    if (TIFFIsTiled(tiff)) {
      level_count++;
    }

    // check depth
    uint32_t depth;
    if (TIFFGetField(tiff, TIFFTAG_IMAGEDEPTH, &depth) &&
        depth != 1) {
      // we can't handle depth != 1
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot handle ImageDepth=%d", depth);
      goto FAIL;
    }

    // check compression
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read compression scheme");
      goto FAIL;
    }
    if ((compression != APERIO_COMPRESSION_JP2K_YCBCR) &&
        (compression != APERIO_COMPRESSION_JP2K_RGB) &&
        !TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }
  } while (TIFFReadDirectory(tiff));

  // allocate private data
  data = g_slice_new0(struct gtif_ops_data);

  levels = g_new0(struct gtif_level *, level_count);
  int32_t i = 0;
  if (!_openslide_tiff_set_dir(tiff, 0, err)) {
    goto FAIL;
  }
#define NZ(x) ((x) ? (x) : "nul")
  do {
    tdir_t dir = TIFFCurrentDirectory(tiff);
    char *image_desc;
    bool has_desc = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc);
    if (TIFFIsTiled(tiff)) {
      //g_debug("tiled directory: %d", dir);
      ppdebug("tiled directory: %d\nimage-desc=%s\n", dir, has_desc ? image_desc : "none");
      struct gtif_level *l = g_slice_new0(struct gtif_level);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      if (i) {
        l->prev = levels[i - 1];
      }
      levels[i++] = l;

      if (!_openslide_tiff_level_init(tiff,
                                      dir,
                                      (struct _openslide_level *) l,
                                      tiffl,
                                      err)) {
        goto FAIL;
      }

      l->grid = _openslide_grid_create_simple(osr,
                                              tiffl->tiles_across,
                                              tiffl->tiles_down,
                                              tiffl->tile_w,
                                              tiffl->tile_h,
                                              gtif_read_tile);

      // get compression
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &l->compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read compression scheme");
        goto FAIL;
      }

      // some Aperio slides have some zero-length tiles, apparently due to
      // an encoder bug
      toff_t *tile_sizes;
      if (!TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &tile_sizes)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Cannot get tile sizes");
        goto FAIL;
      }
      l->missing_tiles = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                               g_free, NULL);
      for (ttile_t tile_no = 0;
           tile_no < tiffl->tiles_across * tiffl->tiles_down; tile_no++) {
        if (tile_sizes[tile_no] == 0) {
          int64_t *p_tile_no = g_new(int64_t, 1);
          *p_tile_no = tile_no;
          g_hash_table_insert(l->missing_tiles, p_tile_no, NULL);
        }
      }
    } else {
      ppdebug("non-tiled directory: %d\nimage_desc=%s\n", dir, has_desc ? image_desc : "none");
      // associated image
      gchar *assoc_name = has_desc ? g_strdup(image_desc) : g_strdup_printf("associated_image_%i", dir);
      success = _openslide_tiff_add_associated_image(osr, assoc_name, tc,
                                                         TIFFCurrentDirectory(tiff),
                                                         err);
      //success = add_associated_image(osr, assoc_name, tc, tiff, err);
      g_free(assoc_name);
      if (!success) {
        goto FAIL;
      }
      ppdebug("associated image added: %d", dir);
      //g_debug("associated image: %d", dir);
    }
  } while (TIFFReadDirectory(tiff));

  // tiles concatenating a missing tile are sometimes corrupt, so we mark
  // them missing too
  for (i = 0; i < level_count - 1; i++) {
    g_hash_table_foreach(levels[i]->missing_tiles, propagate_missing_tile,
                         levels[i + 1]);
  }

  // check for OpenJPEG CVE-2013-6045 breakage
  if (!test_tile_decoding(levels[0], tiff, err)) {
    goto FAIL;
  }

  // read properties
  if (!_openslide_tiff_set_dir(tiff, 0, err)) {
    goto FAIL;
  }
  char *image_desc;
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ImageDescription field");
    goto FAIL;
  }


  // set hash and properties
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    levels[level_count - 1]->tiffl.dir,
                                                    0,
                                                    err)) {
    goto FAIL;
  }

  // parse JSON blob from ImageDescription tiff tag
  GHashTable *j_props = g_hash_table_new_full(NULL, g_str_equal, g_free, g_free);
  json_parse((const char**)&image_desc, NULL, j_props, err);
  gtif_add_properties(osr, j_props);
  g_hash_table_destroy(j_props);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &gtif_ops;

  // put TIFF handle and store tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

FAIL:
  gtif_destroy_data(data, levels, level_count);
  _openslide_tiffcache_put(tc, tiff);
  _openslide_tiffcache_destroy(tc);
  return false;
}

const struct _openslide_format _openslide_format_precipoint_gtif = {
  .name = _PRECIPOINT_GTIFTYPE,
  .vendor = _PRECIPOINT_VENDOR,
  .detect = precipoint_gtif_detect,
  .open = precipoint_gtif_open,
};

#endif
