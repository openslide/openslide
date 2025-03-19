#ifndef OPENSLIDE_OPENSLIDE_DECODE_HEVC_H_
#define OPENSLIDE_OPENSLIDE_DECODE_HEVC_H_

#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

typedef void *OpenHevc_Handle;

OpenHevc_Handle _openslide_hevc_decompress_init(GError **err);

bool _openslide_hevc_decode_buffer(const void *src,
                                   int32_t src_len,
                                   uint8_t *dest,
                                   OpenHevc_Handle handle,
                                   GError **err);

void _openslide_hevc_decompress_destroy(OpenHevc_Handle openHevcHandle);

#endif
