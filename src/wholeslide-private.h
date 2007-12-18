#ifndef WHOLESLIDE_PRIVATE_H
#define WHOLESLIDE_PRIVATE_H

#include "wholeslide.h"
#include "tiffio.h"

#include <stdbool.h>

struct _wholeslide {
  TIFF *tiff;

  uint32_t layer_count;
  uint32_t *layers;
  double *downsamples;

  double objective_power;

  uint32_t overlap_count;
  uint32_t *overlaps;
};


bool _ws_try_trestle(wholeslide_t *wsd);

#endif
