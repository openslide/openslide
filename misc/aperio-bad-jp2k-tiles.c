/* gcc -o aperio-bad-jp2k-tiles aperio-bad-jp2k-tiles.c -lopenjpeg -ltiff */

/* Check for unreadable JP2K tiles in an Aperio slide. */

/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <tiffio.h>
#include <openjpeg.h>

struct decode_state {
  tdir_t dir;
  ttile_t tile;
  void *buf;
  toff_t size;
};

static void error_callback(const char *msg, void *data) {
  struct decode_state *state = data;
  char filename[80];
  FILE *fp;

  printf("Tile %d error: %s", state->tile, msg);
  snprintf(filename, sizeof(filename), "failed-%d-%d", state->dir,
           state->tile);
  fp = fopen(filename, "wb");
  if (fp == NULL) {
    printf("Couldn't open file %s\n", filename);
    return;
  }
  if (fwrite(state->buf, state->size, 1, fp) != 1) {
    printf("Couldn't write file %s\n", filename);
  }
  fclose(fp);
}

static void decode_tile(struct decode_state *state)
{
  opj_cio_t *stream = NULL;
  opj_dinfo_t *dinfo = NULL;
  opj_image_t *image = NULL;

  opj_event_mgr_t event_callbacks = {
    .error_handler = error_callback,
  };

  // init decompressor
  opj_dparameters_t parameters;
  dinfo = opj_create_decompress(CODEC_J2K);
  opj_set_default_decoder_parameters(&parameters);
  opj_setup_decoder(dinfo, &parameters);
  stream = opj_cio_open((opj_common_ptr) dinfo, state->buf, state->size);
  opj_set_event_mgr((opj_common_ptr) dinfo, &event_callbacks, state);

  // decode
  image = opj_decode(dinfo, stream);

  // sanity check
  if (image && image->numcomps != 3) {
    error_callback("numcomps != 3", state);
  }

  // clean up
  if (image) opj_image_destroy(image);
  if (stream) opj_cio_close(stream);
  if (dinfo) opj_destroy_decompress(dinfo);
}

int main(int argc, char **argv)
{
  TIFF *tiff;

  if (argc != 2) {
    fprintf(stderr, "Usage: %s <tiff>\n", argv[0]);
    return 1;
  }

  tiff = TIFFOpen(argv[1], "r");
  if (!tiff) {
    fprintf(stderr, "Couldn't read TIFF\n");
    return 1;
  }

  do {
    uint16_t compression;
    uint32_t depth;
    ttile_t tiles;
    toff_t *sizes;
    struct decode_state state = {0};

    // ensure tiled and JP2K compression
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      fprintf(stderr, "Can't read compression scheme\n");
      TIFFClose(tiff);
      return 1;
    }
    if (!TIFFIsTiled(tiff) || (compression != 33003 && compression != 33005)) {
      continue;
    }

    state.dir = TIFFCurrentDirectory(tiff);
    printf("Directory: %d\n", state.dir);

    // check depth
    if (TIFFGetField(tiff, TIFFTAG_IMAGEDEPTH, &depth) && depth != 1) {
      printf("Depth != 1: %d\n", depth);
      continue;
    }

    // read tiles
    tiles = TIFFNumberOfTiles(tiff);
    if (!TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes)) {
      printf("No tile byte counts\n");
      continue;
    }
    for (state.tile = 0; state.tile < tiles; state.tile++) {
      if (!(state.tile % 50) || state.tile == tiles - 1) {
        fprintf(stderr, "  Reading: %d/%d\r", state.tile, tiles);
      }
      state.size = sizes[state.tile];
      state.buf = malloc(state.size);
      if (TIFFReadRawTile(tiff, state.tile, state.buf, state.size) ==
          state.size) {
        decode_tile(&state);
      } else {
        printf("Tile %d: couldn't read\n", state.tile);
      }
      free(state.buf);
    }
    fprintf(stderr, "\n");
  } while (TIFFReadDirectory(tiff));

  // close
  TIFFClose(tiff);
  return 0;
}
