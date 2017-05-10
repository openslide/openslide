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
#include "openslide-decode-png.h"
#include "openslide-decode-zip.h"
//#include "openslide-decode-deepzoom.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
//#include <math.h>

// VMIC parameters
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

// deepzoom parameters
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


///
///
/// ***** START OF DEEPZOOM SECTION *****
///
///

// deep zoom parameters
struct dzinfo {
// parameters as retrieved from deepzoom.xml
	int32_t dz_level_count;    // total number of levels in DZ structure
	int32_t tilesize;          // DZ parameter: size of tiles
	int32_t overlap;           // DZ parameter: overlap
	int64_t width, height;     // DZ parameter: total dimensions of slide
#define MAX_IFSTR 10
	gchar tile_imgformat_str[MAX_IFSTR+1]; // DZ parameter: type of image (jpg/png/bmp)
	enum image_format tile_format_id;   // ID, same as above

    int32_t os_level_count;    // level count as exposed via openslide after cutoff
	int32_t dz_one_tile_level; // largest level in DZ structure that consists of a single tile

// file and folder names for deepzoom tile tree, no path included
#define MAX_DZFILENAME 50
	gchar key_filename[MAX_DZFILENAME+1];
	gchar folder_name[MAX_DZFILENAME+1];
};

// vmic: zip parameters, includes deep zoom parameters as base class
struct vmicinfo {
	struct dzinfo base;

	// VMIC-specific:
	struct _openslide_ziphandle zip_outer; // outer zip container

	// zip info, if archive is used
	struct _openslide_ziphandle zi; // inner zip container
	int64_t zip_entries;
};



// Extra data for tile inside of grid
struct dz_tileinfo {
	int32_t w, h;

    zip_int64_t zipindex;

//  The following dz tile members would be used for on-demand file search.
//	uint32_t dz_level_id, dz_row, dz_col; 
};

struct dz_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
  int dz_level_id;
  int cols;
  int rows;
  struct dz_tileinfo *matrix; // matrix of tiles
};



// read deepzoom tile and paint it to cairo context
static bool vmic_read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
					  void *extradata G_GNUC_UNUSED,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {

  struct dz_level *l = (struct dz_level *) level;
  struct vmicinfo *vmic = (struct vmicinfo*)osr->data;

  struct dz_tileinfo *tileinfo = &l->matrix[tile_row * l->cols + tile_col];

  // tile size
  int32_t tw = tileinfo->w;
  int32_t th = tileinfo->h;
  zip_int64_t zipindex = tileinfo->zipindex;

//  g_debug("call to vmic_read_tile, level params:l#=%i cols=%i rows=%i matrix=%p\n", l->dz_level_id, l->cols, l->rows, (void*)l->matrix);
//  g_debug("requested tile: levelds=%lf, col=%i, row=%i", (double)l->base.downsample, (int)tile_col, (int)tile_row);
//  g_debug(", w=%i, h=%i, zip=%i, w2=%i, h2=%i, z2=%i\n", (int)tw, (int)th, (int)zipindex, (int)tw2, (int)th2, (int)zipindex2);
//  g_assert(tw==tw2 && th==th2 && zipindex==zipindex2);

   // try to fetch from cache
  struct _openslide_cache_entry *cache_entry;

  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) { // else retrieve JPG from ZIP archive

	int32_t w,h;

	/*
	possible alternative file location without preindexing
	gchar *tilefilename = g_strdup_printf("%s/%i/%i_%i.jpg",_DEEPZOOM_FOLDER,dz_level_id,(int)x,(int)y);
	zip_int64_t zpi = _openslide_zip_name_locate(vmii->zi, tilefilename, ZIP_FL_ENC_RAW); // caution: using ZIP_FL_NOCASE in flags is EXTREMELY slow.
	// NOTE: getting index -1 means the tile doesn't exist. We use this as our "empty" marker

	g_debug("- adding tile dzlevel=%i, x=%i, y=%i, w=%i, h=%i, name=%s, zipindex=%i, remains_x=%i, remains_y=%i\n", (int)dz_level_id, (int)x, (int)y, (int)w, (int)h, tilefilename, (int)zpi, (int)remains_x, (int)remains_y);
	g_free(tilefilename);
	*/

    if (zipindex!=-1)
    {
		if (!_openslide_zip_read_image(&vmic->zi, zipindex, vmic->base.tile_format_id, &tiledata, &w, &h, err))
		{
		  g_prefix_error(err, "Error in function vmic_read_tile: ");
		  return false;
		}
		// the returned buffer at tiledata is allocated with g_slice_alloc

		// check if size of tile is as expected, if not, we exit with an exception
		if (w!=tw || h !=th)
		{
			g_slice_free1((size_t)w * h * 4, tiledata);

			g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
					"vmic_read_tile: size mismatch at tile level/downsample=%.2lf, row=%i, col=%i. expected size=(%i,%i), stored tile=(%i,%i).",
					level->downsample, (int)tile_col, (int)tile_row, (int)tw, (int)th, (int)w, (int)h);

			return false;
		}

	    // reading tile was successful - put it in the cache
	    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
	                         tiledata, (int)w * h * 4,
	                         &cache_entry);
    }

  }

  // draw it

  if (!tiledata)
  {	  // no tile at this position:
	  // don't need to do anything because cairo surface is pre-filled with transparent pixels
	  //cairo_set_source_rgba(cr, 0, 0, 0, 0);
	  //cairo_paint(cr);
  }
  else
  {
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

  if (tiledata)
  {
	  // normal tile is always cached --
	  // done with the cache entry, release it
	  _openslide_cache_entry_unref(cache_entry);
  }

  return true;
}



static bool vmic_paint_region(G_GNUC_UNUSED openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
	  struct dz_level *l = (struct dz_level *) level;
	  //struct vmicinfo *vmic = (struct vmicinfo *)osr->data;

	  //g_debug("call to paint_region: cairo=%p x=%i, y=%i, w=%i, h=%i, level-ds=%lf\n", (void*)cr, (int)x, (int)y, (int)w, (int)h, l->base.downsample);

	  return _openslide_grid_paint_region(l->grid, cr, NULL,
	                                      x / l->base.downsample,
	                                      y / l->base.downsample,
	                                      level, w, h,
	                                      err);
}

// Helper function: find node by name in a node chain, returns NULL if not found.
static xmlNode* _openslide_xml_find_node(xmlNode *node, const xmlChar* name)
{
	xmlNode* cur = node;

	while (cur!=NULL) 
    {  
        if(xmlStrcmp(cur->name, name)==0) {
            break;         
        }
        cur = cur->next;
	}
	return cur;
}

// retrieve deep zoom properties from xml doc
static bool _openslide_xml_get_deepzoom_properties(xmlDoc *xmldoc, struct dzinfo *dzi, GHashTable *properties, GError **err)
{
	bool success = false;
	/* Expected XML structure to fetch as deepzoom parameters - everything else is ignored.
	Image
		TileSize (int)
		PixelPerMicron (double)
		Overlap (int)
		Size
			Width (int)
			Height (int)
		Format (jpg|png|bmp)
	*/

	//dz.props = xmlDocGetRootElement(dz.xmldoc);
	xmlNode *xmlnodeImage = _openslide_xml_find_node(xmldoc->children, (xmlChar*) _DEEPZOOM_PROP_IMAGE_NODE);
	if (xmlnodeImage==NULL) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
					"DZC/XML: cannot find XML %s Element",_DEEPZOOM_PROP_IMAGE_NODE);
		goto FAIL;
	}

	double ppm = _openslide_xml_parse_double_attr(xmlnodeImage,_DEEPZOOM_PROP_PPM, err);
	if (*err) goto FAIL;

	if (ppm==0) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
					"DZC/XML: Cannot retrieve MPP property");
		goto FAIL;
	}

    //g_debug("ppm=%lf\n", ppm);

	success = g_hash_table_insert(	properties,
									g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
									_openslide_format_double(1.0 / ppm));
    if (!success) goto FAIL;

	success = g_hash_table_insert(  properties,
									g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
									_openslide_format_double(1.0 / ppm));
    if (!success) goto FAIL;

	dzi->tilesize = _openslide_xml_parse_int_attr(xmlnodeImage,g_strdup(_DEEPZOOM_PROP_TILESIZE),err);
	dzi->overlap = _openslide_xml_parse_int_attr(xmlnodeImage,g_strdup(_DEEPZOOM_PROP_OVERLAP),err);
    //g_debug("tilesize=%i\n", dzi->tilesize);
    //g_debug("overlap=%i\n", dzi->overlap);

	if (dzi->tilesize <=0 || dzi->overlap < 0 || dzi->overlap >= dzi->tilesize/2)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "DZC/XML: Invalid Overlap %i and/or tilesize %i !", dzi->overlap, dzi->tilesize);
		goto FAIL;
	}

    xmlNode *xmlnodeSize = _openslide_xml_find_node(xmlnodeImage->children, (xmlChar*)_DEEPZOOM_PROP_SIZE_NODE);
	if (xmlnodeSize==NULL)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "DZC/XML: cannot find XML %s Element", _DEEPZOOM_PROP_SIZE_NODE);
		goto FAIL;
	}

	dzi->width = _openslide_xml_parse_int_attr(xmlnodeSize,g_strdup(_DEEPZOOM_PROP_WIDTH),err);
	dzi->height = _openslide_xml_parse_int_attr(xmlnodeSize,g_strdup(_DEEPZOOM_PROP_HEIGHT),err);
	if (dzi->width<=0 || dzi->height<=0) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "DZC/XML: invalid dimensions (w=%i x h=%i)\n", (int)dzi->width, (int)dzi->height);
		goto FAIL;
	}

    //g_debug("overlap=%i\n", dzi->overlap);
	//g_debug("w=%i, h=%i, tsize=%i, ovl=%i\n", (int)dzi->width, (int)dzi->height, (int)dzi->tilesize, (int)dzi->overlap);

	if (*err)
	{	goto FAIL;	}

	xmlChar *str_pictype = xmlGetProp(xmlnodeImage, (const xmlChar*) _DEEPZOOM_PROP_IMAGE_FORMAT);
    if (!str_pictype) goto FAIL;
	//g_debug("format=%s\n", str_pictype);
    
	g_strlcpy(dzi->tile_imgformat_str, (const gchar*) str_pictype, MAX_IFSTR);
    
	dzi->tile_format_id = IMAGE_FORMAT_UNKNOWN;
	if (str_pictype != NULL) {
		if (g_ascii_strcasecmp((const gchar*)str_pictype,"jpg")==0 || g_ascii_strcasecmp((const gchar*)str_pictype,"jpeg")==0)
		{	dzi->tile_format_id = IMAGE_FORMAT_JPEG; }
		else if (g_ascii_strcasecmp((const gchar*)str_pictype,"png")==0)
		{   dzi->tile_format_id = IMAGE_FORMAT_PNG; }
		else if (g_ascii_strcasecmp((const gchar*)str_pictype,"bmp")==0)
		{   dzi->tile_format_id = IMAGE_FORMAT_BMP; }
	}

	if (dzi->tile_format_id == IMAGE_FORMAT_UNKNOWN) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "DZC/XML: Cannot recognize Image format \"%s\".", str_pictype);
		xmlFree(str_pictype);
		goto FAIL;
	}
	xmlFree(str_pictype);

	if (dzi->tile_format_id != IMAGE_FORMAT_JPEG)
	{
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "DZC/XML: at this stage, only tiles with JPEG image format can be processed.");
		goto FAIL;
	}

	// calculate number of levels of deep zoom pyramid
	int longside = MAX(dzi->width, dzi->height);
	int cnt_level = 1;
	dzi->os_level_count = 0;

	// calc by bit shifting (instead of 2-log, as in "ceil(log(longside)/log(2))")
	if(longside>0) // always true
	while (longside > 1) {
		if (dzi->os_level_count==0 && longside <= dzi->tilesize)
		{	// also search for largest one-tiled level
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


// this function searches the .xml or .dzi file that contains
// information about the deepzoom pyramid inside a zip archive.
// if found, "true" is returned and the zip-index to the XML file as well as name of the folder are returned

static bool _openslide_dzz_find_key_file(zip_t *z, struct dzinfo *dzi)
{
	zip_int64_t key_file_id = -1;
	zip_int64_t count = zip_get_num_entries(z,0);
	const char *name;

	for (zip_int64_t i=0; i<count; i++)
	{
		name = zip_get_name(z,i,ZIP_FL_ENC_RAW);

	   	// search for a file with xml or dzi suffix which is not inside a folder
	   	if ( !strchr(name,'\\') && !strchr(name,'/'))
	   	{
	   		if (g_str_has_suffix(name,".dzi") || g_str_has_suffix(name,".xml")
	   				|| g_str_has_suffix(name,".DZI") || g_str_has_suffix(name,".XML"))
	   		{
	   		   	//g_debug("found deepzoom key file, name=%s\n", name);
	   			key_file_id = i;
	   			break;
	   		}
	   	}
	}

	if (key_file_id>=0)
	{	// found!
		g_strlcpy(dzi->key_filename, name, MAX_DZFILENAME-2);
		// now compose corresponding folder name by replacing the ".xml" suffix with "_files"
		g_strlcpy(dzi->folder_name, name, MAX_DZFILENAME-2);
		strcpy(dzi->folder_name + strlen(dzi->folder_name)-4, "_files");
		return true;
	}
	else
	{
		return false;
	}
}

///
///
/// ***** END OF DEEPZOOM SECTION *****
///
///

///
///
/// ***** BEGIN OF VMIC SECTION *****
///
///


// open zip-inside-a-zip container

static struct vmicinfo* vmic_open_container(const char *vmic_filename, bool detection_only, GError **err)
{

	zip_error_t ze;
	zip_source_t *zs;
	bool success;

	struct vmicinfo *vmic = g_new0(struct vmicinfo,1);

	// open outer archive
	zip_error_init(&ze);

	success = _openslide_zip_open_archive(vmic_filename, &vmic->zip_outer, err);
	if (!success) goto FAIL;

	zip_t *zo = vmic->zip_outer.handle;

	zip_int64_t entries_outer = zip_get_num_entries(zo, 0);
	int vmici_index = -1;

	// precipoint file has a deepzoom container with a name ending on ".vmici" or the name "Image"
    // check only first 20 files, to prevent slowdown in case the archive is huge
	for (int i=0; i<MIN(entries_outer,20); i++)
	{	   const char *name = zip_get_name(zo,i,ZIP_FL_ENC_RAW);

	   // The name is either exactly 'Image' or ends with '*.vmici'
	   if ( g_str_has_suffix(name,_PRECIPOINT_INNER_CONTAINER_SUFFIX)
		|| strcmp(name, _PRECIPOINT_INNER_CONTAINER_LEGACY_NAME)==0 ) {
		    //g_debug("checking name=%s ", name);
           
		    //check if it's a zip archive by "magic number"
			zip_file_t *file = zip_fopen_index(zo, i, 0); // this is ok because we don't need to be thread-safe here.
			if (file) {
				uint32_t file_magic=0;
				zip_fread(file, &file_magic, 4);
				zip_fclose(file);

				//g_debug("checking magic=%x\n", (int)file_magic);

				if (file_magic == 0x04034B50) // "PK34"
				{	// this should be the inner archive. No further checks done.
					vmici_index = i;
					break;
				}
			}
	   }
	}

	//g_debug ("found inner zip container at index %i in zip %p\n",(int) vmici_index, (void*)zo);
	if (vmici_index<0) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
				"VMICI image container not found");
		goto FAIL_AND_CLOSE;
	}

	if (detection_only) { // quit if this was a call from vendor_detect

        success = _openslide_zip_close_archive(&vmic->zip_outer);

        return vmic;
	}

	// open inner archive
	zs = zip_source_zip(zo, zo, vmici_index, ZIP_FL_UNCHANGED, 0, 0);
	//g_debug("created zip_source=%p\n", (void*)zs);

	if (zs==NULL) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
		                "Cannot create zip source from VMICI container at zip index %i. Reason unknown.", (int)vmici_index);
		goto FAIL_AND_CLOSE;
	}

	success = _openslide_zip_open_archive_from_source(zs, &vmic->zi, err);
	
	if (!success)
	{
		zip_source_free(zs);
		goto FAIL_AND_CLOSE;
	}

	zip_int64_t zip_entries_inner = zip_get_num_entries(vmic->zi.handle, 0);
	//g_debug("inner archive successfully openend, entry count=%i\n", (int)zip_entries_inner);

	vmic->zip_entries = zip_entries_inner;

	return vmic; //returning valid vmic object

// ---- error conditions

FAIL_AND_CLOSE:
	if (zo)
		_openslide_zip_close_archive(&vmic->zip_outer);

FAIL:
	g_free(vmic);
	return NULL;

}

// close zip containers

static void vmic_close_container(struct vmicinfo *vmic)
{
	if (!vmic) return;
//	g_debug("Closing vmic container, zi=%p, za=%p\n", (void*)vmic->zi.handle, (void*)vmic->zip_outer.handle);
	if (vmic->zi.handle) { _openslide_zip_close_archive(&vmic->zi);} // inner
	if (vmic->zip_outer.handle) { _openslide_zip_close_archive(&vmic->zip_outer); } // outer
}



// Helper function: recursively iterate over xml branch & leaves and dump stuff into properties
// output properties are formatted as "SpecialTag.NodeA.NodeB.Attrib = [Value]"
static void _openslide_convert_xml_tree_to_properties(xmlNode *node, GHashTable *os_properties, const char *propname_prefix)
{
	while (node!=NULL)
	{
		if (node->type == XML_ELEMENT_NODE)
		{
			gchar *elementname = g_strconcat(propname_prefix, ".", node->name, NULL);
			for (xmlAttr* attribute = node->properties;
					attribute!=NULL;
						attribute = attribute->next)
			{
			  xmlChar* value = xmlNodeListGetString(node->doc, attribute->children, 1);
			  //alternative: xmlChar* value = xmlGetProp(node, attribute->name);
			  gchar *propname = g_strconcat(elementname, ".", attribute->name, NULL);
			  //g_debug("adding property=%s value=%s\n", propname, value);
			  g_hash_table_insert(os_properties, g_strdup((gchar*) propname), g_strdup((gchar*) value));
			  g_free (propname);
			  xmlFree(value);
			}
			if (node->children)
				_openslide_convert_xml_tree_to_properties(node->children, os_properties, elementname);
			g_free (elementname);
		}
		else if (node->type == XML_TEXT_NODE)
		{
			xmlChar *content = xmlNodeGetContent(node);
			//g_debug("adding property=%s value=%s\n", propname_prefix, content);
			g_hash_table_insert(os_properties, g_strdup((gchar*)propname_prefix), g_strdup((gchar*)content));
			xmlFree(content);
		}
		node = node->next;
	}
}




// parses all properties specific to VMIC file

static bool vmic_get_properties(openslide_t *osr, struct _openslide_hash *quickhash, GError **err)
{
    bool success;
    xmlDoc *xmldoc;

    struct vmicinfo *vmic = (struct vmicinfo *)osr->data;
    struct dzinfo *dzz = (struct dzinfo *)osr->data;

    _openslide_dzz_find_key_file(vmic->zi.handle, dzz );
    //g_debug("deepzoom key file=%s, folder=%s\n", dzz->key_filename, dzz->folder_name);
    
    // Parsing DEEPZOOM properties
    xmldoc = _openslide_zip_parse_xml_file(&vmic->zi, dzz->key_filename, ZIP_FL_ENC_RAW|ZIP_FL_NOCASE/*|ZIP_FL_NODIR*/, err, quickhash);
	if (!xmldoc) goto FAIL;
    
	success = _openslide_xml_get_deepzoom_properties(xmldoc, dzz, osr->properties, err);
    //g_debug("deepzoom levels=%i, one_tile_level=%i\n", dzz->dz_level_count, dzz->dz_one_tile_level);
	xmlFreeDoc(xmldoc);
	if (!success) goto FAIL;

    if (dzz->overlap != 0) { // this check can be removed as soon as overlapping slides show up
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			                "DZC/XML: DZ Overlap parameter is %i, but as of now (Sept 2016) VMIC tiles are not expected to overlap !", dzz->overlap);
		goto FAIL;
	}

	// Parsing VMIC properties, they should be VMCF/config.osc
    xmldoc = _openslide_zip_parse_xml_file(&vmic->zi, _PRECIPOINT_PROPS_FILENAME, ZIP_FL_ENC_RAW|ZIP_FL_NOCASE/*|ZIP_FL_NODIR*/, err, quickhash);
	if (!xmldoc) goto FAIL;
    
    

	xmlNode *oscconfig = _openslide_xml_find_node(xmldoc->children, (xmlChar*) _PRECIPOINT_PROPS_OSC_NODE);
	if (!oscconfig) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			        "OSC/XML: Can't find osc node (%s) for VMIC parameters in config file \"%s\".",
					_PRECIPOINT_PROPS_OSC_NODE, _PRECIPOINT_PROPS_FILENAME);
		xmlFreeDoc(xmldoc);
		goto FAIL;
	}

	_openslide_convert_xml_tree_to_properties(oscconfig->children,
                                                osr->properties,
                                                _PRECIPOINT_PROPS_PREFIX);

	xmlFreeDoc(xmldoc);

	// gather and expose important props from config.osc
	_openslide_duplicate_double_prop(osr, _PRECIPOINT_PROPPATH_MAGNIFICATION, OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
	
    // copy title/name to "COMMENT" tag
    //_openslide_duplicate_string_prop(osr, _PRECIPOINT_PROPPATH_NAME, OPENSLIDE_PROPERTY_NAME_COMMENT); // this doesn't work hence the following 6 lines of code ->
     
	const char *value = g_hash_table_lookup(osr->properties, _PRECIPOINT_PROPPATH_NAME);
	if (value)	{
		g_hash_table_insert(osr->properties,
				g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
				g_strdup(value)	);
	}

	return true;

FAIL:
	return false;

}



// This function scans the deepzoom archive for tiles whose name matches the appointed level
// and builds a matrix of file indices and image dimensions
// this is to speed up initial open, since file names are not ordered.
static void dzz_find_indices(struct dzinfo *dzi, zip_t *z, const char *folder, int level, int rows, int cols, struct dz_tileinfo * matrix)
{
//	char dummy[3];
	int i;

	// struct timespec tm1, tm2;
	// sscanf pattern - %c is to make sure to reach end of name
	// gchar *pattern = g_strdup_printf("%s%%1[\\/]%i%%1[\\/]%%u_%%u.%s%%c",folder,level,vmi->base.image_format_string);

	// own pattern matching to avoid sscanf
	gchar *prefix1 = g_strdup_printf("%s\\%i\\", folder, level);
	gchar *prefix2 = g_strdup_printf("%s/%i/", folder, level);

	unsigned int prefix_size=strlen(prefix1);
	g_assert(prefix_size==strlen(prefix2));

	//g_debug("scanning catalog %p for prefix1=%s, prefix2=%s, rows=%i, cols=%i\n", (void*)z, prefix1, prefix2, rows, cols);

	for (i=0; i<rows*cols; i++) // preoccupy matrix with "not found marker"
		matrix[i].zipindex = -1;

	zip_int64_t zip_num_entries = zip_get_num_entries(z,0);

	for (int i=0; i<zip_num_entries; i++)
	{	int x=-1,y=-1;

		bool found = false;
		char *fname = (char*) zip_get_name(z, i, ZIP_FL_ENC_RAW);
        //	g_debug("checking file %s...",fname);
		if (memcmp(fname,prefix1,prefix_size)==0 || memcmp(fname, prefix2,prefix_size)==0)
		{
			char *s = fname + prefix_size;
			x = strtol(s, &s, 10);
			if (*s) if (*s++ == '_') {
				
                y = strtol(s, &s, 10);
                
				if (*s) if (*s++ == '.') {
					if (strcmp(s, dzi->tile_imgformat_str)==0)
						found = true;
				}
			}
		}
        //  g_debug("\n");

		//num = sscanf(fn, pattern, dummy, dummy, &x, &y, dummy);

		//g_debug("sscanf returns num=%i, x=%i y=%i from tile name %s..", num, x,y, fn);
		if (!found)
		{
			//g_debug("pattern not recognized => Ignored!\n");
		}
		else if (x<0 || y<0 || x>=cols || y>=rows)
		{
			// tile filename counters out of bounds => ignore
			//g_debug("Coords of tile out of bounds => Ignored!\n");
		}
		else
		{
			// add to matrix. nonexistent tiles will keep the "-1"
			matrix[y*cols+x].zipindex = i;
			//g_debug("Index is %i - OK!\n", (int)i);
		}
	}

	g_free(prefix2);
	g_free(prefix1);
}


// cleanup function to vmic_create_levels

static void vmic_destroy_levels(openslide_t *osr)
{
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

static bool vmic_create_levels(openslide_t *osr, GError **err)
{
	int dz_level_id;
	int os_level_id;

	struct vmicinfo *vmii = (struct vmicinfo *)osr->data;
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

	// for clarification of indices: openslide full image is os_level_id=0, deepzoom full image is dz_level_id = highest dz level 
	for (dz_level_id = dzi->dz_level_count-1; dz_level_id>=dzi->dz_one_tile_level; dz_level_id--)
	{
		//g_debug("deepzoom level=%i, w=%i, h=%i\n", dz_level_id, (int)w, (int)h);

		// we cut off deepzoom pyramid after largest one-tiled level
		// - they don't clutter up the properties
		// - some older tiny VMIC tiles have black borders lines
		// - some openslide clients might have trouble with unexpectedly small levels
		// - we don't really need tiny levels

		struct dz_level *l = g_slice_new0(struct dz_level);
		if (!l) goto FAIL_AND_DESTROY;

		int tiles_down = (h + tilesize - 1) / tilesize;
		int tiles_across = (w + tilesize - 1) / tilesize;
		//g_debug("tiles down=%i, tiles across=%i\n", (int)tiles_down, (int)tiles_across);

		l->base.tile_h = tilesize; // deepzoom tilesize
		l->base.tile_w = tilesize;
		l->base.w = w;	// total pixel size of level
		l->base.h = h;
		l->base.downsample = downsample; // this turns out to 2^level
		l->grid = _openslide_grid_create_tilemap(osr,
					tilesize,
					tilesize,
					vmic_read_tile, NULL/*vmic_tile_free*/);

		l->cols = tiles_across;
		l->rows = tiles_down;
		l->dz_level_id = dz_level_id;

		l->matrix = (struct dz_tileinfo*)g_slice_alloc(sizeof(struct dz_tileinfo) * tiles_across * tiles_down);
		if (!l->matrix) goto FAIL_AND_DESTROY;

		// build a matrix of file indices for all tiles of this level
		dzz_find_indices(dzi, vmii->zi.handle, vmii->base.folder_name, dz_level_id, tiles_down, tiles_across, l->matrix);

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

/*
					// At the moment, we are scanning the zip directory at openening time to make tables of all tiles,
					// so we can later access it by-index only

					// --- alternative approach --- 
					// instead of pre-scanning the directory, We could switch to file search on-demand 
					// because zip archive makes a hash table of the file directory
					
					// If accessing unzipped deepzoom via file system, there's no way around looking for a tile by-name 
					
					gchar *tilefilename = g_strdup_printf("%s/%i/%i_%i.jpg",_DEEPZOOM_FOLDER,dz_level_id,(int)x,(int)y);
					zip_int64_t zpi = _openslide_zip_name_locate(vmii->zi, tilefilename, ZIP_FL_ENC_RAW); // caution: ZIP_FL_NOCASE=slow search!
					
					// NOTE: getting index -1 means the tile doesn't exist. This is our "empty" marker if tile doesn't exist.

					g_debug("- adding tile dzlevel=%i, x=%i, y=%i, w=%i, h=%i, name=%s, zipindex=%i, remains_x=%i, remains_y=%i\n", 
						(int)dz_level_id, (int)x, (int)y, (int)w, (int)h, tilefilename, (int)zpi, (int)remains_x, (int)remains_y);
					g_free(tilefilename);
*/
					struct dz_tileinfo *tileinfo = &l->matrix[y*tiles_across+x];
					tileinfo->w = tile_w; // expected width of tile
					tileinfo->h = tile_h; // expected height of tile

					_openslide_grid_tilemap_add_tile(l->grid,
									x, y,
									offset_x, offset_y,
									tile_w, tile_h,
									NULL);

					remains_x -= tilesize;
			}
			if (remains_x==0)
				g_assert(w % tilesize==0);
			else
				g_assert(remains_x + tilesize == w % tilesize);

			remains_y -= tilesize;
		}
		if (remains_y==0)
			g_assert(h % tilesize==0);
		else
			g_assert(remains_y + tilesize == h % tilesize);

		osr->levels[os_level_id++] = (struct _openslide_level*) l;

		w = (w+1)>>1; // next smaller level
		h = (h+1)>>1;
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


struct zip_associated_image {
	struct _openslide_associated_image base;
	struct _openslide_ziphandle *refzip;
	zip_uint64_t zipindex;
};

static bool vmic_get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *const dest_buf,
                                      GError **err) {

	struct zip_associated_image *img = (struct zip_associated_image*)_img;

	uint32_t *img_buf;
	int32_t w,h;
	bool success;

    //g_debug("requested assoc image: w=%i, h=%i, destbuf=%p\n", (int)w, (int)h, (void*)dest_buf);
        
	success = _openslide_zip_read_image(img->refzip, img->zipindex, IMAGE_FORMAT_JPEG, &img_buf, &w, &h, err);
    
	if (success) {
		if (img->base.w != w || img->base.h != h)
		{
			g_slice_free1((gsize)w*(gsize)h*4, img_buf);

			g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
				"get_associated_image_data: size mismatch of image");

			return false;
		}
		memcpy(dest_buf, img_buf, w*h*4);
		g_slice_free1(w*h*4, img_buf);
	}
	return success;
}

static void vmic_destroy_associated_image(struct _openslide_associated_image *_img) {
	struct zip_associated_image *img = (struct zip_associated_image*)_img;
	g_slice_free1(sizeof(struct zip_associated_image), img);
}

static const struct _openslide_associated_image_ops precipoint_associated_ops = {
  .get_argb_data = vmic_get_associated_image_data,
  .destroy = vmic_destroy_associated_image,
};

static void vmic_collect_associated_images(openslide_t *osr, GError **err)
{
	bool success;

	// so far (as of September 2016) we have only a single associated image in a VMIC slide: a macro

	const char *filename = _PRECIPOINT_MACRO_IMAGE;
	const char *qualifier = "macro";

	struct vmicinfo *vmic = (struct vmicinfo *)osr->data;

	zip_int64_t file_id = _openslide_zip_name_locate(&vmic->zi, filename, ZIP_FL_ENC_RAW|ZIP_FL_NOCASE);

	if (file_id>0)
	{
		uint32_t *buf;
		int32_t w,h;

		success = _openslide_zip_read_image(&vmic->zi, file_id, IMAGE_FORMAT_JPEG, &buf, &w, &h, err);
		if (success) {
			// we found an image
			struct zip_associated_image *img = g_slice_new0(struct zip_associated_image);
			img->base.ops = &precipoint_associated_ops;
			img->base.w = w;
			img->base.h = h;
			img->refzip = &vmic->zi;
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
	    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
	                "Is a TIFF file");
	    return false;
  }

//  if (!debug_file) debug_file = _openslide_fopen("openslide_debug.txt","a",err);

  //g_debug("Call to detect file \"%s\"\n",filename);

  // run an open with test flag set
  struct vmicinfo *vmic = vmic_open_container(filename, true, err);
  if (vmic!=NULL) {
	  vmic_close_container(vmic);
	  g_free(vmic);
  }

  return vmic!=NULL;
}

static const struct _openslide_ops precipoint_ops = {
  .paint_region = vmic_paint_region,
  .destroy = vmic_destroy,
};

// exported function: vendor_open
static bool precipoint_open(openslide_t *osr, const char *filename,
                       struct _openslide_tifflike *tl G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1,
					   GError **err) {
	bool success;

	g_assert(osr->data == NULL);
	g_assert(osr->levels == NULL);

//	if (!debug_file) debug_file = _openslide_fopen("openslide_debug.txt","a",err);

	struct vmicinfo *vmic = vmic_open_container(filename, false, err);
	if (!vmic) return false;

	osr->data = vmic;

	//g_debug("getting properties\n");
	success = vmic_get_properties(osr, quickhash1, err);
	if (!success) goto FAIL;
	//g_debug("creating levels\n");

	success = vmic_create_levels(osr, err);
	if (!success) goto FAIL;

	vmic_collect_associated_images(osr, err);
	// everything retrieved ok, preparing slide for use

	gchar *hashfilename = g_strdup_printf("%s/%i/0_0.%s",
			vmic->base.folder_name,
			vmic->base.dz_one_tile_level,
			vmic->base.tile_imgformat_str);
	//g_debug("Chosen quickhash file=%s\n", hashfilename);
    
    int64_t zph = _openslide_zip_name_locate(&vmic->zi, hashfilename, ZIP_FL_ENC_RAW|ZIP_FL_NOCASE);
    if (zph==-1) {
		g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
			"precipoint_open: cannot find image for quickhash, name=%s\n", hashfilename);
	    g_free(hashfilename);
    	goto FAIL;
    }

    gpointer hashbuf;
    size_t bufsize;
    success = _openslide_zip_read_file_data(&vmic->zi, zph, &hashbuf, &bufsize, err);
    if (!success)
    {
    	g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "precipoint_open: can't read file data from zip, filename=%s, reason=%s\n", hashfilename, *err != NULL ? (*err)->message : "unknown");
	    g_free(hashfilename);
    	goto FAIL;
    }
    g_free(hashfilename);

    _openslide_hash_data(quickhash1,hashbuf,bufsize);

    g_slice_free1(bufsize, hashbuf);

	osr->ops = &precipoint_ops;

	//g_debug("Open VMIC success !!\n");

	return true;

FAIL:
	//g_debug("Open VMIC failure, closing zip.\n");
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

