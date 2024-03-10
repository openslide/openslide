
#include <config.h>
#include <x86intrin.h>
#include "openslide-image.h"

#ifdef USE_SSSE3
void openslide_bgr24_to_xrgb32_ssse3(uint8_t *src, size_t src_len,
                                     uint8_t *dst) {
  /* four 24-bits pixels a time */
  const int mm_step = 12;
  /* Decrease mm_len by 1 so that the last read is still 16 bytes inside
   * source buffer.
   */
  size_t mm_len = src_len / mm_step - 1;
  __m128i bgr, xrgb;
  __m128i shuffle = _mm_setr_epi8(
      0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
  for (size_t n = 0; n < mm_len; n++) {
    bgr = _mm_lddqu_si128((__m128i const *)src);
    xrgb = _mm_shuffle_epi8(bgr, shuffle);
    _mm_storeu_si128((__m128i *)dst, xrgb);

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
