
#include <config.h>
#include "openslide-image.h"

#ifdef USE_AVX2
#include <x86intrin.h>

void _openslide_bgr24_to_argb32_avx2(uint8_t *src, size_t src_len,
                                     uint32_t *dst) {
  /* eight 24-bits pixels a time */
  const int mm_step = 24;
  size_t mm_len = src_len / mm_step - 1;
  char cff = (char)0xFF;  // specify 0xFF as char to silence sanitizer
  /* Since the alpha will be changed to 255 later, its shuffle control mask can
   * be any value. 0xFF sets alpha to 0.
   */
  __m256i shuffle = _mm256_setr_epi8(
      0, 1, 2, cff, 3, 4, 5, cff, 6, 7, 8, cff, 9, 10, 11, cff,
      0, 1, 2, cff, 3, 4, 5, cff, 6, 7, 8, cff, 9, 10, 11, cff);
  __m256i opaque = _mm256_setr_epi8(
      0, 0, 0, cff, 0, 0, 0, cff, 0, 0, 0, cff, 0, 0, 0, cff,
      0, 0, 0, cff, 0, 0, 0, cff, 0, 0, 0, cff, 0, 0, 0, cff);
  __m256i bgr, argb, out;
  for (size_t n = 0; n < mm_len; n++) {
    /* Load 16 bytes into lower and upper lane of an AVX2 register. In
     * each lane, only first 12 bytes (4 RGB pixels) are used, they are
     * shuffled into 16 bytes (4 ARGB pixels)
     */
    bgr = _mm256_setr_m128i(_mm_loadu_si128((__m128i *)src),
                            _mm_loadu_si128((__m128i *)(src + 12)));
    argb = _mm256_shuffle_epi8(bgr, shuffle);
    out = _mm256_or_si256(argb, opaque);  // change alpha to 255
    _mm256_storeu_si256((__m256i *)dst, out);

    src += mm_step;
    dst += 8;
  }

  for (size_t i = mm_len * mm_step; i < src_len; i += 3, src += 3) {
    *dst++ = BGR24TOARGB32(src);
  }
}

void _openslide_restore_czi_zstd1_avx2(uint8_t *src, size_t src_len,
                                       uint8_t *dst) {
  size_t half_len = src_len / 2;
  uint8_t *p = dst;
  uint8_t *slo = src;
  uint8_t *shi = src + half_len;
  const int mm_step = 32;
  __m256i vhi, vlo, out, tmp1, tmp2;
  size_t len_mm = half_len / mm_step;
  size_t len_remain = half_len % mm_step;
  size_t i;
  for (i = 0; i < len_mm; i++) {
    /* _mm256_loadu_si256 is slightly slower */
    vlo = _mm256_lddqu_si256((__m256i const *) slo);
    vhi = _mm256_lddqu_si256((__m256i const *) shi);
    /* Given two 256 bits register:
     *   - vlo: [a b c d], a-d are 64 bits each.
     *   - vhi: [A B C D], A-D are 64 bits each.
     */
    tmp1 = _mm256_unpacklo_epi8(vlo, vhi); // [Aa-mix Cc-mix] hilo restored
    tmp2 = _mm256_unpackhi_epi8(vlo, vhi); // [Bb-mix Dd-mix] hilo restored

    out = _mm256_permute2x128_si256(tmp1, tmp2, 0x20); // [Aa-mix Bb-mix]
    _mm256_storeu_si256((__m256i *) p, out);
    p += mm_step;
    out = _mm256_permute2x128_si256(tmp1, tmp2, 0x31); // [Cc-mix Dd-mix]
    _mm256_storeu_si256((__m256i *) p, out);
    p += mm_step;

    slo += mm_step;
    shi += mm_step;
  }

  for (i = 0; i < len_remain; i++) {
    *p++ = *slo++;
    *p++ = *shi++;
  }
}

void _openslide_gray16_to_gray8_avx2(uint8_t *src, size_t src_len,
                                     int pixel_real_bits, uint8_t *dst) {
  /* sixteen 16-bits pixels a time */
  int nshift = pixel_real_bits - 8;
  const int mm_step = 32;
  /* Decrease mm_len by 1 so that the last write is still 16 bytes inside
   * dst buffer.
   */
  size_t mm_len = src_len / mm_step - 1;
  __m256i gray8, tmp1, tmp2;
  __m256i hi8 = _mm256_setr_epi8(
      1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1,
      1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1);
  __m256i lo8 = _mm256_setr_epi8(
      0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1,
      0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
  __m256i allzero = _mm256_set_epi64x(0, 0, 0, 0);
  for (size_t n = 0; n < mm_len; n++) {
    tmp1 = _mm256_lddqu_si256((__m256i const *)src); // gray16
    tmp2 = _mm256_srli_epi16(tmp1, nshift);          // right shift
    gray8 = _mm256_shuffle_epi8(tmp2, lo8);          // bits 0-7 of gray16

    /* check after right shift, whether the high 8 bits are non-zero. Sometimes
     * 14 bits zeiss gray uses more than 14 bits.
     */
    tmp1 = _mm256_shuffle_epi8(tmp2, hi8);           // bits 8-15 of gray16
    /* 0xFF if high 8 bits is non-zero, 0 otherwise. The sign bit of high 8
     * bits is always zero since it has been shift right, therefore it is safe to
     * compare signed with 0.
     */
    tmp2 = _mm256_cmpgt_epi8(tmp1, allzero);
    tmp1 = _mm256_or_si256(tmp2, gray8);
    tmp2 = _mm256_permute4x64_epi64(tmp1, 0x08);
    _mm256_storeu_si256((__m256i *)dst, tmp2);

    src += mm_step;
    dst += 16;
  }

  size_t i = mm_len * mm_step;
  while (i < src_len) {
    *dst++ = gray16togray8(src, nshift);
    i += 2;
    src += 2;
  }
}

#endif
