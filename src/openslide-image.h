

#ifndef OPENSLIDE_OPENSLIDE_IMAGE_H_
#define OPENSLIDE_OPENSLIDE_IMAGE_H_
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#define BGR24TOXRGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[0]) | ((uint32_t)((p)[1]) << 8) |               \
   ((uint32_t)((p)[2]) << 16))

#define BGR48TOXRGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[1]) | ((uint32_t)((p)[3]) << 8) |               \
   ((uint32_t)((p)[5]) << 16))

void _openslide_simd_init(void);
void _openslide_bgr24_to_xrgb32(uint8_t *src, size_t src_len, uint8_t *dst);
void _openslide_bgr48_to_xrgb32(uint8_t *src, size_t src_len, uint8_t *dst);
void _openslide_bgr48_to_xrgb32_generic(uint8_t *src, size_t src_len,
                                        uint8_t *dst);
void _openslide_bgr24_to_xrgb32_generic(uint8_t *src, size_t src_len,
                                        uint8_t *dst);
void _openslide_bgr24_to_xrgb32_ssse3(uint8_t *src, size_t src_len,
                                      uint8_t *dst);
void _openslide_bgr24_to_xrgb32_avx2(uint8_t *src, size_t src_len,
                                     uint8_t *dst);
#endif
