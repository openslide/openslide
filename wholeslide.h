/**
 * @file wholeslide.h
 * The API for the libwholeslide library.
 */

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
 *
 * This function reads and decompresses a region of a whole slide
 * image into the specified memory location. @p dest must be a valid
 * pointer to enough memory to hold the region. To compute the proper
 * size for @p dest, use ws_get_region_num_bytes().
 *
 * @param ws The whole slide image handle.
 * @param dest The destination buffer for the RGBA data.
 * @param x The top left x-coordinate.
 * @param y The top left y-coordinate.
 * @param slice The desired slice.
 * @param w The width of the region.
 * @param h The height of the region.
 */
void ws_read_region(wholeslide_t *ws,
		    void *dest,
		    uint32_t x, uint32_t y,
		    const char *slice,
		    uint32_t w, uint32_t h);

/**
 * Give a hint that a region is likely to be needed soon.
 *
 * @param ws The whole slide image handle.
 * @param x The top left x-coordinate.
 * @param y The top left y-coordinate.
 * @param slice The desired slice.
 * @param w The width of the region.
 * @param h The height of the region.
 * @returns A unique identifier for this prefetch hint.
 */
int ws_prefetch_hint(wholeslide_t *ws,
		     uint32_t x, uint32_t y,
		     const char *slice,
		     uint32_t w, uint32_t h);

/**
 * Cancel an existing prefetch hint.
 *
 * @param ws The whole slide image handle.
 * @param prefetch_id An identifier returned by ws_prefetch_hint().
 */
void ws_cancel_prefetch_hint(wholeslide_t *ws, int prefetch_id);


/**
 * Close a whole slide image handle.
 *
 * @param ws The whole slide image handle.
 */
void ws_close(wholeslide_t *ws);


uint32_t ws_get_slice_count(wholeslide_t *ws);
const char *ws_get_slice_name(wholeslide_t *ws, uint32_t slice_number);
uint32_t ws_get_baseline_height(wholeslide_t *ws);
uint32_t ws_get_baseline_width(wholeslide_t *ws);
uint32_t ws_get_slice_height(wholeslide_t *ws, const char *slice);
uint32_t ws_get_slice_width(wholeslide_t *ws, const char *slice);
double ws_get_slice_downsample(wholeslide_t *ws, const char *slice);

const char *ws_get_comment(wholeslide_t *ws);

#endif
