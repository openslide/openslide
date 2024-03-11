
#include <config.h>
#include "openslide-image.h"

#ifdef CONFIG_OSR_ARCH_X86_64
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
#endif
#ifdef USE_NEON
void _openslide_simd_init(void) {
}
#endif


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
    return openslide_bgr24_to_xrgb32_avx2(src, src_len, dst);
  }
#endif
#ifdef USE_SSSE3
  if (simd_use_ssse3) {
    return openslide_bgr24_to_xrgb32_ssse3(src, src_len, dst);
  }
#endif

#ifdef USE_NEON
  return _openslide_bgr24_to_xrgb32_neon(src, src_len, dst);
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

#ifdef USE_NEON
#include <arm_neon.h>

void _openslide_bgr24_to_xrgb32_neon(uint8_t *src, size_t src_len,
                                     uint8_t *dst) {
  printf("cwdebug: use neon\n");
  /* four 24-bits pixels a time */
  const int mm_step = 12;
  size_t mm_len = src_len / mm_step - 1;
  uint8x16_t bgr, xrgb;
  static const uint8_t shuffle_bgr24_xrgb32_arr[] = {
      0, 1, 2, 255, 3, 4, 5, 255, 6, 7, 8, 255, 9, 10, 11, 255};
  const uint8x16_t shuffle = vld1q_u8(shuffle_bgr24_xrgb32_arr);
  for (size_t n = 0; n < mm_len; n++) {
    bgr = vld1q_u8((const uint8_t *)src);
    xrgb = vqtbl1q_u8(bgr, shuffle);
    vst1q_u8(dst, xrgb);

    src += mm_step;
    dst += 16;
  }

  uint32_t *p = (uint32_t *)dst;
  size_t i = mm_len * mm_step;
  while (i < src_len) {
    *p++ = BGR24TOXRGB32(src);
    i += 3;
    src += 3;
  }
}
#endif
