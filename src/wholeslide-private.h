#ifndef WHOLESLIDE_PRIVATE_H
#define WHOLESLIDE_PRIVATE_H

#include "wholeslide.h"

#include <stdbool.h>
#include <tiffio.h>

/* the main structure */
struct _wholeslide {
  struct _wholeslide_ops *ops;
  void *data;

  uint32_t layer_count;
  uint32_t *layers;
  double *downsamples;

  double objective_power;
};

/* the function pointer structure for backends */
struct _wholeslide_ops {
  void (*read_region)(wholeslide_t *wsd, uint32_t *dest,
		      uint32_t x, uint32_t y,
		      uint32_t layer,
		      uint32_t w, uint32_t h);
  void (*destroy)(wholeslide_t *wsd);
  void (*get_dimensions)(wholeslide_t *wsd, uint32_t layer,
			 uint32_t *w, uint32_t *h);
  const char* (*get_comment)(wholeslide_t *wsd);
};


/* vendor detection and parsing */
bool _ws_try_trestle(wholeslide_t *wsd, const char* filename);
bool _ws_try_hamamatsu(wholeslide_t *wsd, const char* filename);


/* variants on vendors */
void _ws_add_tiff_ops(wholeslide_t *wsd,
		      TIFF *tiff,
		      uint32_t overlap_count,
		      uint32_t *overlaps);

#endif
