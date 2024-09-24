#include "config_components.h"

#include "libavutil/buffer.h"
#include "libavutil/eval.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/stereo3d.h"
#include "libavutil/time.h"
#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "internal.h"
#include "packet_internal.h"
#include "atsc_a53.h"
#include "sei.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mediaass/sevc_enc.h>

typedef struct {
    AVClass *class;
    // 这里可以添加你的编码器需要的其他字段
    int inited;
    int bypass;
} LowBitrateEncoderContext;

static av_cold int e2enc_init(AVCodecContext *avctx) {

    printf("e2enc_init enter! \n");
    return 0;
}

static int e2enc_encode(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    const char* filename = "e2e.bin";
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open file");
        return -1;  // 文件打开失败
    }

    // 分配数据包，确保分配成功
    int frame_size = 1024 * 1024 * 10; 
    int ret = av_new_packet(pkt, frame_size);
    if (ret < 0) {
        fclose(file);
        return ret;  // 分配失败
    }

    // 读取文件内容到数据包中
    size_t bytesRead = fread(pkt->data, 1, frame_size, file);
    if (bytesRead <= 0) {  // 检查读取是否成功
        av_packet_unref(pkt);
        fclose(file);
        return (bytesRead < 0) ? -1 : 0;  // 读取失败或 EOF
    }

    // 设置有效数据大小
    pkt->size = bytesRead;
    *got_packet = 1;  // 标记已成功生成数据包

    // 关闭文件
    fclose(file);
    
    return 0;
}

static void e2enc_flush(AVCodecContext *avctx)
{
    printf("e2enc_flush enter! \n");
}

static av_cold int e2enc_close(AVCodecContext *avctx) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    // 清理编码器
    return 0;
}

static const AVClass e2enc_class = {
    .class_name = "e2enc",
    .item_name  = av_default_item_name,
    .option     = NULL,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault e2enc_defaults[] = {
    { "b", "2M" },
    { NULL },
};



static const enum AVPixelFormat pix_fmts_all[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV16,
#ifdef X264_CSP_NV21
    AV_PIX_FMT_NV21,
#endif
    AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUV422P10,
    AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_NV20,
#ifdef X264_CSP_I400
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_GRAY10,
#endif
    AV_PIX_FMT_NONE
};

FFCodec ff_e2enc_encoder = {
    .p.name           = "e2enc",
    CODEC_LONG_NAME("End to End Video Encoder"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_E2ENC,
    .p.capabilities   = AV_CODEC_CAP_DR1 ,
    .p.priv_class     = &e2enc_class,
    .p.wrapper_name   = "e2enc",
    .priv_data_size   = sizeof(LowBitrateEncoderContext),
    .init             = e2enc_init,
    FF_CODEC_ENCODE_CB(e2enc_encode),
    .flush            = e2enc_flush,
    .close            = e2enc_close,
    .defaults         = e2enc_defaults,
    .p.pix_fmts       = pix_fmts_all,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS
                      ,
};
