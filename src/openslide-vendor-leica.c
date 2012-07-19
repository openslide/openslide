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
#include <errno.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

static const char LEICA_DESCRIPTION[] = "Leica";

struct layer {
	int32_t layer_number;
	int64_t width;
};

static int width_compare(gconstpointer a, gconstpointer b) {
	const struct layer *la = (const struct layer *) a;
	const struct layer *lb = (const struct layer *) b;

	if (la->width > lb->width) {
		return -1;
	} else if (la->width == lb->width) {
		return 0;
	} else {
		return 1;
	}
}

int parseIntProp(xmlNodePtr node, xmlChar *name)
{
	int result = 0;
	xmlChar *tmp = xmlGetProp(node, name);
	result = atoi((char *) tmp);
	xmlFree(tmp);
	return result;
}

bool parseXmlDescription(char *xml, xmlChar **outBarcode, int *outThumbnailIfd, int *outMainImageIfdFrom, int *outMainImageIfdTo)
{
	xmlDocPtr doc = NULL;
	xmlNode *root_element = NULL;
	xmlNode *collection = NULL, *barcode = NULL, *image1 = NULL, *image2 = NULL;

	xmlXPathContextPtr context;
	xmlXPathObjectPtr result;

	int collectionX = 0, collectionY = 0;
	int image1X = 0, image1Y = 0;
	int image2X = 0, image2Y = 0;

	//try to parse the xml
    doc = xmlParseMemory(xml, strlen(xml));
    if (doc == NULL) {
		// not leica
		goto FAIL;
    }

	root_element = xmlDocGetRootElement(doc);

	//the recognizable structure is the following:
	/*
		scn (root node)
			collection
				barcode
				image
				image
	*/

	collection = xmlFirstElementChild(root_element);
	//the root node should only have one child, named collection, otherwise fail
	if (xmlNextElementSibling(collection) != NULL || xmlStrcmp(collection->name, (const xmlChar *) "collection") != 0)
	{
		printf("Didn't expect more than one collection elements\n");
		goto FAIL;
	}

	barcode = xmlFirstElementChild(collection);
	if (barcode == NULL || xmlStrcmp(barcode->name, (const xmlChar *) "barcode") != 0)
	{
		printf("Didn't find barcode element\n");
		goto FAIL;
	}

	image1 = xmlNextElementSibling(barcode);
	if (image1 == NULL || xmlStrcmp(image1->name, (const xmlChar *) "image") != 0)
	{
		printf("Didn't find first image element\n");
		goto FAIL;
	}

	image2 = xmlNextElementSibling(image1);
	if (image2 == NULL || xmlStrcmp(image2->name, (const xmlChar *) "image") != 0)
	{
		printf("Didn't find second image element\n");
		goto FAIL;
	}

	//there should be no more child nodes from here on
	if (xmlNextElementSibling(image2) != NULL)
	{
		printf("No more elements expected\n");
		goto FAIL;
	}

	//read collection's size
	collectionX = parseIntProp(collection, BAD_CAST "sizeX");
	collectionY = parseIntProp(collection, BAD_CAST "sizeY");

	//create XPATH context to query the document
	context = xmlXPathNewContext(doc);
	if (context == NULL) {
		printf("Error in xmlXPathNewContext\n");
		goto FAIL;
	}

	//register the document's NS to a shorter name
	xmlXPathRegisterNs(context, BAD_CAST "new", root_element->ns->href);
	result = xmlXPathEvalExpression((const xmlChar *) "/new:scn/new:collection/new:image/new:view", context);

	if (result == NULL || result->nodesetval->nodeNr != 2)
	{
		printf("Could not find view elements inside images\n");
		goto FAIL;
	}

	//read first image's size
	image1X = parseIntProp(result->nodesetval->nodeTab[0], BAD_CAST "sizeX");
	image1Y = parseIntProp(result->nodesetval->nodeTab[0], BAD_CAST "sizeY");

	//read second image's size
	image2X = parseIntProp(result->nodesetval->nodeTab[1], BAD_CAST "sizeX");
	image2Y = parseIntProp(result->nodesetval->nodeTab[1], BAD_CAST "sizeY");

	xmlXPathFreeObject(result);

	if (image1X == collectionX && image1Y == collectionY)
	{
		//image 1 is the thumbnail
		context->node = image1;
	}
	else if (image2X == collectionX && image2Y == collectionY)
	{
		//image 2 is the thumbnail
		context->node = image2;
	}
	else 
	{
		printf("Cannot distinct main image from thumbnail");
		goto FAIL;
	}

	//read the first IFD number and set it as the thumbnail IFD
	result = xmlXPathEvalExpression((const xmlChar *) "new:pixels/new:dimension", context);

	if (result == NULL || result->nodesetval->nodeNr == 0)
	{
		printf("no dimensions found");
		if (result != NULL)
			xmlXPathFreeObject(result);
		goto FAIL;
	}

	//add the smaller one as the thumbnail (nodeNr - 1)
	*outThumbnailIfd = parseIntProp(result->nodesetval->nodeTab[result->nodesetval->nodeNr - 1], BAD_CAST "ifd");
	xmlXPathFreeObject(result);

	//read the other image's IFDs to calculate the main image's IFDs range
	if (context->node == image1)
		context->node = image2;
	else
		context->node = image1;

	result = xmlXPathEvalExpression((const xmlChar *) "new:pixels/new:dimension", context);

	if (result == NULL || result->nodesetval->nodeNr == 0)
	{
		printf("no dimensions found");
		if (result != NULL)
			xmlXPathFreeObject(result);
		goto FAIL;
	}

	*outMainImageIfdFrom = parseIntProp(result->nodesetval->nodeTab[0], BAD_CAST "ifd");
	*outMainImageIfdTo = parseIntProp(result->nodesetval->nodeTab[result->nodesetval->nodeNr - 1], BAD_CAST "ifd");

	xmlXPathFreeObject(result);

	*outBarcode = xmlNodeGetContent(barcode);
	if (outBarcode == NULL)
	{
		printf("could not read barcode property");
		goto FAIL;
	}

	if (context != NULL)
		xmlXPathFreeContext(context);
	xmlFreeDoc(doc);
	//xmlCleanupParser();

	return true;

FAIL:
	if (context != NULL)
		xmlXPathFreeContext(context);
	if (doc != NULL)
		xmlFreeDoc(doc);
	//xmlCleanupParser();

	return false;
}

bool _openslide_try_leica(openslide_t *osr, TIFF *tiff, struct _openslide_hash *quickhash1) {
	GList *layer_list = NULL;
	int32_t layer_count = 0;
	int32_t *layers = NULL;
	int32_t current_layer = 0;
	char *tagval;
	int tiff_result;
	xmlChar *barcode = NULL;
	int thumbnailIFD = 0;	//which IFD contains the thumbnail image
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

	if (!parseXmlDescription(tagval, &barcode, &thumbnailIFD, &mainImageIFDFrom, &mainImageIFDTo))
	{
		// unrecognizable xml
		goto FAIL;
	}

	if (osr) {
		g_hash_table_insert(osr->properties,
			g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
			g_strdup("leica"));

		g_hash_table_insert(osr->properties,
			g_strdup("leica.barcode"),
			g_strdup((char *)barcode));

		xmlFree(barcode);
	}

	// accumulate tiled layers
	current_layer = 0;
	layer_count = 0;
	do {
		if (TIFFIsTiled(tiff)) {
			// get width
			uint32_t width;
			if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width)) {
				// oh no
				continue;
			}

			// verify that we can read this compression (hard fail if not)
			uint16_t compression;
			if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
				g_warning("Can't read compression scheme");
				goto FAIL;
			};

			if (!TIFFIsCODECConfigured(compression)) {
				g_warning("Unsupported TIFF compression: %u", compression);
				goto FAIL;
			}

			if (current_layer == thumbnailIFD)
			{
				//thumbnail
				_openslide_add_tiff_associated_image(osr ? osr->associated_images : NULL, "thumbnail", tiff);
			}
			else if (current_layer >= mainImageIFDFrom && current_layer <= mainImageIFDTo)
			{
				// push into list if it belongs to the main image
				struct layer *l = g_slice_new(struct layer);
				l->layer_number = current_layer;
				l->width = width;
				layer_list = g_list_prepend(layer_list, l);
				layer_count++;
			}
		}
		current_layer++;
	} while (TIFFReadDirectory(tiff));

	// sort tiled layers
	layer_list = g_list_sort(layer_list, width_compare);

	// copy layers in, while deleting the list
	layers = g_new(int32_t, layer_count);
	for (int i = 0; i < layer_count; i++) {
		struct layer *l = (struct layer *)layer_list->data;
		layer_list = g_list_delete_link(layer_list, layer_list);

		layers[i] = l->layer_number;
		g_slice_free(struct layer, l);
	}

	g_assert(layer_list == NULL);

	// all set, load up the TIFF-specific ops
	_openslide_add_tiff_ops(osr, tiff,
		0, NULL,
		layer_count, layers,
		_openslide_generic_tiff_tilereader,
		quickhash1);

	return true;

FAIL:
	// free the layer list
	for (GList *i = layer_list; i != NULL; i = g_list_delete_link(i, i)) {
		g_slice_free(struct layer, i->data);
	}

	return false;
}
