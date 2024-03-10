
#include <config.h>
#include "openslide-image.h"

static bool simd_use_avx2 = false;
static bool simd_use_ssse3 = false;
static bool simd_initialized = false;

void _openslide_simd_init(void) {
  if (simd_initialized) {
    return;
  }
  simd_use_avx2 = __builtin_cpu_supports("avx2");
  simd_use_ssse3 = __builtin_cpu_supports("ssse3");
  simd_initialized = true;
}

/* xrgb32 is short for CAIRO_FORMAT_RGB24. Each pixel is 32-bit native-endian,
 * with the upper 8 bits unused. Red, Green, and Blue are stored in the
 * remaining 24 bits in that order.
 *
 * on i7-7700, ssse2 and avx2 process inputs at 1.8 GB/s, while non-SIMD is
 * 1.3 GB/s
 */
void _openslide_bgr24_to_xrgb32(uint8_t *src, size_t src_len, uint8_t *dst) {
#ifdef USE_AVX2
  if (simd_use_avx2) {
    return _openslide_bgr24_to_xrgb32_avx2(src, src_len, dst);
  }
#endif
#ifdef USE_SSSE3
  if (simd_use_ssse3) {
    return _openslide_bgr24_to_xrgb32_ssse3(src, src_len, dst);
  }
#endif
  return _openslide_bgr24_to_xrgb32_generic(src, src_len, dst);
}

void _openslide_bgr24_to_xrgb32_generic(uint8_t *src, size_t src_len,
                                        uint8_t *dst) {
  uint32_t *p = (uint32_t *)dst;
  size_t i = 0;
  /* one 24-bits pixels a time */
  while (i < src_len) {
    *p++ = BGR24TOXRGB32(src);
    i += 3;
    src += 3;
  }
}

void _openslide_bgr48_to_xrgb32_generic(uint8_t *src, size_t src_len,
                                               uint8_t *dst) {
  uint32_t *p = (uint32_t *)dst;
  size_t i = 0;
  /* one 48-bits pixels a time */
  while (i < src_len) {
    *p++ = BGR48TOXRGB32(src);
    i += 6;
    src += 6;
  }
}

/* Do not have a good BGR48 czi file for testing. No SIMD for now. */
void _openslide_bgr48_to_xrgb32(uint8_t *src, size_t src_len, uint8_t *dst) {
  return _openslide_bgr48_to_xrgb32_generic(src, src_len, dst);
}
