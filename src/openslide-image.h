#ifndef OPENSLIDE_OPENSLIDE_IMAGE_H_
#define OPENSLIDE_OPENSLIDE_IMAGE_H_
#include <stdio.h>
#include <inttypes.h>

#define BGR24TOARGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[0]) | ((uint32_t)((p)[1]) << 8) |               \
   ((uint32_t)((p)[2]) << 16))

#define BGR48TOARGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[1]) | ((uint32_t)((p)[3]) << 8) |               \
   ((uint32_t)((p)[5]) << 16))

typedef void (*_openslide_bgr_convert_t)(uint8_t *, size_t, uint32_t *);
extern _openslide_bgr_convert_t _openslide_bgr24_to_argb32;
void _openslide_bgr24_to_argb32_ssse3(uint8_t *src, size_t src_len,
                                      uint32_t *dst);
void _openslide_bgr24_to_argb32_avx2(uint8_t *src, size_t src_len,
                                     uint32_t *dst);

extern _openslide_bgr_convert_t _openslide_bgr48_to_argb32;

typedef void (*_openslide_restore_czi_zstd1_t)(uint8_t *, size_t, uint8_t *);
extern _openslide_restore_czi_zstd1_t _openslide_restore_czi_zstd1;

void _openslide_restore_czi_zstd1_sse3(uint8_t *src, size_t src_len,
                                       uint8_t *dst);
void _openslide_restore_czi_zstd1_avx2(uint8_t *src, size_t src_len,
                                       uint8_t *dst);

#endif
