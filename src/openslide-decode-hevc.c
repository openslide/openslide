#include "openslide-private.h"
#include "openslide-decode-hevc.h"

#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"

// Clamp a value to the range [0, 255]
static uint8_t clamp(int value) {
  return (value < 0) ? 0 : (value > 255) ? 255 : (uint8_t)value;
}

// Convert YUV420P to BGRA
static void yuv420p_to_bgra(uint8_t *dst, const uint8_t *y, const uint8_t *u, const uint8_t *v, int width, int height, int y_stride, int uv_stride) {
  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      // Get Y, U, and V values
      int y_value = y[i * y_stride + j];
      int u_value = u[(i / 2) * uv_stride + (j / 2)];
      int v_value = v[(i / 2) * uv_stride + (j / 2)];

      // Convert YUV to RGB
      int c = y_value - 16;
      int d = u_value - 128;
      int e = v_value - 128;

      int r = clamp((298 * c + 409 * e + 128) >> 8);
      int g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
      int b = clamp((298 * c + 516 * d + 128) >> 8);

      // Pack RGB into BGRA (with alpha = 0)
      dst[(i * width + j) * 4 + 0] = b; // Blue
      dst[(i * width + j) * 4 + 1] = g; // Green
      dst[(i * width + j) * 4 + 2] = r; // Red
      dst[(i * width + j) * 4 + 3] = 0; // Alpha
    }
  }
}

typedef struct OpenHevcWrapperContext {
  AVCodec *codec;
  AVCodecContext *c;
  AVFrame *picture;
  AVPacket pkt;
  AVFrame *BGRA;
} OpenHevcWrapperContext;

OpenHevc_Handle _openslide_hevc_decompress_init(GError **err) {
  OpenHevcWrapperContext *openHevcContext = av_malloc(sizeof(OpenHevcWrapperContext));
  av_init_packet(&openHevcContext->pkt);
  // Find the HEVC decoder.
  openHevcContext->codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
  if (!openHevcContext->codec) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "avcodec_find_decoder() failed");
    return NULL;
  }

  // Allocate a codec context for the decoder.
  openHevcContext->c = avcodec_alloc_context3(openHevcContext->codec);
  // Allocate a frame for decoding.
  openHevcContext->picture = av_frame_alloc();
  openHevcContext->BGRA = av_frame_alloc();
  openHevcContext->c->flags |= AV_CODEC_FLAG_UNALIGNED;
  // Set thread parameters.
  av_opt_set(openHevcContext->c, "thread_type", "slice", 0);
  av_opt_set_int(openHevcContext->c, "threads", 1, 0);
  // Set the decoder id.
  av_opt_set_int(openHevcContext->c->priv_data, "decoder-id", 0, 0);

  // Open the codec.
  if (avcodec_open2(openHevcContext->c, openHevcContext->codec, NULL) < 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "avcodec_open2() failed");
    return NULL;
  }

  return (OpenHevc_Handle) openHevcContext;
}

bool _openslide_hevc_decode_buffer(const void *src,
                                   int32_t src_len,
                                   uint8_t *dest,
                                   OpenHevc_Handle handle,
                                   GError **err) {
  g_assert(src != NULL);
  g_assert(src_len >= 0);
  OpenHevcWrapperContext *openHevcContext = (OpenHevcWrapperContext *) handle;
  // Set the packet data and size.
  openHevcContext->pkt.size = src_len;
  openHevcContext->pkt.data = (uint8_t *) src;
  openHevcContext->pkt.pts = AV_NOPTS_VALUE;

  // Send the packet to the decoder.
  if (avcodec_send_packet(openHevcContext->c, &openHevcContext->pkt) < 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "avcodec_send_packet() failed");
    return false;
  }
  if (avcodec_receive_frame(openHevcContext->c, openHevcContext->picture) < 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "avcodec_receive_frame() failed");
    return false;
  }

  // Ensure the frame is in YUV420P format.
  if (openHevcContext->picture->format != AV_PIX_FMT_YUV420P) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, "Unsupported pixel format (expected YUV420P)");
    return false;
  }

  // Get frame dimensions.
  int width = openHevcContext->c->width;
  int height = openHevcContext->c->height;

  // Get Y, U, and V planes.
  const uint8_t *y_plane = openHevcContext->picture->data[0];
  const uint8_t *u_plane = openHevcContext->picture->data[1];
  const uint8_t *v_plane = openHevcContext->picture->data[2];

  // Get strides for Y and UV planes.
  int y_stride = openHevcContext->picture->linesize[0];
  int uv_stride = openHevcContext->picture->linesize[1];

  // Perform YUV420P to BGRA conversion.
  yuv420p_to_bgra(dest, y_plane, u_plane, v_plane, width, height, y_stride, uv_stride);

  return true;
}

void _openslide_hevc_decompress_destroy(OpenHevc_Handle openHevcHandle) {
  OpenHevcWrapperContext *openHevcContext = (OpenHevcWrapperContext *) openHevcHandle;
  avcodec_free_context(&openHevcContext->c);
  av_frame_free(&openHevcContext->picture);
  av_frame_free(&openHevcContext->BGRA);
  av_packet_unref(&openHevcContext->pkt);
  av_freep(&openHevcContext);
}
