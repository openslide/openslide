#ifndef _WHOLESLIDE_PRIVATE_H_
#define _WHOLESLIDE_PRIVATE_H_

#include "wholeslide.h"
#include "tiffio.h"

struct _wholeslide {
  TIFF *tiff;

  uint32_t layer_count;
  uint32_t *layers;
  double *downsamples;

  double objective_power;

  uint32_t overlap_count;
  uint32_t *overlaps;

  uint32_t background_color;
};


#endif
