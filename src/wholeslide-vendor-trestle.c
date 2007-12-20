#include "wholeslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

static const char TRESTLE_SOFTWARE[] = "MedScan";
static const char OVERLAPS_XY[] = "OverlapsXY=";
static const char OBJECTIVE_POWER[] = "Objective Power=";

bool _ws_try_trestle(wholeslide_t *wsd, const char *filename) {
  char *tagval;

  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF, not trestle
  }

  TIFFGetField(tiff, TIFFTAG_SOFTWARE, &tagval);
  if (strncmp(TRESTLE_SOFTWARE, tagval, strlen(TRESTLE_SOFTWARE))) {
    // not trestle
    TIFFClose(tiff);
    return false;
  }

  // parse
  TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);

  uint32_t overlap_count = 0;
  uint32_t *overlaps = NULL;

  char **first_pass = g_strsplit(tagval, ";", -1);
  for (char **cur_str = first_pass; *cur_str != NULL; cur_str++) {
    //fprintf(stderr, " XX: %s\n", *cur_str);
    if (g_str_has_prefix(*cur_str, OVERLAPS_XY)) {
      // found it
      char **second_pass = g_strsplit(*cur_str, " ", -1);

      overlap_count = g_strv_length(second_pass) - 1; // skip fieldname
      overlaps = g_new(uint32_t, overlap_count);

      int i = 0;
      // skip fieldname
      for (char **cur_str2 = second_pass + 1; *cur_str2 != NULL; cur_str2++) {
	overlaps[i] = atoi(*cur_str2);
	i++;
      }

      g_strfreev(second_pass);
    } else if (g_str_has_prefix(*cur_str, OBJECTIVE_POWER)) {
      // found a different one
      wsd->objective_power = g_ascii_strtod(*cur_str + strlen(OBJECTIVE_POWER),
					    NULL);
    }
  }

  // count layers
  do {
    wsd->layer_count++;
  } while (TIFFReadDirectory(tiff));
  wsd->layers = g_new(uint32_t, wsd->layer_count);
  wsd->downsamples = g_new(double, wsd->layer_count);

  // directories are linear
  for (uint32_t i = 0; i < wsd->layer_count; i++) {
    wsd->layers[i] = i;
  }

  // all set, load up the TIFF-specific ops
  _ws_add_tiff_ops(wsd, tiff, overlap_count, overlaps);

  g_strfreev(first_pass);

  return true;
}
