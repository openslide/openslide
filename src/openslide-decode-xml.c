/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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
#include <math.h>
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
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
  g_autoptr(xmlChar) value = xmlGetProp(node, BAD_CAST name);
  if (value == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No integer attribute \"%s\"", name);
    return -1;
  }

  gchar *endptr;
  int64_t result = g_ascii_strtoll((gchar *) value, &endptr, 10);
  if (value[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid integer attribute \"%s\"", name);
    return -1;
  }

  return result;
}

double _openslide_xml_parse_double_attr(xmlNode *node, const char *name,
                                        GError **err) {
  g_autoptr(xmlChar) value = xmlGetProp(node, BAD_CAST name);
  if (value == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No floating-point attribute \"%s\"", name);
    return NAN;
  }

  double result = _openslide_parse_double((char *) value);
  if (isnan(result)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid floating-point attribute \"%s\"", name);
    return NAN;
  }
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
  g_autoptr(xmlXPathObject) result =
    xmlXPathEvalExpression(BAD_CAST xpath, ctx);
  if (result && (result->nodesetval == NULL ||
                 result->nodesetval->nodeNr == 0)) {
    return NULL;
  }
  return g_steal_pointer(&result);
}

// return NULL unless exactly one match
xmlNode *_openslide_xml_xpath_get_node(xmlXPathContext *ctx,
                                       const char *xpath) {
  g_autoptr(xmlXPathObject) result = _openslide_xml_xpath_eval(ctx, xpath);
  if (result && result->nodesetval->nodeNr == 1) {
    return result->nodesetval->nodeTab[0];
  }
  return NULL;
}

char *_openslide_xml_xpath_get_string(xmlXPathContext *ctx,
                                      const char *xpath) {
  g_autoptr(xmlXPathObject) result =
    xmlXPathEvalExpression(BAD_CAST xpath, ctx);
  if (result && result->nodesetval && result->nodesetval->nodeNr) {
    g_autoptr(xmlChar) xmlstr = xmlXPathCastToString(result);
    return g_strdup((char *) xmlstr);
  }
  return NULL;
}

void _openslide_xml_set_prop_from_xpath(openslide_t *osr,
                                        xmlXPathContext *ctx,
                                        const char *property_name,
                                        const char *xpath) {
  char *str = _openslide_xml_xpath_get_string(ctx, xpath);
  if (str) {
    g_hash_table_insert(osr->properties,
                        g_strdup(property_name),
                        str);
  }
}

// xmlFree() is a macro that makes an indirect function call to code in
// another library, which makes CFI unhappy.  Wrap it so we can filter the
// wrapper out of CFI checks.
void _openslide_xml_char_free(xmlChar *c) {
  xmlFree(c);
}
