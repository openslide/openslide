#include <config.h>
#include "openslide-image.h"

static void bgr24_to_argb32_dispatch(uint8_t *src, size_t src_len,
                                     uint32_t *dst);
static void bgr24_to_argb32_generic(uint8_t *src, size_t src_len,
                                    uint32_t *dst);

static void bgr48_to_argb32_dispatch(uint8_t *src, size_t src_len,
                                     uint32_t *dst);

static void restore_czi_zstd1_dispatch(uint8_t *src, size_t src_len,
                                       uint8_t *dst);
static void restore_czi_zstd1_generic(uint8_t *src, size_t src_len,
                                      uint8_t *dst);

#ifdef USE_NEON
static void bgr24_to_argb32_neon(uint8_t *src, size_t src_len, uint32_t *dst);
static void restore_czi_zstd1_neon(uint8_t *src, size_t src_len, uint8_t *dst);
#endif

_openslide_bgr_convert_t _openslide_bgr24_to_argb32 = &bgr24_to_argb32_dispatch;

_openslide_bgr_convert_t _openslide_bgr48_to_argb32 = &bgr48_to_argb32_dispatch;

_openslide_restore_czi_zstd1_t _openslide_restore_czi_zstd1 =
    &restore_czi_zstd1_dispatch;

static void bgr24_to_argb32_generic(uint8_t *src, size_t src_len,
                                    uint32_t *dst) {
  // one 24-bit pixel at a time
  for (size_t i = 0; i < src_len; i += 3, src += 3) {
    *dst++ = BGR24TOARGB32(src);
  }
}

/* on i7-7700, ssse3 and avx2 process inputs at 1.8 GB/s, while non-SIMD is
 * 1.3 GB/s
 */
static void bgr24_to_argb32_dispatch(uint8_t *src, size_t src_len,
                                     uint32_t *dst) {
#ifdef USE_AVX2
  if (__builtin_cpu_supports("avx2")) {
    _openslide_bgr24_to_argb32 = &_openslide_bgr24_to_argb32_avx2;
    return _openslide_bgr24_to_argb32_avx2(src, src_len, dst);
  }
#endif
#ifdef USE_SSSE3
  if (__builtin_cpu_supports("ssse3")) {
    _openslide_bgr24_to_argb32 = &_openslide_bgr24_to_argb32_ssse3;
    return _openslide_bgr24_to_argb32_ssse3(src, src_len, dst);
  }
#endif

#ifdef USE_NEON
  _openslide_bgr24_to_argb32 = &bgr24_to_argb32_neon;
  return bgr24_to_argb32_neon(src, src_len, dst);
#endif

  _openslide_bgr24_to_argb32 = &bgr24_to_argb32_generic;
  return bgr24_to_argb32_generic(src, src_len, dst);
}

static void _openslide_bgr48_to_argb32_generic(uint8_t *src, size_t src_len,
                                               uint32_t *dst) {
  // one 48-bit pixel at a time
  for (size_t i = 0; i < src_len; i += 6, src += 6) {
    *dst++ = BGR48TOARGB32(src);
  }
}

/* Not enough 48 bits RGB slide for testing. No SIMD for now. */
static void bgr48_to_argb32_dispatch(uint8_t *src, size_t src_len,
                                     uint32_t *dst) {

  _openslide_bgr48_to_argb32 = &_openslide_bgr48_to_argb32_generic;
  return _openslide_bgr48_to_argb32_generic(src, src_len, dst);
}

/* czi zstd1 compression mode has an option to pack less significant byte of
 * 16 bits pixels in the first half of image array, and more significant byte
 * in the second half of image array.
 */
static void restore_czi_zstd1_generic(uint8_t *src, size_t src_len,
                                      uint8_t *dst) {
  size_t half_len = src_len / 2;
  uint8_t *p = dst;
  uint8_t *slo = src;
  uint8_t *shi = src + half_len;
  for (size_t i = 0; i < half_len; i++) {
    *p++ = *slo++;
    *p++ = *shi++;
  }
}

static void restore_czi_zstd1_dispatch(uint8_t *src, size_t src_len,
                                       uint8_t *dst) {
#ifdef USE_AVX2
  if (__builtin_cpu_supports("avx2")) {
    _openslide_restore_czi_zstd1 = &_openslide_restore_czi_zstd1_avx2;
    return _openslide_restore_czi_zstd1_avx2(src, src_len, dst);
  }
#endif
#ifdef USE_SSSE3
  if (__builtin_cpu_supports("ssse3")) {
    _openslide_restore_czi_zstd1 = &_openslide_restore_czi_zstd1_sse3;
    return _openslide_restore_czi_zstd1_sse3(src, src_len, dst);
  }
#endif
#ifdef USE_NEON
  _openslide_restore_czi_zstd1 = &restore_czi_zstd1_neon;
  return restore_czi_zstd1_neon(src, src_len, dst);
#endif

  _openslide_restore_czi_zstd1 = &restore_czi_zstd1_generic;
  return restore_czi_zstd1_generic(src, src_len, dst);
}

#ifdef USE_NEON
#include <arm_neon.h>

/* On a Cortex A53, for converting BGR24 to Cairo ARGB32
 *   - w/o neon:  0.3663 GB/s
 *   - with neon: 0.4487 GB/s
 * Neon speed up 1.23x.
 */
static void bgr24_to_argb32_neon(uint8_t *src, size_t src_len, uint32_t *dst) {
  /* four 24-bits pixels a time */
  const int mm_step = 12;
  size_t mm_len = src_len / mm_step - 1;
  uint8x16_t bgr, argb, out;
  const uint8_t shuffle_bgr24_argb32_arr[] = {0, 1, 2, 255, 3, 4,  5,  255,
                                              6, 7, 8, 255, 9, 10, 11, 255};
  const uint8_t opaque_arr[] = {0, 0, 0, 255, 0, 0, 0, 255,
                                0, 0, 0, 255, 0, 0, 0, 255};
  const uint8x16_t shuffle = vld1q_u8(shuffle_bgr24_argb32_arr);
  const uint8x16_t opaque = vld1q_u8(opaque_arr);
  for (size_t n = 0; n < mm_len; n++) {
    bgr = vld1q_u8((const uint8_t *) src);
    argb = vqtbl1q_u8(bgr, shuffle);
    out = vorrq_u8(argb, opaque);
    vst1q_u8((uint8_t *) dst, out);

    src += mm_step;
    dst += 4;
  }

  for (size_t i = mm_len * mm_step; i < src_len; i += 3, src += 3) {
    *dst++ = BGR24TOARGB32(src);
  }
}

static void restore_czi_zstd1_neon(uint8_t *src, size_t src_len, uint8_t *dst) {
  size_t half_len = src_len / 2;
  uint8_t *p = dst;
  uint8_t *slo = src;
  uint8_t *shi = src + half_len;
  const int mm_step = 16;
  uint8x16_t vhi, vlo, tmp;

  size_t len_mm = half_len / mm_step;
  size_t len_remain = half_len % mm_step;
  for (size_t i = 0; i < len_mm; i++) {
    vlo = vld1q_u8((const uint8_t *) slo);
    vhi = vld1q_u8((const uint8_t *) shi);
    tmp = vzip1q_u8(vlo, vhi); // lower half of vlo and vhi
    vst1q_u8(p, tmp);
    p += mm_step;
    tmp = vzip2q_u8(vlo, vhi); // upper half of vlo and vhi
    vst1q_u8(p, tmp);
    p += mm_step;

    slo += mm_step;
    shi += mm_step;
  }

  for (size_t i = 0; i < len_remain; i++) {
    *p++ = *slo++;
    *p++ = *shi++;
  }
}

#endif
