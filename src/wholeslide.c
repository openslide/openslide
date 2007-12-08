#include <stdlib.h>
#include <stdbool.h>

#include "wholeslide-private.h"

static bool extract_info(wholeslide_t *wsd) {
  // determine vendor
  char *desc;

  TIFFGetField(wsd->tiff, TIFFTAG_IMAGEDESCRIPTION, &desc);
  printf("desc: %s\n", desc);

  return true;
}


wholeslide_t *ws_open(const char *filename) {
  // alloc memory
  wholeslide_t *wsd = calloc(1, sizeof(wholeslide_t));

  // open the file
  wsd->tiff = TIFFOpen(filename, "r");

  extract_info(wsd);

  // return
  return wsd;
}


