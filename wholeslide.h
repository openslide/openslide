#ifndef _WHOLESLIDE_H_
#define _WHOLESLIDE_H_

#include <stdio.h>
#include <stdint.h>


typedef struct _wholeslide wholeslide_t;

/**
 * Open a whole slide image.
 *
 * @param filename The filename to open.
 * @return A new handle to an open whole slide image.
 */
wholeslide_t *ws_open(const char *filename);

/**
 * Compute minimum size for given image region.
 *
 * @param ws The whole slide image handle.
 * @param x The top left x-coordinate.
 * @param y The top left y-coordinate.
 * @param slice The desired slice.
 * @param w The width of the region.
 * @param h The height of the region.
 * @return The minimum number of bytes to allocate to hold the uncompressed image data for the region.
 */
uint32_t ws_get_region_num_bytes(wholeslide_t *ws,
				 uint32_t x, uint32_t y,
				 const char *slice,
				 uint32_t w, uint32_t h);

/**
 * Copy RGBA data from a whole slide image.
 */
void ws_read_region(wholeslide_t *ws,
		    void *dest,
		    uint32_t x, uint32_t y,
		    const char *slice,
		    uint32_t w, uint32_t h);

int ws_prefetch_hint(wholeslide_t *ws,
		     uint32_t x, uint32_t y,
		     const char *slice,
		     uint32_t w, uint32_t h);

void ws_cancel_prefetch_hint(wholeslide_t *ws, int prefetch_id);


/* query list of preferred zoom factors */
uint32_t wholeslide_get_preferred_zoom_count(wholeslide_t *ws);
double wholeslide_get_one_preferred_zoom(wholeslide_t *ws, uint32_t n);

/* destructor */
void wholeslide_destroy(wholeslide_t *ws);


/* TODO: error handling */
/* TODO: metadata */


#endif
