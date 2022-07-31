/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2012 Carnegie Mellon University
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

/* Read the entirety of slide level 0, using the specified number of threads,
   and report the runtime. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <glib.h>
#include <openslide.h>
#include "openslide-common.h"

#define TILE_SIZE 512

struct state {
  openslide_t *osr;
  GAsyncQueue *jobs;
  GAsyncQueue *completions;
};

struct tile {
  int64_t x;
  int64_t y;
};

static struct tile sentinel;

static void *thread_func(void *data) {
  struct state *state = data;
  struct tile *tile;	
  uint32_t bufsz = TILE_SIZE * TILE_SIZE * sizeof(uint32_t);
  uint32_t *buf = g_slice_alloc(bufsz);

  g_async_queue_push(state->completions, &sentinel);
  while (1) {
    tile = g_async_queue_pop(state->jobs);
    if (tile == &sentinel) {
      break;
    }
    openslide_read_region(state->osr, buf, tile->x, tile->y, 0, TILE_SIZE,
                          TILE_SIZE);
    g_async_queue_push(state->completions, tile);
  }
  g_async_queue_push(state->completions, &sentinel);
  g_slice_free1(bufsz, buf);
  return NULL;
}

int main(int argc, char **argv) {
  struct state state;

  common_fix_argv(&argc, &argv);
  if (argc != 3) {
    printf("Usage: %s <file> <threads>\n", argv[0]);
    return 2;
  }

  int threads = atoi(argv[2]);
  if (threads < 1) {
    printf("Invalid thread count\n");
    return 1;
  }

  // open file
  state.osr = openslide_open(argv[1]);
  if (!state.osr) {
    printf("Unrecognized file\n");
    return 1;
  }
  const char *error = openslide_get_error(state.osr);
  if (error) {
    printf("%s\n", error);
    openslide_close(state.osr);
    return 1;
  }

  // start threads
  state.jobs = g_async_queue_new();
  state.completions = g_async_queue_new();
  for (int i = 0; i < threads; i++) {
    g_thread_unref(g_thread_new("reader", thread_func, &state));
  }

  // wait for threads to start
  for (int i = 0; i < threads; i++) {
    g_async_queue_pop(state.completions);
  }

  // enqueue jobs
  struct tile *tile;
  int priming = 5 * threads;
  int64_t w, h;
  openslide_get_level0_dimensions(state.osr, &w, &h);
  g_autoptr(GTimer) timer = g_timer_new();
  for (int64_t y = 0; y < h; y += TILE_SIZE) {
    for (int64_t x = 0; x < w; x += TILE_SIZE) {
      if (priming) {
        tile = g_slice_new(struct tile);
        priming--;
      } else {
        tile = g_async_queue_pop(state.completions);
      }
      tile->x = x;
      tile->y = y;
      g_async_queue_push(state.jobs, tile);
    }
  }

  // tell threads to stop
  for (int i = 0; i < threads; i++) {
    g_async_queue_push(state.jobs, &sentinel);
  }

  // wait for threads
  while (threads > 0) {
    tile = g_async_queue_pop(state.completions);
    if (tile == &sentinel) {
      threads--;
    } else {
      g_slice_free(struct tile, tile);
    }
  }

  // print error or time
  error = openslide_get_error(state.osr);
  if (error) {
    printf("%s\n", error);
  } else {
    double seconds = g_timer_elapsed(timer, NULL);
    int tiles = (w + TILE_SIZE - 1) * (h + TILE_SIZE - 1) /
                (TILE_SIZE * TILE_SIZE);
    printf("%d tiles in %g seconds -> %g tiles/sec\n", tiles, seconds,
           tiles / seconds);
  }

  // clean up
  openslide_close(state.osr);
  g_async_queue_unref(state.jobs);
  g_async_queue_unref(state.completions);
  return 0;
}
