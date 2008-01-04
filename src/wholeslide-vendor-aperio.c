#include "config.h"

#include "wholeslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

static const char APERIO_DESCRIPTION[] = "Aperio Image Library";
static const char APPARENT_MAG[] = "AppMag = ";
static const char MICRONS_PER_PIXEL[] = "MPP = ";
static const char FIELDS_START[] = ", ";

bool _ws_try_aperio(wholeslide_t *wsd, const char *filename) {
  char *tagval;

  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF, not aperio
  }

  TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (tagval && strncmp(APERIO_DESCRIPTION, tagval, strlen(APERIO_DESCRIPTION))) {
    // not aperio
    TIFFClose(tiff);
    return false;
  }

  // parse
  char *fields = strstr(tagval, FIELDS_START);   // fields start after ", "
  if (fields != NULL) {
    char **split_fields = g_strsplit(fields + strlen(FIELDS_START), "|", -1);
    for (char **cur_str = split_fields; *cur_str != NULL; cur_str++) {
      //fprintf(stderr, " XX: %s\n", *cur_str);
      if (g_str_has_prefix(*cur_str, APPARENT_MAG)) {
	wsd->objective_power = g_ascii_strtod(*cur_str + strlen(APPARENT_MAG),
					      NULL);
      }
    }
    g_strfreev(split_fields);
  }

  // for aperio, the tiled layers are the ones we want
  uint32_t layer_count = 0;
  uint32_t *layers = NULL;
  do {
    if (TIFFIsTiled(tiff)) {
      layer_count++;
    }
  } while (TIFFReadDirectory(tiff));
  layers = g_new(uint32_t, layer_count);

  TIFFSetDirectory(tiff, 0);
  uint32_t i = 0;
  do {
    if (TIFFIsTiled(tiff)) {
      layers[i++] = TIFFCurrentDirectory(tiff);
      printf("tiled layer: %d\n", TIFFCurrentDirectory(tiff));
    }
    TIFFReadDirectory(tiff);
  } while (i < layer_count);

  // all set, load up the TIFF-specific ops
  _ws_add_tiff_ops(wsd, tiff, 0, NULL, layer_count, layers);

  return true;
}
