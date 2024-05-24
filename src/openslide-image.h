#ifndef OPENSLIDE_OPENSLIDE_IMAGE_H_
#define OPENSLIDE_OPENSLIDE_IMAGE_H_
#include <stdio.h>
#include <inttypes.h>

void _openslide_bgr24_to_argb32(uint8_t *src, size_t src_len, uint32_t *dst);
void _openslide_bgr48_to_argb32(uint8_t *src, size_t src_len, uint32_t *dst);


#endif
