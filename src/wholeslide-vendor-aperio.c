#include "config.h"

#include "wholeslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

#include <openjpeg.h>

static const char APERIO_DESCRIPTION[] = "Aperio Image Library";

struct _ws_tiff_tilereader {
  TIFF *tiff;
  uint32_t tile_width;
  uint32_t tile_height;

  opj_dinfo_t *decoder;
};


// XXX revisit assumptions that color is always downsampled in x by 2
static struct _ws_tiff_tilereader *_ws_aperio_tiff_tilereader_create(TIFF *tiff) {
  struct _ws_tiff_tilereader *wtt = g_slice_new(struct _ws_tiff_tilereader);

  wtt->tiff = tiff;

  uint32_t w;
  uint32_t h;

  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &w);
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &h);
  wtt->tile_width = w;
  wtt->tile_height = h;

  // make a decoder
  wtt->decoder = opj_create_decompress(CODEC_J2K);

  return wtt;
}

static void _ws_aperio_tiff_tilereader_read(struct _ws_tiff_tilereader *wtt,
					    uint32_t *dest,
					    uint32_t x, uint32_t y) {
  // get tile number
  ttile_t tile_no = TIFFComputeTile(wtt->tiff, x, y, 0, 0);

  printf("aperio reading tile_no: %d\n", tile_no);

  // get tile size
  tsize_t max_tile_size = TIFFTileSize(wtt->tiff);

  // get raw tile
  tdata_t buf = g_slice_alloc(max_tile_size);
  tsize_t size = TIFFReadRawTile(wtt->tiff, tile_no, buf, max_tile_size); // XXX?

  // source of compressed data
  opj_cio_t *j2k_source = opj_cio_open((opj_common_ptr) wtt->decoder, buf, size);

  // decode
  opj_image_t *image = opj_decode(wtt->decoder, j2k_source);

  // erase
  opj_cio_close(j2k_source);
  g_slice_free1(max_tile_size, buf);
}

static void _ws_aperio_tiff_tilereader_destroy(struct _ws_tiff_tilereader *wtt) {
  opj_destroy_decompress(wtt->decoder);
  g_slice_free(struct _ws_tiff_tilereader, wtt);
}



bool _ws_try_aperio(wholeslide_t *wsd, const char *filename) {
  char *tagval;

  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF, not aperio
  }

  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result ||
      (strncmp(APERIO_DESCRIPTION, tagval, strlen(APERIO_DESCRIPTION)) != 0)) {
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

  printf("compression mode: %d\n", compression_mode);

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
