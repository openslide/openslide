#include <config.h>
#include "openslide-image.h"


void _openslide_bgr24_to_argb32(uint8_t *src, size_t src_len, uint32_t *dst) {
  // one 24-bit pixel at a time
  for (size_t i = 0; i < src_len; i += 3, src += 3) {
    *dst++ = (0xFF000000 |
              (uint32_t)(src[0]) |
              ((uint32_t)(src[1]) << 8) |
              ((uint32_t)(src[2]) << 16));
  }
}

void _openslide_bgr48_to_argb32(uint8_t *src, size_t src_len, uint32_t *dst) {
  // one 48-bit pixel at a time
  for (size_t i = 0; i < src_len; i += 6, src += 6) {
    *dst++ = (0xFF000000 |
              (uint32_t)(src[1]) |
              ((uint32_t)(src[3]) << 8) |
              ((uint32_t)(src[5]) << 16));
  }
}
