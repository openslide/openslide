#include "config.h"

#include <jasper/jasper.h>

#include "wholeslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

static const char APERIO_DESCRIPTION[] = "Aperio Image Library";

struct _ws_tiff_tilereader {
  TIFF *tiff;
};

static struct _ws_tiff_tilereader *_ws_aperio_tiff_tilereader_create(TIFF *tiff) {
  struct _ws_tiff_tilereader *wtt = g_slice_new(struct _ws_tiff_tilereader);

  wtt->tiff = tiff;
  return wtt;
}

static void _ws_aperio_tiff_tilereader_read(struct _ws_tiff_tilereader *wtt,
					    uint32_t *dest,
					    uint32_t x, uint32_t y) {
  // get tile number
  ttile_t tile_no = TIFFComputeTile(wtt->tiff, x, y, 0, 0);

  // get tile size
  tsize_t max_tile_size = TIFFTileSize(wtt->tiff);

  // get raw tile
  tdata_t buf = g_slice_alloc(max_tile_size);
  tsize_t size = TIFFReadRawTile(wtt->tiff, tile_no, buf, max_tile_size); // XXX?

  // make a jasper stream
  jas_stream_t *jas_stream = jas_stream_memopen(buf, size);

  // make a jasper image
  jas_image_t *jas_image = jpc_decode(jas_stream, "");

  // set proper colorspace
  jas_image_setclrspc(jas_image, JAS_CLRSPC_SYCBCR);
  jas_image_setcmpttype(jas_image, 0,
			JAS_IMAGE_CT_COLOR(JAS_CHANIND_YCBCR_Y));
  jas_image_setcmpttype(jas_image, 1,
			JAS_IMAGE_CT_COLOR(JAS_CHANIND_YCBCR_CB));
  jas_image_setcmpttype(jas_image, 2,
			JAS_IMAGE_CT_COLOR(JAS_CHANIND_YCBCR_CR));

  // set subsampling
  

  // decode it
  

  // erase
  jas_image_destroy(jas_image);
  jas_stream_close(jas_stream);
  g_slice_free1(max_tile_size, buf);
}

static void _ws_aperio_tiff_tilereader_destroy(struct _ws_tiff_tilereader *wtt) {
  g_slice_free(struct _ws_tiff_tilereader, wtt);
}



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
  TIFFSetDirectory(tiff, 0);
  uint16_t compression_mode;
  TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression_mode);

  if (compression_mode == 33003) {
    // special jpeg 2000 aperio thing
    _ws_add_tiff_ops(wsd, tiff, 0, NULL, layer_count, layers,
		     _ws_aperio_tiff_tilereader_create,
		     _ws_aperio_tiff_tilereader_read,
		     _ws_aperio_tiff_tilereader_destroy);
  } else {
    // let libtiff handle it
    _ws_add_tiff_ops(wsd, tiff, 0, NULL, layer_count, layers,
		     _ws_generic_tiff_tilereader_create,
		     _ws_generic_tiff_tilereader_read,
		     _ws_generic_tiff_tilereader_destroy);
  }

  return true;
}
