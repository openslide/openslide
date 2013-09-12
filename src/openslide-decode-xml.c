/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

xmlDoc *_openslide_xml_parse(const char *xml, GError **err) {
  xmlDoc *doc = xmlReadMemory(xml, strlen(xml), "/", NULL,
                              XML_PARSE_NOERROR |
                              XML_PARSE_NOWARNING |
                              XML_PARSE_NONET);
  if (doc == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Could not parse XML");
    return NULL;
  }
  return doc;
}

bool _openslide_xml_has_default_namespace(xmlDoc *doc, const char *ns) {
  xmlNode *root = xmlDocGetRootElement(doc);
  if (ns && root->ns) {
    return !xmlStrcmp(root->ns->href, BAD_CAST ns);
  } else {
    return (!ns && !root->ns);
  }
}

int64_t _openslide_xml_parse_int_attr(xmlNode *node, const char *name,
                                      GError **err) {
  xmlChar *value = xmlGetProp(node, BAD_CAST name);
  if (value == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "No integer attribute \"%s\"", name);
    return -1;
  }

  gchar *endptr;
  int64_t result = g_ascii_strtoll((gchar *) value, &endptr, 10);
  if (value[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid integer attribute \"%s\"", name);
    xmlFree(value);
    return -1;
  }

  xmlFree(value);
  return result;
}

xmlXPathContext *_openslide_xml_xpath_create(xmlDoc *doc) {
  xmlXPathContext *ctx = xmlXPathNewContext(doc);
  if (ctx == NULL) {
    // allocation error, abort
    g_error("xmlXPathNewContext failed");
    // not reached
  }

  // register the document's NS, if any, to a shorter name
  xmlNode *root = xmlDocGetRootElement(doc);
  if (root->ns) {
    xmlXPathRegisterNs(ctx, BAD_CAST "d", root->ns->href);
  }

  return ctx;
}

// return NULL if no matches
xmlXPathObject *_openslide_xml_xpath_eval(xmlXPathContext *ctx,
                                          const char *xpath) {
  xmlXPathObject *result = xmlXPathEvalExpression(BAD_CAST xpath, ctx);
  if (result && (result->nodesetval == NULL ||
                 result->nodesetval->nodeNr == 0)) {
    xmlXPathFreeObject(result);
    return NULL;
  }
  return result;
}

// return NULL unless exactly one match
xmlNode *_openslide_xml_xpath_get_node(xmlXPathContext *ctx,
                                       const char *xpath) {
  xmlXPathObject *result = _openslide_xml_xpath_eval(ctx, xpath);
  xmlNode *obj = NULL;
  if (result && result->nodesetval->nodeNr == 1) {
    obj = result->nodesetval->nodeTab[0];
  }
  xmlXPathFreeObject(result);
  return obj;
}

void _openslide_xml_set_prop_from_xpath(openslide_t *osr,
                                        xmlXPathContext *ctx,
                                        const char *property_name,
                                        const char *xpath) {
  xmlXPathObject *result = xmlXPathEvalExpression(BAD_CAST xpath, ctx);
  if (osr && result && result->nodesetval && result->nodesetval->nodeNr) {
    xmlChar *str = xmlXPathCastToString(result);
    g_hash_table_insert(osr->properties,
                        g_strdup(property_name),
                        g_strdup((char *) str));
    xmlFree(str);
  }
  xmlXPathFreeObject(result);
}
