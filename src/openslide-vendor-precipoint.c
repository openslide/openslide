/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2016 Markus Pöpping
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

/* VMIC constants */
const char _PRECIPOINT_VENDOR[] = "PreciPoint";
const char _PRECIPOINT_VMICTYPE[] = "M8-VMIC";

const char _PRECIPOINT_INNER_CONTAINER_SUFFIX[] = ".vmici"; // either one of these is the name of the inner archive
const char _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME[] = "Image";

const char _PRECIPOINT_MACRO_IMAGE[] = "VMCF/overview.jpg";
const char _PRECIPOINT_PROPS_FILENAME[] = "VMCF/config.osc";
const char _PRECIPOINT_PROPS_OSC_NODE[] = "ObjectScanConfig";
const char _PRECIPOINT_PROPS_PREFIX[] = "PreciPoint";
const char _PRECIPOINT_PROPPATH_MAGNIFICATION[] = "PreciPoint.Objective.Magnification";
const char _PRECIPOINT_PROPPATH_NAME[] = "PreciPoint.ScanData.Name";

#define VMIC_HANDLE_CACHE_MAX 64

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
  int outstanding;
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

static struct vmic_handle* vmic_handle_new(struct vmic_handlecache *vc, GError **err) {
  
  struct vmic_handle* vh = g_slice_new0(struct vmic_handle);
  if (vh) {
    vh->ref_vhc = vc;

    zip_t *zo = _openslide_zip_open_archive(vc->filename, err);
    if (!zo) {
      goto FAIL;
    }
    zip_source_t *zs = zip_source_zip(zo, zo, vc->inner_index, 0, 0, 0);
    g_debug("vmic_handle_new: zip_source_zip returns %p", (void*)zs);

    if (!zs) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "vmic_handle_new: cannot create zip source, filename=%s, index=%i",
                  vc->filename, (int)vc->inner_index);
      zip_close(zo);
      goto FAIL;
    }

    vh->inner = _openslide_zip_open_archive_from_source(zs, err);
    g_debug("vmic_handle_new: _openslide_zip_open_archive_from_source returns %p",
            (void*)vh->inner);
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

static struct vmic_handlecache* vmic_handlecache_new(const char *filename, zip_int64_t inner_index) {
  struct vmic_handlecache *hc = g_slice_new0(struct vmic_handlecache);
  hc->filename = g_strdup(filename);
  hc->inner_index = inner_index;
  hc->cache = g_queue_new();
  hc->lock = g_mutex_new();
  return hc;
}

static struct vmic_handle* vmic_handlecache_get(struct vmic_handlecache *vc, GError **err) {
  //g_debug("get vmic zip");
  g_mutex_lock(vc->lock);
  vc->outstanding++;
  struct vmic_handle *vh = g_queue_pop_head(vc->cache);
  g_mutex_unlock(vc->lock);

  if (vh == NULL) {
    g_debug("create vmic zip\n");
    vh = vmic_handle_new(vc, err);
  }
  if (vh == NULL) {
    g_mutex_lock(vc->lock);
    vc->outstanding--; //not guaranteed to be atomic!
    g_mutex_unlock(vc->lock);
  }
  
  return vh;
}

static void vmic_handlecache_put(struct vmic_handlecache *vc, struct vmic_handle *vh) {
  if (vh == NULL || vc == NULL) {
    return;
  }

  //g_debug("put vmic");
  g_mutex_lock(vc->lock);
  g_assert(vc->outstanding);
  vc->outstanding--;
  if (g_queue_get_length(vc->cache) < VMIC_HANDLE_CACHE_MAX) {
    g_queue_push_head(vc->cache, vh);
    vh = NULL;
  }
  g_mutex_unlock(vc->lock);

  if (vh) {
    g_debug("too many vmic handles in queue\n");
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
    g_debug("delete vmic handle\n");
    vmic_handle_delete(vh);
  }
  g_mutex_unlock(vc->lock);
  g_queue_free(vc->cache);
  g_mutex_free(vc->lock);
  g_free(vc->filename);
  g_slice_free(struct vmic_handlecache, vc);
}

/* Extra data for tile inside of grid */
struct dz_tileinfo {
  int32_t w, h;
  zip_int64_t zipindex;
};

/* Params of each OS level */
struct dz_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
  int dz_level_id;
  int cols;
  int rows;
  struct dz_tileinfo *matrix; // matrix with additional info for tiles
};


/* read deepzoom tile and paint it to cairo context */
static bool vmic_read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *extradata G_GNUC_UNUSED,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {

  struct dz_level *lev = (struct dz_level *) level;
  struct vmicinfo *vmic = (struct vmicinfo*)osr->data;

  struct dz_tileinfo *tileinfo = & lev->matrix[tile_row * lev->cols + tile_col];

  // tile size
  int32_t tw = tileinfo->w;
  int32_t th = tileinfo->h;
  zip_int64_t zipindex = tileinfo->zipindex;

  //  g_debug("call to vmic_read_tile, level params:l#=%i cols=%i rows=%i matrix=%p\n", l->dz_level_id, l->cols, l->rows, (void*)l->matrix);
  //  g_debug("requested tile: levelds=%lf, col=%i, row=%i", (double)l->base.downsample, (int)tile_col, (int)tile_row);
  //  g_debug(", w=%i, h=%i, zip=%i, w2=%i, h2=%i, z2=%i\n", (int)tw, (int)th, (int)zipindex, (int)tw2, (int)th2, (int)zipindex2);

  /* try to fetch from cache */
  struct _openslide_cache_entry *cache_entry;

  uint32_t *tiledata = _openslide_cache_get( osr->cache,
                                             level, 
                                             tile_col, tile_row,
                                             &cache_entry);
  
  if (!tiledata) { // in case of cache miss, retrieve tile from ZIP archive

    int32_t w,h;
    bool success;
    /*
    possible alternative file location without preindexing
    gchar *tilefilename = g_strdup_printf("%s/%i/%i_%i.jpg",_DEEPZOOM_FOLDER,dz_level_id,(int)x,(int)y);
    zip_int64_t zpi = _openslide_zip_name_locate(vmii->zi, tilefilename, ZIP_FL_ENC_RAW); // caution: using ZIP_FL_NOCASE in flags is EXTREMELY slow.

    g_debug("- adding tile dzlevel=%i, x=%i, y=%i, w=%i, h=%i, name=%s, zipindex=%i, remains_x=%i, remains_y=%i\n", (int)dz_level_id, (int)x, (int)y, (int)w, (int)h, tilefilename, (int)zpi, (int)remains_x, (int)remains_y);
    g_free(tilefilename);
    */

    if (zipindex != -1) { // note: getting an index "-1" means the tile doesn't exist. We used this as empty marker
    
      struct vmic_handle *vh = vmic_handlecache_get(vmic->archive, err);
      
      success = (vh!=NULL);
      
      // the returned buffer @tiledata will be allocated with g_slice_alloc

      if (success ) {
        if (!_openslide_zip_read_image( vh->inner, zipindex,
                                        vmic->dz.tile_format_id, &tiledata, 
                                        &w, &h, err) ) {
          g_prefix_error(err, "Error in function vmic_read_tile: ");
          success = false;
        } 
      }
      
      // check if size of tile is as expected
      if (success && (w != tw || h != th) ) {      

          g_slice_free1((gsize) w * h * 4, tiledata);

          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "vmic_read_tile: size mismatch at tile level=%i, row=%i, col=%i. expected size=(%i,%i), stored tile=(%i,%i).",
                  (int)lev->dz_level_id, (int)tile_row, (int)tile_col, 
                  (int)tw, (int)th, (int)w, (int)h);

          success = false;
      }
      
      vmic_handlecache_put(vmic->archive, vh);
      
      if (!success) {
        // something had failed during above procedure
        return false;
      }

      // reading tile was successful - put it in the cache
      // the tile needs to be unref'd after put
      _openslide_cache_put( osr->cache, level, 
                            tile_col, tile_row,
                            tiledata, (int)w * h * 4,
                            &cache_entry);
    }
    
  }

  // draw it

  if (tiledata)  {
    // use pixel data as image source
    cairo_surface_t *surface =
            cairo_image_surface_create_for_data( (unsigned char *) tiledata,
                                                 CAIRO_FORMAT_ARGB32,
                                                 tw, th,
                                                 tw * 4);

    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_surface_destroy(surface);
    cairo_paint(cr);
  }
  else {
    // no tile at this position => don't need to do anything
    // because cairo surface is pre-filled with transparent pixels
  }

  if (tiledata)  {
    // normal tile is always cached --
    // done with the cache entry, release it
    _openslide_cache_entry_unref(cache_entry);
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

  return _openslide_grid_paint_region(lev->grid, cr, NULL,
                                      x / lev->base.downsample,
                                      y / lev->base.downsample,
                                      level, w, h,
                                      err);
}

/* Helper function: find node by name in a node chain, returns NULL if not found. does not recurse branches */
static xmlNode* _openslide_xml_find_node(xmlNode *node, const xmlChar* name)  {
    
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
static bool dzz_get_deepzoom_properties(xmlDoc *xmldoc, struct dzinfo *dzi, GHashTable *properties, GError **err)
{    
  bool success = false;

  xmlNode *xmlnodeImage = _openslide_xml_find_node(xmldoc->children, (xmlChar*) _DEEPZOOM_PROP_IMAGE_NODE);
  if (xmlnodeImage == NULL) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "DZC/XML: cannot find XML %s Element",_DEEPZOOM_PROP_IMAGE_NODE);
      goto FAIL;
  }

  double ppm = _openslide_xml_parse_double_attr(xmlnodeImage,_DEEPZOOM_PROP_PPM, err);
  if (*err) goto FAIL;

  if (ppm == 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "DZC/XML: Cannot retrieve MPP property");
      goto FAIL;
  }

  //g_debug("ppm=%lf\n", ppm);

  success = g_hash_table_insert(  properties,
                                  g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                                  _openslide_format_double(1.0 / ppm) );
  if (!success) goto FAIL;

  success = g_hash_table_insert(  properties,
                                  g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                                  _openslide_format_double(1.0 / ppm) );
  if (!success) goto FAIL;

  dzi->tilesize = _openslide_xml_parse_int_attr( xmlnodeImage,
                                                 g_strdup(_DEEPZOOM_PROP_TILESIZE),
                                                 err);
  dzi->overlap = _openslide_xml_parse_int_attr( xmlnodeImage,
                                                g_strdup(_DEEPZOOM_PROP_OVERLAP),
                                                err);
  //g_debug("tilesize=%i\n", dzi->tilesize);
  //g_debug("overlap=%i\n", dzi->overlap);

  if (dzi->tilesize <= 0 || dzi->overlap < 0 || dzi->overlap >= dzi->tilesize/2)
  {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
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

  //g_debug("overlap=%i\n", dzi->overlap);
  //g_debug("w=%i, h=%i, tsize=%i, ovl=%i\n", (int)dzi->width, (int)dzi->height, (int)dzi->tilesize, (int)dzi->overlap);

  if (*err) goto FAIL;

  xmlChar *str_pictype = xmlGetProp(xmlnodeImage, (const xmlChar*) _DEEPZOOM_PROP_IMAGE_FORMAT);
  if (!str_pictype) goto FAIL;
  //g_debug("format=%s\n", str_pictype);
  
  g_strlcpy(dzi->tile_imgformat_str, (const gchar*) str_pictype, MAX_IFSTR);
  
  dzi->tile_format_id = IMAGE_FORMAT_UNKNOWN;
  if (str_pictype != NULL) {
    if ( g_ascii_strcasecmp((const gchar*)str_pictype,"jpg") == 0 )
    { dzi->tile_format_id = IMAGE_FORMAT_JPG; }
    else if (g_ascii_strcasecmp((const gchar*)str_pictype,"png") == 0)
    { dzi->tile_format_id = IMAGE_FORMAT_PNG; }
    else if (g_ascii_strcasecmp((const gchar*)str_pictype,"bmp") == 0)
    { dzi->tile_format_id = IMAGE_FORMAT_BMP; }
  }

  if (dzi->tile_format_id == IMAGE_FORMAT_UNKNOWN) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
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

  /* calc by bit shifting (avoiding formula "ceil(log(longside)/log(2))") */
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


// This function searches the .xml or .dzi file that contains
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

    // search for a file with xml or dzi suffix which is not inside a folder
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
    // found!
    g_strlcpy(dzi->key_filename, name, MAX_DZFILENAME-2);
    // compose corresponding folder name by replacing the ".xml" suffix with "_files"
    g_strlcpy(dzi->folder_name, name, MAX_DZFILENAME-2);
    strcpy(dzi->folder_name + strlen(dzi->folder_name)-4, "_files");
    return true;
  }
  else {
    return false;
  }
}


/* check if vmic container */
static struct vmicinfo* vmic_try_init(const char *vmic_filename, GError **err) {

  zip_error_t ze;
  struct vmicinfo *vmic = g_new0(struct vmicinfo, 1);

  // open outer archive
  zip_error_init(&ze);

  zip_t *zo = _openslide_zip_open_archive(vmic_filename, err);
  if (!zo) goto FAIL;

  zip_int64_t entries_outer = zip_get_num_entries(zo, 0);
  int vmici_index = -1;

  // precipoint file has a deepzoom container with a name ending on ".vmici" or the name "Image"
  // check only first 20 files, to prevent slowdown in case the archive is huge
  for (int i=0; i < MIN(entries_outer, 20); i++) {
      
    const char *inner_name = zip_get_name(zo, i, ZIP_FL_ENC_RAW);

      // The name is either exactly 'Image' or ends with '*.vmici'
    if ( g_str_has_suffix(inner_name,_PRECIPOINT_INNER_CONTAINER_SUFFIX)
         || strcmp(inner_name, _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME) == 0 ) {
      //g_debug("checking name=%s ", name);
      
      //check if it's a zip archive by "magic number"
      zip_file_t *file = zip_fopen_index(zo, i, 0); // this is ok because we don't need to be thread-safe here.
      if (file) {
        uint32_t file_magic=0;
        zip_fread(file, &file_magic, 4);
        zip_fclose(file);

        //g_debug("checking magic=%x\n", (int)file_magic);

        if (file_magic == 0x04034B50) // "PK34"
        {   // this should be the inner archive. No further checks done.
            vmici_index = i;
            break;
        }
      }
    }
  }

  //g_debug ("found inner zip container at index %i in zip %p\n",(int) vmici_index, (void*)zo);
  if (vmici_index < 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
            "VMICI image container not found");
  }

  // close it, we have enough information now
  // it will be reopened by vmic_handlecache_get
  if (zo) {
    _openslide_zip_close_archive(zo);
  }

  //
  if ( (vmic->archive = vmic_handlecache_new(vmic_filename, vmici_index))==NULL ) {
    goto FAIL;
  }
  return vmic;

FAIL:
  g_free(vmic);
  return NULL;
}

/* close zip containers */
static void vmic_close_container(struct vmicinfo *vmic)
{
  if (!vmic) {
    return;
  }
  vmic_handlecache_destroy(vmic->archive);
  vmic->archive = NULL;

  //  g_debug("Closing vmic container, zi=%p, za=%p\n", (void*)vmic->zi.handle, (void*)vmic->zip_outer.handle);
  //if (vmic->zi) { _openslide_zip_close_archive(&vmic->zi); } // inner
  //if (vmic->zip_outer) { _openslide_zip_close_archive(&vmic->zip_outer); } // outer
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

  success = _openslide_zip_read_file_data(z, xml_file_id, &xmlbuf, &xmlsize, err);

  //g_debug("ready to parse xml_file_id=%i, xmlbuf=%p, xmlsize=%i\n", (int)xml_file_id, (void*)xmlbuf, (int)xmlsize);

  if (!success) {
    g_prefix_error( err, 
                    "Cannot access VMIC XML description file at zip index %li - reason:", 
                    (long int)xml_file_id);
    goto FINISH;
  }

  if (hash) {
    _openslide_hash_data(hash, xmlbuf, xmlsize);
  }

  xmldoc = xmlReadMemory( xmlbuf, xmlsize, NULL, NULL,
                          XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET);
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
static void vmic_convert_xml_tree_to_properties(xmlNode *node, GHashTable *os_properties, const char *propname_prefix)
{
  while (node != NULL)
  {
    if (node->type == XML_ELEMENT_NODE) {
      gchar *elementname = g_strconcat(propname_prefix, ".", node->name, NULL);
      for (xmlAttr* attribute = node->properties;
              attribute != NULL;
                  attribute = attribute->next)
      {
        xmlChar* value = xmlGetProp(node, attribute->name);
        gchar *propname = g_strconcat(elementname, ".", attribute->name, NULL);
        //g_debug("adding property=%s value=%s\n", propname, value);
        g_hash_table_insert(os_properties, propname, g_strdup((gchar*) value));
        xmlFree(value);
      }
      if (node->children) {
          vmic_convert_xml_tree_to_properties(node->children, os_properties, elementname);
      }
      g_free (elementname);
    }
    else if (node->type == XML_TEXT_NODE) {
      xmlChar *content = xmlNodeGetContent(node);
      //g_debug("adding property=%s value=%s\n", propname_prefix, content);
      g_hash_table_insert(os_properties, g_strdup((gchar*)propname_prefix), g_strdup((gchar*)content));
      xmlFree(content);
    }
    node = node->next;
  }
}


/* parses all properties specific to VMIC file */
static bool vmic_get_properties(openslide_t *osr, zip_t *z, struct _openslide_hash *quickhash, GError **err) {
  bool success;
  xmlDoc *xmldoc;

//  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;
  struct dzinfo *dzz = (struct dzinfo *)osr->data;

  dzz_find_key_file(z, dzz );
  //g_debug("deepzoom key file=%s, folder=%s\n", dzz->key_filename, dzz->folder_name);
  
  // Parsing DEEPZOOM properties
  xmldoc = _openslide_zip_parse_xml_file(z, dzz->key_filename, ZIP_FL_ENC_RAW|ZIP_FL_NOCASE, err, quickhash);
  if (!xmldoc) goto FAIL;
  
  success = dzz_get_deepzoom_properties(xmldoc, dzz, osr->properties, err);
  //g_debug("deepzoom levels=%i, one_tile_level=%i\n", dzz->dz_level_count, dzz->dz_one_tile_level);
  xmlFreeDoc(xmldoc);
  if (!success) goto FAIL;

  if (dzz->overlap != 0) { // this check can be removed as soon as overlapping slides show up
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "DZC/XML: DZ Overlap parameter is %i, but as of now (Sept 2016) VMIC tiles are not expected to overlap !",
                 dzz->overlap);
    goto FAIL;
  }

  // Parsing VMIC properties from VMCF/config.osc
  xmldoc = _openslide_zip_parse_xml_file( z, _PRECIPOINT_PROPS_FILENAME, 
                                          ZIP_FL_ENC_RAW|ZIP_FL_NOCASE, err, 
                                          quickhash );
  if (!xmldoc) goto FAIL;

  xmlNode *oscconfig = _openslide_xml_find_node( xmldoc->children, 
                                                 (xmlChar*) _PRECIPOINT_PROPS_OSC_NODE);
  if (!oscconfig) {
    g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "OSC/XML: Can't find osc node (%s) for VMIC parameters in config file \"%s\".",
                 _PRECIPOINT_PROPS_OSC_NODE, _PRECIPOINT_PROPS_FILENAME);
    xmlFreeDoc(xmldoc);
    goto FAIL;
  }

  vmic_convert_xml_tree_to_properties( oscconfig->children,
                                       osr->properties,
                                       _PRECIPOINT_PROPS_PREFIX);

  xmlFreeDoc(xmldoc);

  // gather and expose important props from config.osc
  _openslide_duplicate_double_prop( osr, _PRECIPOINT_PROPPATH_MAGNIFICATION, 
                                    OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER );
  
  // copy title/name to "COMMENT" tag
  const char *value = g_hash_table_lookup(osr->properties, _PRECIPOINT_PROPPATH_NAME);
  if (value) {
    g_hash_table_insert( osr->properties,
                         g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
                         g_strdup(value) );
  }

  return true;

FAIL:
  return false;
}


// This function parses the deepzoom archive for tiles which name matches 
// the appointed level and builds a matrix of file indices and image dimensions
// this is to speed up initial open, since file names are not sorted.
static void dzz_find_indices( struct dzinfo *dzi, zip_t *z, 
                              const char *folder, int level, 
                              int rows, int cols, 
                              struct dz_tileinfo * matrix) {

  zip_int64_t i;

  // struct timespec tm1, tm2;
  // sscanf pattern - %c is to make sure to reach end of name
  // gchar *pattern = g_strdup_printf("%s%%1[\\/]%i%%1[\\/]%%u_%%u.%s%%c",folder,level,vmi->base.image_format_string);

  // own pattern matching to avoid sscanf
  gchar *prefix1 = g_strdup_printf("%s\\%i\\", folder, level);
  gchar *prefix2 = g_strdup_printf("%s/%i/", folder, level);

  unsigned int prefix_size = strlen(prefix1);
  g_assert(prefix_size == strlen(prefix2));

  //g_debug("scanning catalog %p for prefix1=%s, prefix2=%s, rows=%i, cols=%i\n", (void*)z, prefix1, prefix2, rows, cols);

  // preoccupy matrix with "not found marker"
  for (i=0; i<(zip_int64_t)rows*(zip_int64_t)cols; i++) {
      matrix[i].zipindex = -1;
  }

  zip_int64_t zip_num_entries = zip_get_num_entries(z,0);

  for (i=0; i<zip_num_entries; i++) {
    int x=-1, y=-1;

    bool match = false;
    char *fname = (char*) zip_get_name(z, i, ZIP_FL_ENC_RAW);
    
    if (memcmp(fname, prefix1, prefix_size) == 0 || memcmp(fname, prefix2, prefix_size) == 0)
    { // retrieve indices of tile from file name and store tile index in matrix
      char *s = fname + prefix_size;
      x = strtol(s, &s, 10);
      if (*s) if (*s++ == '_') {
              
        y = strtol(s, &s, 10);
              
        if (*s) if (*s++ == '.') {
          if (strcmp(s, dzi->tile_imgformat_str) == 0) { match = true; }
        }
      }
    }

    if (match) {
      
      if (x < 0 || y < 0 || x >= cols || y >= rows)
      {
        // this shouldn't happen in a well-formed DZ file
        //g_debug("Coords of tile out of bounds => Ignored!\n");
      }
      else
      {
        matrix[y*cols + x].zipindex = i;
        //g_debug("Index for tile (x=%i, y=%i) is %i - OK!\n", (int)x, (int)y, (int)i);
      }
    }
  }
  g_free(prefix2);
  g_free(prefix1);
}


// cleanup function to vmic_create_levels

static void vmic_destroy_levels(openslide_t *osr) {

  for (int32_t i = 0; i < osr->level_count; i++) {
    struct dz_level *l = (struct dz_level*) osr->levels[i];
    if (l) {    
      g_slice_free1(l->rows * l->cols * sizeof(struct dz_tileinfo), l->matrix);
      _openslide_grid_destroy(l->grid);   
      g_slice_free1(sizeof(struct dz_level), l);
    }
  }
  g_free(osr->levels);
}


// generate slide level data from deep zoom parameters

static bool vmic_create_levels(openslide_t *osr, zip_t *z, GError **err) {

  int dz_level_id;
  int os_level_id;

  //struct vmicinfo *vmii = (struct vmicinfo *)osr->data;
  struct dzinfo *dzi = (struct dzinfo *)osr->data;

  g_assert(osr->levels == NULL);

  osr->level_count = dzi->os_level_count; //capped levelcount

  //g_debug("allocating array of %i pointers\n",osr->level_count);
  osr->levels = g_new0(struct _openslide_level *, osr->level_count);
  if (!osr->levels) goto FAIL;

  int w = dzi->width;
  int h = dzi->height;
  int tilesize = dzi->tilesize;
  int overlap = dzi->overlap;
  double downsample = 1;

  os_level_id = 0;

  // usage of indices: openslide full image is os_level_id=0, deepzoom full image is dz_level_id = highest dz level 
  for (dz_level_id = dzi->dz_level_count-1; 
         dz_level_id >= dzi->dz_one_tile_level; 
           dz_level_id--) {
    //g_debug("deepzoom level=%i, w=%i, h=%i\n", dz_level_id, (int)w, (int)h);

    // we cut off deepzoom pyramid after largest one-tiled level, because
    // - they don't clutter up the properties
    // - some older tiny VMIC tiles have black borders
    // - some openslide clients might have trouble with unexpectedly small levels
    // - we don't really need tiny levels

    struct dz_level *l = g_slice_new0(struct dz_level);
    if (!l) goto FAIL_AND_DESTROY;

    int tiles_down = (h + tilesize - 1) / tilesize;
    int tiles_across = (w + tilesize - 1) / tilesize;
    //g_debug("tiles down=%i, tiles across=%i\n", (int)tiles_down, (int)tiles_across);

    l->base.tile_h = tilesize; 
    l->base.tile_w = tilesize;
    l->base.w = w;  // total pixel size of level
    l->base.h = h;
    l->base.downsample = downsample; // contains 2^level
    l->grid = _openslide_grid_create_tilemap(osr, tilesize, tilesize, vmic_read_tile, NULL);
    l->cols = tiles_across;
    l->rows = tiles_down;
    l->dz_level_id = dz_level_id;

    l->matrix = (struct dz_tileinfo*) g_slice_alloc(sizeof(struct dz_tileinfo) * tiles_across * tiles_down);
    if (!l->matrix) goto FAIL_AND_DESTROY;

    // build a matrix of file indices for all tiles of this level
    dzz_find_indices(dzi, z, dzi->folder_name, dz_level_id, tiles_down, tiles_across, l->matrix);

    int remains_y = h;
    int tile_w, tile_h;
    double offset_x, offset_y;

    for (int y = 0; y < tiles_down; y++) {

      // calculate expected height of individual tile
      offset_y = 0;
      tile_h = MIN(remains_y, tilesize);
      if (y>0) { tile_h += overlap; offset_y = -overlap; }
      if (y<tiles_down-1) tile_h += overlap;

      int remains_x = w;

      for (int x = 0; x < tiles_across; x++) {

        // expected width of individual tile
        offset_x = 0;
        tile_w = MIN(remains_x, tilesize);
        if (x>0) { tile_w += overlap; offset_x = -overlap; }
        if (x<tiles_across-1) tile_w += overlap;

        // --- alternative approach : Fetch file on demand by name --- 
        // gchar *tilefilename = g_strdup_printf("%s/%i/%i_%i.jpg",_DEEPZOOM_FOLDER,dz_level_id,(int)x,(int)y);
        // zip_int64_t zpi = _openslide_zip_name_locate(vmii->zi, tilefilename, ZIP_FL_ENC_RAW); // caution: ZIP_FL_NOCASE=slow search!

        struct dz_tileinfo *tileinfo = &l->matrix[y*tiles_across+x];
        tileinfo->w = tile_w; // expected width of tile
        tileinfo->h = tile_h; // expected height of tile

        _openslide_grid_tilemap_add_tile( l->grid,
                                          x, y,
                                          offset_x, offset_y,
                                          tile_w, tile_h,
                                          NULL);

        remains_x -= tilesize;
      }
      
      if (remains_x == 0) {
        g_assert(w % tilesize == 0);
      }
      else {
        g_assert(remains_x + tilesize == w % tilesize);
      }

      remains_y -= tilesize;
    }
    if (remains_y == 0) {
      g_assert(h % tilesize == 0);
    }
    else {
      g_assert(remains_y + tilesize == h % tilesize);
    }

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
  vmic_destroy_levels(osr);
  vmic_close_container(vmic);
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

  uint32_t *img_buf;
  int32_t w,h;
  bool success;

  //g_debug("requested assoc image: w=%i, h=%i, destbuf=%p\n", (int)w, (int)h, (void*)dest_buf);
  
  struct vmic_handlecache *hc = assoc_img->ref_vmic->archive;
  struct vmic_handle *vh = vmic_handlecache_get(hc, err);      
  if (vh==NULL) {
    return false;
  }
  
  success = _openslide_zip_read_image( vh->inner, assoc_img->zipindex, 
                                       IMAGE_FORMAT_JPG, &img_buf,
                                       &w, &h, err);
  
  if (success) {
    if (assoc_img->base.w == w && assoc_img->base.h == h) {
      memcpy(dest_buf, img_buf, w * h * 4);
    }
    else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
          "get_associated_image_data: unexpected size mismatch of image");
    }
  }
  
  g_slice_free1((gsize)w * (gsize)h * 4, img_buf);
  
  vmic_handlecache_put(hc, vh);

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

  bool success;

  // so far (as of January 2017) we have only a "macro"

  const char *filename = _PRECIPOINT_MACRO_IMAGE;
  const char *qualifier = "macro";

  struct vmicinfo *vmic = (struct vmicinfo *)osr->data;

  zip_int64_t file_id = _openslide_zip_name_locate(z, filename, ZIP_FL_ENC_RAW|ZIP_FL_NOCASE);

  if (file_id>0) {
    
    uint32_t *buf;
    int32_t w,h;

    success = _openslide_zip_read_image(z, file_id, IMAGE_FORMAT_JPG, &buf, &w, &h, err);
    if (success) {
      struct vmic_associated_image *img = g_slice_new0(struct vmic_associated_image);
      img->base.ops = &precipoint_associated_ops;
      img->base.w = w;
      img->base.h = h;
      img->ref_vmic = vmic;
      img->zipindex = file_id;
      //g_debug("adding assoc image: w=%i, h=%i\n", (int)img->base.w, (int)img->base.h);
      g_hash_table_insert(osr->associated_images, g_strdup(qualifier), img);

      g_slice_free1(w*h*4, buf);
    }

  }
}

// exported function: vendor_detect
static bool precipoint_detect(const char *filename,
                              struct _openslide_tifflike *tl,
                              GError **err) {
  if (tl) { // exclude tiff files to speed up detection
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "Is a TIFF file");
    return false;
  }

  // try to open
  struct vmicinfo *vmic = vmic_try_init(filename, err);

  if (vmic != NULL) { 
    // close on success
    vmic_close_container(vmic);
    g_free(vmic);
  }

  return (vmic != NULL);
}

static const struct _openslide_ops precipoint_ops = {
  .paint_region = vmic_paint_region,
  .destroy = vmic_destroy,
};

// exported function: vendor_open 
static bool precipoint_open( openslide_t *osr, const char *filename,
                             struct _openslide_tifflike *tl G_GNUC_UNUSED,
                             struct _openslide_hash *quickhash1,
                             GError **err) {
  bool success;

  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  struct vmicinfo *vmic = vmic_try_init(filename, err);
  if (!vmic) {
    return false;
  }

  osr->data = vmic;

  struct vmic_handle *vh = vmic_handlecache_get(vmic->archive, err);
  if (!vh) {
	  g_prefix_error( err,
                    "precipoint_open: creating new handle for zip archive failed, reason: ");
    goto FAIL;
  }
  zip_t *zi = vh->inner;

  success = vmic_get_properties(osr, zi, quickhash1, err);
  if (!success) goto FAIL;

  success = vmic_create_levels(osr, zi, err);
  if (!success) goto FAIL;

  vmic_collect_associated_images(osr, zi, err);
  if (*err) goto FAIL;

  gchar *hashfilename = g_strdup_printf("%s/%i/0_0.%s",
                                        vmic->dz.folder_name,
                                        vmic->dz.dz_one_tile_level,
                                        vmic->dz.tile_imgformat_str);
  //g_debug("quickhash tile=%s\n", hashfilename);
  
  int64_t zph = _openslide_zip_name_locate( zi, hashfilename, 
                                            ZIP_FL_ENC_RAW|ZIP_FL_NOCASE );
  if (zph==-1) {
	  g_set_error( err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                 "precipoint_open: cannot find image for quickhash, name=%s\n",
	               hashfilename);
    g_free(hashfilename);
    goto FAIL;
  }

  gpointer hashbuf;
  gsize bufsize;
  
  success = _openslide_zip_read_file_data(zi, zph, &hashbuf, &bufsize, err);
  if (!success) {
    g_prefix_error( err, 
                    "precipoint_open: can't read file data from zip, filename=%s, reason: ",
                    hashfilename);    
    g_free(hashfilename);
    goto FAIL;
  }
  
  g_free(hashfilename);

  _openslide_hash_data(quickhash1, hashbuf, bufsize);

  g_slice_free1(bufsize, hashbuf);

  osr->ops = &precipoint_ops;

  vmic_handlecache_put(vmic->archive, vh);

  return true;

FAIL:
  vmic_handlecache_put(vmic->archive, vh);

  vmic_close_container(vmic);
  g_free(vmic);
  osr->data=NULL;
  return false;
}
                      
const struct _openslide_format _openslide_format_precipoint_vmic = {
  .name = _PRECIPOINT_VMICTYPE,
  .vendor = _PRECIPOINT_VENDOR,
  .detect = precipoint_detect,
  .open = precipoint_open,
};
