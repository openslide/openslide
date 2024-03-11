
#include <config.h>
#include "openslide-image.h"

#ifdef USE_AVX2
#include <x86intrin.h>

void openslide_bgr24_to_xrgb32_avx2(uint8_t *src, size_t src_len,
                                    uint8_t *dst) {
  /* eight 24-bits pixels a time */
  const int mm_step = 24;
  size_t mm_len = src_len / mm_step - 1;
  __m256i shuffle = _mm256_setr_epi8(
      0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1,
      0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
  __m256i bgr, xrgb;
  for (size_t n = 0; n < mm_len; n++) {
    bgr = _mm256_setr_m128i(_mm_loadu_si128((__m128i *)src),
                            _mm_loadu_si128((__m128i *)(src + 12)));
    xrgb = _mm256_shuffle_epi8(bgr, shuffle);
    _mm256_storeu_si256((__m256i *)dst, xrgb);

    src += mm_step;
    dst += 32;
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
