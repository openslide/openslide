/*
 *  Wholeslide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  Wholeslide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  Wholeslide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Wholeslide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking Wholeslide statically or dynamically with other modules is
 *  making a combined work based on Wholeslide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

/**
 * @file wholeslide.h
 * The API for the libwholeslide library.
 */

#ifndef WHOLESLIDE_H
#define WHOLESLIDE_H

#include <wholeslide-features.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>


typedef struct _wholeslide wholeslide_t;

/**
 * Do a quick check to see if a whole slide image is valid.
 *
 * @param filename The filename to check.
 * @return If ws_open() will succeed.
 */
wholeslide_public
bool ws_can_open(const char *filename);

/**
 * Open a whole slide image.
 *
 * @param filename The filename to open.
 * @return A new handle to an open whole slide image.
 */
wholeslide_public
wholeslide_t *ws_open(const char *filename);

/**
 * Copy ARGB data from a whole slide image.
 *
 * This function reads and decompresses a region of a whole slide
 * image into the specified memory location. @p dest must be a valid
 * pointer to enough memory to hold the region. To compute the proper
 * size for @p dest, use @p w * @p h * 4.
 *
 * @param wsd The whole slide image handle.
 * @param dest The destination buffer for the ARGB data.
 * @param x The top left x-coordinate. Must be non-negative.
 * @param y The top left y-coordinate. Must be non-negative
 * @param layer The desired layer.
 * @param w The width of the region. Must be greater than 0.
 * @param h The height of the region. Must be greater than 0.
 */
wholeslide_public
void ws_read_region(wholeslide_t *wsd,
		    uint32_t *dest,
		    int64_t x, int64_t y,
		    int32_t layer,
		    int64_t w, int64_t h);

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
wholeslide_public
int ws_give_prefetch_hint(wholeslide_t *wsd,
			  int64_t x, int64_t y,
			  int32_t layer,
			  int64_t w, int64_t h);

/**
 * Cancel an existing prefetch hint.
 *
 * @param wsd The whole slide image handle.
 * @param prefetch_id An identifier returned by ws_give_prefetch_hint().
 */
wholeslide_public
void ws_cancel_prefetch_hint(wholeslide_t *wsd, int prefetch_id);


/**
 * Close a whole slide image handle.
 *
 * @param wsd The whole slide image handle.
 */
wholeslide_public
void ws_close(wholeslide_t *wsd);

/**
 * Get the number of layers in the whole slide image.
 *
 * @param wsd The whole slide image handle.
 * @return The number of layers.
 */
wholeslide_public
int32_t ws_get_layer_count(wholeslide_t *wsd);

/**
 * Get the dimensions of layer 0 (the largest layer).
 *
 * @param wsd The whole slide image handle.
 * @param[out] w The width of the image.
 * @param[out] h The height of the image.
 */
wholeslide_public
void ws_get_layer0_dimensions(wholeslide_t *wsd, int64_t *w, int64_t *h);

/**
 * Get the dimensions of a layer.
 *
 * @param wsd The whole slide image handle.
 * @param layer The desired layer.
 * @param[out] w The width of the image.
 * @param[out] h The height of the image.
 */
wholeslide_public
void ws_get_layer_dimensions(wholeslide_t *wsd, int32_t layer,
			     int64_t *w, int64_t *h);

/**
 * Get the downsampling factor of a given layer.
 *
 * @param wsd The whole slide image handle.
 * @param layer The desired layer.
 * @return The downsampling factor for this layer.
 */
wholeslide_public
double ws_get_layer_downsample(wholeslide_t *wsd, int32_t layer);


/**
 * Get the best layer to use for displaying the given downsample.
 *
 * @param wsd The whole slide image handle.
 * @param downsample The downsample factor.
 * @return The layer identifier.
 */
wholeslide_public
int32_t ws_get_best_layer_for_downsample(wholeslide_t *wsd, double downsample);


/**
 * Get the comment (if any) for this image.
 *
 * @param wsd The whole slide image handle.
 * @return The comment for this image.
 */
wholeslide_public
const char *ws_get_comment(wholeslide_t *wsd);

#endif
