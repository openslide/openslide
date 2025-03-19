#include "openslide-private.h"
#include "openslide-decode-hevc.h"

#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/cpu.h"
#include "libavutil/imgutils.h"

typedef struct OpenHevcWrapperContext {
    AVCodec *codec;
    AVCodecContext *c;
    AVFrame *picture;
    AVPacket pkt;
    AVFrame *BGRA;
    struct SwsContext *sws_context;
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
    openHevcContext->sws_context = NULL;
    openHevcContext->BGRA = av_frame_alloc();
    openHevcContext->c->flags |= AV_CODEC_FLAG_UNALIGNED;
    /* set thread parameters */
    av_opt_set(openHevcContext->c, "thread_type", "slice", 0);
    av_opt_set_int(openHevcContext->c, "threads", 1, 0);
    /* set the decoder id */
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

    enum AVPixelFormat pix_fmt = (enum AVPixelFormat) openHevcContext->picture->format;
    int width = openHevcContext->c->width;
    int height = openHevcContext->c->height;
    int align = av_cpu_max_align();
    uint8_t *buf = (uint8_t *) av_malloc(av_image_get_buffer_size(AV_PIX_FMT_BGRA, width, height, align));
    av_image_fill_arrays(openHevcContext->BGRA->data,
                         openHevcContext->BGRA->linesize,
                         buf,
                         AV_PIX_FMT_BGRA,
                         width,
                         height,
                         align);

    // Initialize the SWS context for software scaling (YUV to BGRA conversion).
    openHevcContext->sws_context = sws_getCachedContext(openHevcContext->sws_context,
                                                        width,
                                                        height,
                                                        pix_fmt,
                                                        width,
                                                        height,
                                                        AV_PIX_FMT_BGRA,
                                                        SWS_BILINEAR,
                                                        NULL,
                                                        NULL,
                                                        NULL);
    if (!openHevcContext->sws_context) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "sws_getCachedContext() failed");
        av_freep(&buf);
        return false;
    }

    // Perform the conversion (YUV to BGRA).
    sws_scale(openHevcContext->sws_context,
              (const unsigned char *const *) openHevcContext->picture->data,
              openHevcContext->picture->linesize,
              0,
              height,
              openHevcContext->BGRA->data,
              openHevcContext->BGRA->linesize);

    // Copy the converted BGRA data to the output buffer.
    for (int y = 0; y < height; y++) {
        uint8_t *imgLine = openHevcContext->BGRA->data[0] + (y * openHevcContext->BGRA->linesize[0]);
        uint8_t *pixLine = dest + (y * width * 4);
        memcpy(pixLine, imgLine, width * 4);
    }

    // Clean up.
    av_freep(&buf);

    return true;
}

void _openslide_hevc_decompress_destroy(OpenHevc_Handle openHevcHandle) {
    OpenHevcWrapperContext *openHevcContext = (OpenHevcWrapperContext *) openHevcHandle;
    sws_freeContext(openHevcContext->sws_context);
    avcodec_free_context(&openHevcContext->c);
    av_frame_free(&openHevcContext->picture);
    av_frame_free(&openHevcContext->BGRA);
    // av_packet_free(&openHevcContext->pkt);
    av_freep(&openHevcContext);
}
