/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

/**
 * @file openslide.h
 * The API for the OpenSlide library.
 */

#ifndef OPENSLIDE_OPENSLIDE_H_
#define OPENSLIDE_OPENSLIDE_H_

#include <openslide-features.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>


typedef struct _openslide openslide_t;

/**
 * Do a quick check to see if a whole slide image is valid.
 *
 * @param filename The filename to check.
 * @return If openslide_open() will succeed.
 */
openslide_public
bool openslide_can_open(const char *filename);

/**
 * Open a whole slide image.
 *
 * @param filename The filename to open.
 * @return A new handle to an open whole slide image.
 */
openslide_public
openslide_t *openslide_open(const char *filename);

/**
 * Copy ARGB data from a whole slide image.
 *
 * This function reads and decompresses a region of a whole slide
 * image into the specified memory location. @p dest must be a valid
 * pointer to enough memory to hold the region. To compute the proper
 * size for @p dest, use @p w * @p h * 4.
 *
 * @param osr The whole slide image handle.
 * @param dest The destination buffer for the ARGB data.
 * @param x The top left x-coordinate. Must be non-negative.
 * @param y The top left y-coordinate. Must be non-negative
 * @param layer The desired layer.
 * @param w The width of the region. Must be greater than 0.
 * @param h The height of the region. Must be greater than 0.
 */
openslide_public
void openslide_read_region(openslide_t *osr,
			   uint32_t *dest,
			   int64_t x, int64_t y,
			   int32_t layer,
			   int64_t w, int64_t h);

/**
 * Give a non-blocking hint that a region is likely to be needed soon.
 *
 * @param osr The whole slide image handle.
 * @param x The top left x-coordinate.
 * @param y The top left y-coordinate.
 * @param layer The desired layer.
 * @param w The width of the region.
 * @param h The height of the region.
 * @returns A unique identifier for this prefetch hint.
 */
openslide_public
int openslide_give_prefetch_hint(openslide_t *osr,
				 int64_t x, int64_t y,
				 int32_t layer,
				 int64_t w, int64_t h);

/**
 * Cancel an existing prefetch hint.
 *
 * @param osr The whole slide image handle.
 * @param prefetch_id An identifier returned by openslide_give_prefetch_hint().
 */
openslide_public
void openslide_cancel_prefetch_hint(openslide_t *osr, int prefetch_id);


/**
 * Close a whole slide image handle.
 *
 * @param osr The whole slide image handle.
 */
openslide_public
void openslide_close(openslide_t *osr);

/**
 * Get the number of layers in the whole slide image.
 *
 * @param osr The whole slide image handle.
 * @return The number of layers.
 */
openslide_public
int32_t openslide_get_layer_count(openslide_t *osr);

/**
 * Get the dimensions of layer 0 (the largest layer).
 *
 * @param osr The whole slide image handle.
 * @param[out] w The width of the image.
 * @param[out] h The height of the image.
 */
openslide_public
void openslide_get_layer0_dimensions(openslide_t *osr, int64_t *w, int64_t *h);

/**
 * Get the dimensions of a layer.
 *
 * @param osr The whole slide image handle.
 * @param layer The desired layer.
 * @param[out] w The width of the image.
 * @param[out] h The height of the image.
 */
openslide_public
void openslide_get_layer_dimensions(openslide_t *osr, int32_t layer,
				    int64_t *w, int64_t *h);

/**
 * Get the downsampling factor of a given layer.
 *
 * @param osr The whole slide image handle.
 * @param layer The desired layer.
 * @return The downsampling factor for this layer.
 */
openslide_public
double openslide_get_layer_downsample(openslide_t *osr, int32_t layer);


/**
 * Get the best layer to use for displaying the given downsample.
 *
 * @param osr The whole slide image handle.
 * @param downsample The downsample factor.
 * @return The layer identifier.
 */
openslide_public
int32_t openslide_get_best_layer_for_downsample(openslide_t *osr,
						double downsample);


/**
 * Get the comment (if any) for this image.
 *
 * @param osr The whole slide image handle.
 * @return The comment for this image.
 */
openslide_public
const char *openslide_get_comment(openslide_t *osr);

#endif
