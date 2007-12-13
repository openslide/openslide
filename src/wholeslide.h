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
 * Compute minimum buffer size for given image region.
 *
 * @param wsd The whole slide image handle.
 * @return The minimum number of bytes needed to hold the uncompressed image data for the region.
 */
size_t ws_get_region_num_bytes(wholeslide_t *wsd,
			       uint32_t w, uint32_t h);

/**
 * Copy ARGB data from a whole slide image.
 *
 * This function reads and decompresses a region of a whole slide
 * image into the specified memory location. @p dest must be a valid
 * pointer to enough memory to hold the region. To compute the proper
 * size for @p dest, use ws_get_region_num_bytes().
 *
 * @param wsd The whole slide image handle.
 * @param dest The destination buffer for the ARGB data.
 * @param x The top left x-coordinate.
 * @param y The top left y-coordinate.
 * @param layer The desired layer.
 * @param w The width of the region.
 * @param h The height of the region.
 */
void ws_read_region(wholeslide_t *wsd,
		    uint32_t *dest,
		    uint32_t x, uint32_t y,
		    uint32_t layer,
		    uint32_t w, uint32_t h);

/**
 * Give a non-blocking hint that a region is likely to be needed soon.
 *
 * @param wsd The whole slide image handle.
 * @param x The top left x-coordinate.
 * @param y The top left y-coordinate.
 * @param layer The desired layer.
 * @param w The width of the region.
 * @param h The height of the region.
 * @returns A unique identifier for this prefetch hint.
 */
uint32_t ws_give_prefetch_hint(wholeslide_t *wsd,
			       uint32_t x, uint32_t y,
			       uint32_t layer,
			       uint32_t w, uint32_t h);

/**
 * Cancel an existing prefetch hint.
 *
 * @param wsd The whole slide image handle.
 * @param prefetch_id An identifier returned by ws_give_prefetch_hint().
 */
void ws_cancel_prefetch_hint(wholeslide_t *wsd, uint32_t prefetch_id);


/**
 * Close a whole slide image handle.
 *
 * @param wsd The whole slide image handle.
 */
void ws_close(wholeslide_t *wsd);

/**
 * Get the number of layers in the whole slide image.
 *
 * @param wsd The whole slide image handle.
 * @return The number of layers.
 */
uint32_t ws_get_layer_count(wholeslide_t *wsd);

/**
 * Get the dimensions of the baseline image.
 *
 * @param wsd The whole slide image handle.
 * @param[out] w The width of the image.
 * @param[out] h The height of the image.
 */
void ws_get_baseline_dimensions(wholeslide_t *wsd, uint32_t *w, uint32_t *h);

/**
 * Get the dimensions of a layer.
 *
 * @param wsd The whole slide image handle.
 * @param layer The desired layer.
 * @param[out] w The width of the image.
 * @param[out] h The height of the image.
 */
void ws_get_layer_dimensions(wholeslide_t *wsd, uint32_t layer,
			     uint32_t *w, uint32_t *h);

/**
 * Get the downsampling factor of a given layer.
 *
 * @param wsd The whole slide image handle.
 * @param layer The desired layer.
 * @return The downsampling factor for this layer.
 */
double ws_get_layer_downsample(wholeslide_t *wsd, uint32_t layer);


/**
 * Get the best layer to use for displaying the given downsample.
 *
 * @param wsd The whole slide image handle.
 * @param downsample The downsample factor.
 * @return The layer identifier.
 */
uint32_t ws_get_best_layer_for_downsample(wholeslide_t *wsd, double downsample);


/**
 * Get the comment (if any) for this image.
 *
 * @param wsd The whole slide image handle.
 * @return The comment for this image.
 */
const char *ws_get_comment(wholeslide_t *wsd);

#endif
