#ifndef WHOLESLIDE_PRIVATE_H
#define WHOLESLIDE_PRIVATE_H

#include "wholeslide.h"

#include <stdbool.h>
#include <tiffio.h>
#include <jpeglib.h>

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
		      uint32_t *overlaps,
		      uint32_t layer_count,
		      uint32_t *layers);
void _ws_add_jpeg_ops(wholeslide_t *wsd,
		      FILE *f,
		      uint32_t mcu_row_count,
		      int64_t *mcu_row_starts);

/* some JPEG support */
void _ws_jpeg_fancy_src(j_decompress_ptr cinfo, FILE *infile,
			int64_t header_length,
			int64_t start_position);
int64_t _ws_jpeg_fancy_src_get_filepos(j_decompress_ptr cinfo);

#endif
