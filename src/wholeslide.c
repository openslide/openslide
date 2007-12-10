#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <tiffio.h>
#include <glib.h>

#include "wholeslide-private.h"

static const char *TRESTLE_SOFTWARE = "MedScan";

static const char *OVERLAPS_XY = "OverlapsXY=";
static const char *OBJECTIVE_POWER = "Objective Power=";

static void parse_trestle(wholeslide_t *wsd) {
  char *tagval;

  TIFFGetField(wsd->tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);

  char **first_pass = g_strsplit(tagval, ";", -1);
  for (char **cur_str = first_pass; *cur_str != NULL; cur_str++) {
    //fprintf(stderr, " XX: %s\n", *cur_str);
    if (strncmp(*cur_str, OVERLAPS_XY, strlen(OVERLAPS_XY)) == 0) {
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
    } else if (strncmp(*cur_str,
		       OBJECTIVE_POWER, strlen(OBJECTIVE_POWER)) == 0) {
      // found a different one
      wsd->objective_power = g_ascii_strtod(*cur_str + strlen(OBJECTIVE_POWER),
					    NULL);
    }
  }

  g_strfreev(first_pass);
}



static bool extract_info(wholeslide_t *wsd) {
  // determine vendor
  char *tagval;

  TIFFGetField(wsd->tiff, TIFFTAG_SOFTWARE, &tagval);
  if (!strncmp(TRESTLE_SOFTWARE, tagval, strlen(TRESTLE_SOFTWARE))) {
    // trestle
    // TODO implement and check return values
    parse_trestle(wsd);
  }

  return true;
}

static void print_info(wholeslide_t *wsd) {
  unsigned int i;

  printf("layers: \n");
  for (i = 0; i < wsd->layer_count; i++) {
    printf(" %d\n", wsd->layers[i]);
  }
  printf("\n");

  printf("objective_power: %g\n\n", wsd->objective_power);

  printf("overlaps: \n");
  for (i = 0; i < wsd->overlap_count; i++) {
    printf(" %d\n", wsd->overlaps[i]);
  }
  printf("\n");
}

wholeslide_t *ws_open(const char *filename) {
  // alloc memory
  wholeslide_t *wsd = g_new0(wholeslide_t, 1);

  // open the file
  wsd->tiff = TIFFOpen(filename, "r");

  extract_info(wsd);

  print_info(wsd);

  // return
  return wsd;
}


void ws_close(wholeslide_t *wsd) {
  TIFFClose(wsd->tiff);

  g_free(wsd->layers);
  g_free(wsd->overlaps);
}
