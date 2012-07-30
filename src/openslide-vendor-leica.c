/*
*  OpenSlide, a library for reading whole slide image files
*
*  Copyright (c) 2007-2010 Carnegie Mellon University
*  Copyright (c) 2011 Google, Inc.
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
* LEICA (scn) BigTIFF support
*
* quickhash comes from what the TIFF backend does
*
*/

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

static const char LEICA_DESCRIPTION[] = "Leica";
static const char LEICA_COLLECTION_TAG[] = "collection";
static const char LEICA_BARCODE_TAG[] = "barcode";
static const char LEICA_IMAGE_TAG[] = "image";

struct level {
	int32_t level_number;
	int64_t width;
};

static int width_compare(gconstpointer a, gconstpointer b) {
	const struct level *la = (const struct level *) a;
	const struct level *lb = (const struct level *) b;

	if (la->width > lb->width) {
		return -1;
	} else if (la->width == lb->width) {
		return 0;
	} else {
		return 1;
	}
}

static gint64 parse_int_prop(xmlNodePtr node, xmlChar *name) {
	gint64 result = 0;
	xmlChar *tmp = xmlGetProp(node, name);
	result = g_ascii_strtoll((gchar *) tmp, NULL, 0);
	xmlFree(tmp);
	return result;
}

static bool parse_xml_description(char *xml, openslide_t *osr, 
	int *out_macro_ifd, GList **out_main_image_ifds, int *level_count) {
	xmlDocPtr doc = NULL;
	xmlNode *root_element = NULL;
	xmlNode *collection = NULL;

	xmlNode *main_image = NULL;
	xmlNode *macro_image = NULL;

	xmlNode *image = NULL;
	xmlNode *view = NULL;

	xmlChar *str = NULL;

	xmlXPathContextPtr context = NULL;
	xmlXPathObjectPtr images_result = NULL;
	xmlXPathObjectPtr result = NULL;

	gint64 collection_width = 0;
	gint64 collection_height = 0;

	gint64 macro_width = 0;
	gint64 macro_height = 0;

	gint64 test_width = 0;
	gint64 test_height = 0;
	gint64 test_ifd = 0;

	bool success = false;

	int i;

	*out_macro_ifd = -1;
	*level_count = 0;

	//try to parse the xml
	doc = xmlParseMemory(xml, strlen(xml));
	if (doc == NULL) {
		// not leica
		goto FAIL;
	}

	root_element = xmlDocGetRootElement(doc);

	//create XPATH context to query the document
	context = xmlXPathNewContext(doc);
	if (context == NULL) {
		g_warning("Error in xmlXPathNewContext");
		goto FAIL;
	}

	//register the document's NS to a shorter name
	xmlXPathRegisterNs(context, BAD_CAST "new", root_element->ns->href);

	//the recognizable structure is the following:
	/*
		scn (root node)
			collection
				barcode
				image
				image
	*/

	result = xmlXPathEvalExpression(BAD_CAST "/new:scn/new:collection", context);
	//the root node should only have one child, named collection, otherwise fail
	if (result == NULL || result->nodesetval->nodeNr != 1) {
		g_warning("Didn't expect more than one collection elements");
		goto FAIL;
	}

	collection = result->nodesetval->nodeTab[0];
	xmlXPathFreeObject(result);
	result = NULL;

	if (osr) {
		result = xmlXPathEvalExpression(BAD_CAST "/new:scn/new:collection/new:barcode", context);
		if (result == NULL || result->nodesetval->nodeNr != 1) {
			g_warning("Didn't find barcode element");
			goto FAIL;
		}

		str = xmlNodeGetContent(result->nodesetval->nodeTab[0]);

		g_hash_table_insert(osr->properties,
			g_strdup("leica.barcode"),
			g_strdup((char *) str));

		xmlFree(str);
		xmlXPathFreeObject(result);
		result = NULL;
	}

	//read collection's size
	collection_width = parse_int_prop(collection, BAD_CAST "sizeX");
	collection_height = parse_int_prop(collection, BAD_CAST "sizeY");

	//get the image nodes
	context->node = collection;
	images_result = xmlXPathEvalExpression(BAD_CAST "new:image", context);
	if (images_result == NULL || images_result->nodesetval->nodeNr == 0) {
		g_warning("Didn't find any images");
		goto FAIL;
	}

	//loop through all image nodes to find the main image and the macro
	for (i = 0; i < images_result->nodesetval->nodeNr; i++) {
		image = images_result->nodesetval->nodeTab[i];

		context->node = image;
		result = xmlXPathEvalExpression(BAD_CAST "new:view", context);

		if (result == NULL || result->nodesetval->nodeNr != 1) {
			g_warning("Didn't find view node");
			goto FAIL;
		}

		test_width = parse_int_prop(result->nodesetval->nodeTab[0], 
									BAD_CAST "sizeX");
		test_height = parse_int_prop(result->nodesetval->nodeTab[0], 
									BAD_CAST "sizeY");

		xmlXPathFreeObject(result);
		result = NULL;

		//we assume that the macro's dimensions are the same as the collection's
		if (test_width == collection_width && 
			test_height == collection_height) {
			macro_image = image;
		} else {
			main_image = image;
		}
	}

	if (main_image == NULL) {
		g_warning("Didn't find main image node");
		goto FAIL;
	}

	context->node = main_image;
	result = xmlXPathEvalExpression(BAD_CAST "new:pixels/new:dimension", context);

	if (result == NULL || result->nodesetval->nodeNr == 0) {
		g_warning("Didn't find any dimensions in the main image");
		goto FAIL;
	}

	//add all the IFDs of the main image to the level list
	for (i = 0; i < result->nodesetval->nodeNr; i++) {
		struct level *l = g_slice_new(struct level);
		l->level_number = parse_int_prop(result->nodesetval->nodeTab[i], 
									BAD_CAST "ifd");
		l->width = parse_int_prop(result->nodesetval->nodeTab[i], 
									BAD_CAST "sizeX");

		*out_main_image_ifds = g_list_append(*out_main_image_ifds, l);
		(*level_count)++;
	}

	xmlXPathFreeObject(result);
	result = NULL;

	if (osr != NULL) {
		//add some more properties from the main image

		result = xmlXPathEvalExpression(BAD_CAST "new:creationDate", context);
		if (result != NULL && result->nodesetval->nodeNr > 0) {
			str = xmlNodeGetContent(result->nodesetval->nodeTab[0]);

			g_hash_table_insert(osr->properties,
				g_strdup("leica.creationDate"),
				g_strdup((char *) str));

			xmlFree(str);
		}
		
		if (result != NULL) {
			xmlXPathFreeObject(result);
			result = NULL;
		}

		result = xmlXPathEvalExpression(BAD_CAST "new:device", context);
		if (result != NULL && result->nodesetval->nodeNr > 0) {
			str = xmlGetProp(result->nodesetval->nodeTab[0], BAD_CAST "version");

			g_hash_table_insert(osr->properties,
				g_strdup("leica.deviceVersion"),
				g_strdup((char *) str));

			xmlFree(str);

			str = xmlGetProp(result->nodesetval->nodeTab[0], BAD_CAST "model");

			g_hash_table_insert(osr->properties,
				g_strdup("leica.deviceModel"),
				g_strdup((char *) str));

			xmlFree(str);
		}
		
		if (result != NULL) {
			xmlXPathFreeObject(result);
			result = NULL;
		}

		result = xmlXPathEvalExpression(BAD_CAST "new:scanSettings/new:objectiveSettings/new:objective", context);
		if (result != NULL && result->nodesetval->nodeNr > 0) {
			str = xmlNodeGetContent(result->nodesetval->nodeTab[0]);

			g_hash_table_insert(osr->properties,
				g_strdup("leica.objective"),
				g_strdup((char *) str));

			xmlFree(str);
		}
		
		if (result != NULL) {
			xmlXPathFreeObject(result);
			result = NULL;
		}

		result = xmlXPathEvalExpression(BAD_CAST "new:scanSettings/new:illuminationSettings/new:numericalAperture", context);
		if (result != NULL && result->nodesetval->nodeNr > 0) {
			str = xmlNodeGetContent(result->nodesetval->nodeTab[0]);

			g_hash_table_insert(osr->properties,
				g_strdup("leica.aperture"),
				g_strdup((char *) str));

			xmlFree(str);
		}
		
		if (result != NULL) {
			xmlXPathFreeObject(result);
			result = NULL;
		}

		result = xmlXPathEvalExpression(BAD_CAST "new:scanSettings/new:illuminationSettings/new:illuminationSource", context);
		if (result != NULL && result->nodesetval->nodeNr > 0) {
			str = xmlNodeGetContent(result->nodesetval->nodeTab[0]);

			g_hash_table_insert(osr->properties,
				g_strdup("leica.illuminationSource"),
				g_strdup((char *) str));

			xmlFree(str);
		}
		
		if (result != NULL) {
			xmlXPathFreeObject(result);
			result = NULL;
		}
	}

	if (macro_image != NULL) {
		context->node = macro_image;
		result = xmlXPathEvalExpression(BAD_CAST "new:pixels/new:dimension", context);

		if (result == NULL || result->nodesetval->nodeNr == 0) {
			g_warning("Didn't find any dimensions in the macro image");
			goto FAIL;
		}

		for (i = 0; i < result->nodesetval->nodeNr; i++) {
			test_width = parse_int_prop(result->nodesetval->nodeTab[i], 
										BAD_CAST "sizeX");
			test_height = parse_int_prop(result->nodesetval->nodeTab[i], 
										BAD_CAST "sizeY");
			test_ifd = parse_int_prop(result->nodesetval->nodeTab[i], 
										BAD_CAST "ifd");

			if (test_width >= macro_width && test_height >= macro_height) {
				macro_width = test_width;
				macro_height = test_height;
				*out_macro_ifd = test_ifd;
			}
		}

		xmlXPathFreeObject(result);
		result = NULL;
	}

	success = true;
FAIL:
	if (result != NULL) {
		xmlXPathFreeObject(result);
	}

	if (images_result != NULL) {
		xmlXPathFreeObject(images_result);
	}

	if (context != NULL) {
		xmlXPathFreeContext(context);
	}

	if (doc != NULL) {
		xmlFreeDoc(doc);
	}
	
	return success;
}

static bool check_directory(TIFF *tiff, uint16 dir_num) {
	if (TIFFSetDirectory(tiff, dir_num) == 0) {
		g_warning("Can't find directory");
		return false;
	}

	// get width
	uint32_t width;
	if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width)) {
		return false;
	}

	// verify that we can read this compression (hard fail if not)
	uint16_t compression;
	if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
		g_warning("Can't read compression scheme");
		return false;
	}

	if (!TIFFIsCODECConfigured(compression)) {
		g_warning("Unsupported TIFF compression: %u", compression);
		return false;
	}

	return true;
}

bool _openslide_try_leica(openslide_t *osr, TIFF *tiff, 
						struct _openslide_hash *quickhash1) {
	GList *level_list = NULL;
	int32_t level_count = 0;
	int32_t *levels = NULL;
	char *tagval;
	int tiff_result;

	int macroIFD = 0;	//which IFD contains the macro image
	int mainImageIFDFrom = 0, mainImageIFDTo = 0;	//main image IFD range

	if (!TIFFIsTiled(tiff)) {
		goto FAIL; // not tiled
	}

	//get the xml description
	tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);

	//check if it containes the literal "Leica"
	if (!tiff_result || (strstr(tagval, LEICA_DESCRIPTION) == NULL)) {
		// not leica
		goto FAIL;
	}

	if (!parse_xml_description(tagval, osr, &macroIFD, &level_list, &level_count)) {
		// unrecognizable xml
		goto FAIL;
	}

	if (osr) {
		g_hash_table_insert(osr->properties,
			g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
			g_strdup("leica"));
	}

	//add macro image if found
	if (macroIFD != -1 && check_directory(tiff, macroIFD)) {
		_openslide_add_tiff_associated_image(
			osr ? osr->associated_images : NULL, "macro", tiff
		);
	}

	// sort tiled levels
	level_list = g_list_sort(level_list, width_compare);

	// copy levels in, while deleting the list
	levels = g_new(int32_t, level_count);
	for (int i = 0; i < level_count; i++) {
		struct level *l = (struct level *)level_list->data;
		if (!check_directory(tiff, l->level_number)) {
			goto FAIL;
		}
		level_list = g_list_delete_link(level_list, level_list);

		levels[i] = l->level_number;
		g_slice_free(struct level, l);
	}

	g_assert(level_list == NULL);

	// all set, load up the TIFF-specific ops
	_openslide_add_tiff_ops(osr, tiff,
		0, NULL,
		level_count, levels,
		_openslide_generic_tiff_tilereader,
		quickhash1);

	if (osr) {
		g_hash_table_remove(osr->properties, "openslide.comment");
		g_hash_table_remove(osr->properties, "tiff.ImageDescription");
	}

	return true;

FAIL:
	// free the level list
	for (GList *i = level_list; i != NULL; i = g_list_delete_link(i, i)) {
		g_slice_free(struct level, i->data);
	}

	if (levels != NULL) {
		g_free(levels);
	}

	return false;
}
