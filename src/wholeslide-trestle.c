#include "wholeslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>

static const char *TRESTLE_SOFTWARE = "MedScan";

static const char *OVERLAPS_XY = "OverlapsXY=";
static const char *OBJECTIVE_POWER = "Objective Power=";

bool _ws_try_trestle(wholeslide_t *wsd) {
  char *tagval;

  TIFFGetField(wsd->tiff, TIFFTAG_SOFTWARE, &tagval);
  if (strncmp(TRESTLE_SOFTWARE, tagval, strlen(TRESTLE_SOFTWARE))) {
    // not trestle
    return false;
  }

  // parse
  TIFFGetField(wsd->tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);

  char **first_pass = g_strsplit(tagval, ";", -1);
  for (char **cur_str = first_pass; *cur_str != NULL; cur_str++) {
    //fprintf(stderr, " XX: %s\n", *cur_str);
    if (g_str_has_prefix(*cur_str, OVERLAPS_XY)) {
      // found it
      char **second_pass = g_strsplit(*cur_str, " ", -1);

      wsd->overlap_count = g_strv_length(second_pass) - 1; // skip fieldname
      wsd->overlaps = g_new(uint32_t, wsd->overlap_count);

      int i = 0;
      // skip fieldname
      for (char **cur_str2 = second_pass + 1; *cur_str2 != NULL; cur_str2++) {
	wsd->overlaps[i] = atoi(*cur_str2);
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
  } while (TIFFReadDirectory(wsd->tiff));
  wsd->layers = g_new(uint32_t, wsd->layer_count);
  wsd->downsamples = g_new(double, wsd->layer_count);

  // directories are linear
  for (uint32_t i = 0; i < wsd->layer_count; i++) {
    wsd->layers[i] = i;
  }

  // get baseline size
  uint32_t blw, blh;
  ws_get_baseline_dimensions(wsd, &blw, &blh);

  // compute downsamples
  //  printf("downsamples\n");
  for (uint32_t i = 0; i < wsd->layer_count; i++) {
    uint32_t w, h;
    ws_get_layer_dimensions(wsd, i, &w, &h);

    wsd->downsamples[i] = (double) blh / (double) h;

    //    printf(" %g\n", wsd->downsamples[i]);
  }
  //  printf("\n");


  g_strfreev(first_pass);
}
