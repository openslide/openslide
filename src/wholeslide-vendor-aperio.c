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
};

// OpenJPEG memory-read functions
struct _ws_opj_mem_stream_state {
  const tdata_t buf;
  const tsize_t size;
  toff_t offset;
};

static OPJ_UINT32 _ws_opj_mem_stream_read (void *p_buffer,
					   OPJ_UINT32 p_nb_bytes,
					   void *p_user_data) {
  struct _ws_opj_mem_stream_state *ss =
    (struct _ws_opj_mem_stream_state *) p_user_data;

  //  printf("READ: %p, %d\n", p_buffer, p_nb_bytes);

  if (ss->offset == ss->size) {
    return -1; // EOF
  }

  toff_t new_offset = ss->offset + p_nb_bytes;
  size_t bytes_to_read = p_nb_bytes;

  if (new_offset > ss->size) {
    toff_t over = new_offset - ss->size;
    bytes_to_read -= over;
    new_offset = ss->size;
  }

  memcpy(p_buffer, ss->buf + ss->offset, bytes_to_read);
  ss->offset = new_offset;

  return bytes_to_read;
}

static OPJ_SIZE_T _ws_opj_mem_stream_skip (OPJ_SIZE_T p_nb_bytes,
					   void *p_user_data) {
  struct _ws_opj_mem_stream_state *ss =
    (struct _ws_opj_mem_stream_state *) p_user_data;

  if (ss->offset == ss->size) {
    return -1; // EOF
  }
  toff_t new_offset = ss->offset + p_nb_bytes;
  size_t bytes_to_skip = p_nb_bytes;

  if (new_offset > ss->size) {
    toff_t over = new_offset - ss->size;
    bytes_to_skip -= over;
    new_offset = ss->size;
  }

  ss->offset = new_offset;

  return bytes_to_skip;
}

static bool _ws_opj_mem_stream_seek (OPJ_SIZE_T p_nb_bytes,
				     void *p_user_data) {
  struct _ws_opj_mem_stream_state *ss =
    (struct _ws_opj_mem_stream_state *) p_user_data;

  if (p_nb_bytes > ss->size) {
    return false;
  }

  ss->offset = p_nb_bytes;

  return true;
}

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

  return wtt;
}

static void _ws_aperio_tiff_tilereader_read(struct _ws_tiff_tilereader *wtt,
					    uint32_t *dest,
					    uint32_t x, uint32_t y) {
  // get tile number
  ttile_t tile_no = TIFFComputeTile(wtt->tiff, x, y, 0, 0);

  //  printf("aperio reading tile_no: %d\n", tile_no);

  // get tile size
  tsize_t max_tile_size = TIFFTileSize(wtt->tiff);

  // get raw tile
  tdata_t buf = g_slice_alloc(max_tile_size);
  tsize_t size = TIFFReadRawTile(wtt->tiff, tile_no, buf, max_tile_size); // XXX?

  // set source of compressed data
  struct _ws_opj_mem_stream_state stream_state = {
    .buf = buf,
    .size = size,
    .offset = 0,
  };
  opj_stream_t *stream = opj_stream_default_create(true);
  opj_stream_set_read_function(stream, _ws_opj_mem_stream_read);
  opj_stream_set_skip_function(stream, _ws_opj_mem_stream_skip);
  opj_stream_set_seek_function(stream, _ws_opj_mem_stream_seek);
  opj_stream_set_user_data(stream, &stream_state);

  // decode
  OPJ_INT32 tx0;
  OPJ_INT32 ty0;
  OPJ_UINT32 tw;
  OPJ_UINT32 th;
  OPJ_UINT32 ntx;
  OPJ_UINT32 nty;
  opj_codec_t *codec = opj_create_decompress(CODEC_J2K);
  opj_image_t *image;
  opj_read_header(codec, &image,
		  &tx0, &ty0, &tw, &th, &ntx, &nty, stream);


  image = opj_decode(codec, stream);
  opj_end_decompress(codec, stream);
  opj_image_comp_t *comps = image->comps;

  // copy
  for (int i = 0; i < wtt->tile_height * wtt->tile_width; i++) {
    uint8_t Y = comps[0].data[i];
    uint8_t Cr = comps[1].data[i/2];
    uint8_t Cb = comps[2].data[i/2];

    uint8_t A = 255;
    double R = Y + 1.402 * (Cr - 128);
    double G = Y - 0.34414 * (Cb - 128) - 0.71414 * (Cr - 128);
    double B = Y + 1.772 * (Cb - 128);

    if (R > 255) {
      R = 255;
    }
    if (R < 0) {
      R = 0;
    }
    if (G > 255) {
      G = 255;
    }
    if (G < 0) {
      G = 0;
    }
    if (B > 255) {
      B = 255;
    }
    if (B < 0) {
      B = 0;
    }

    dest[i] = A << 24 |
      ((uint8_t) R) << 16 | ((uint8_t) G << 8) | ((uint8_t) B);
  }

  // erase
  g_slice_free1(max_tile_size, buf);
  opj_stream_destroy(stream);
  opj_destroy_codec(codec);
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
