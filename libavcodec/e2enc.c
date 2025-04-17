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
#include "e2e/e2e_enc.h"

typedef struct {
    AVClass *class;
    e2e_t* e2e_hanle;
    e2e_init_t* config;
} e2eEncoderContext;


static av_cold int e2enc_init(AVCodecContext *avctx) {
    e2e_init_t* config = NULL;
    e2e_t* e2e_handle = NULL;
    e2eEncoderContext* ctx = (e2eEncoderContext*)avctx->priv_data;

    config = av_malloc(sizeof(e2e_init_t));
    if(!config)
    {
        av_log(ctx, AV_LOG_ERROR, "e2enc_init config malloc failed!\n");
        return -1;
    }

    config->width = avctx->width;
    config->height = avctx->height;
    config->format = 0;
    config->gop_size = 1;
    config->frames = 1;
    config->quality = 8;


    e2e_handle = e2e_encoder_init(config);
    if(!e2e_handle)
    {
        av_log(ctx, AV_LOG_ERROR, "e2enc_init e2e_handle is NULL. \n");
        return -1;
    }

    ctx->e2e_hanle = e2e_handle;
    ctx->config = config;

    return 0;
}

static int e2enc_encode(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet) {

    av_log(avctx, AV_LOG_DEBUG, "e2enc_encode enter! \n");
    e2eEncoderContext *ctx = avctx->priv_data;
    uint8_t* e2enc_idata;
    int e2enc_idata_size;
    int input_w = frame->width;
    int input_h = frame->height;
    
    if(avctx->pix_fmt == AV_PIX_FMT_RGB24){
        e2enc_idata_size = input_w*input_h*3;
        e2enc_idata = (uint8_t*)malloc(e2enc_idata_size);
        for(int i=0;i<input_h;i++)
        {
            memcpy(e2enc_idata+3*i*input_w,frame->data[0]+i*frame->linesize[0],input_w*3);
        }
    }
    else{
        av_log(ctx, AV_LOG_ERROR, "e2enc_encode input fmt e2e encoder is not support \n");
        return -1;
    }

    e2e_pic_t* pic_in = av_malloc(sizeof(e2e_pic_t));
    pic_in->data = e2enc_idata;
    pic_in->data_size = e2enc_idata_size;

    e2e_bitsteam_t* bit_stream_out = NULL;
    if(ctx->e2e_hanle == NULL)
    {
        av_log(ctx, AV_LOG_ERROR, "e2enc_encode tx->e2e_hanle is NULL\n");
        return -1;
    }
    int ret = e2e_encode(ctx->e2e_hanle,pic_in,&bit_stream_out);
    if(ret!=0)
    {
        av_log(ctx, AV_LOG_ERROR, "e2enc_encode e2e_encode fail.\n");
        return -1;
    }

    int pkt_size = bit_stream_out->bitstream_size;
    ret = av_new_packet(pkt, pkt_size);
    if(ret < 0)
    {
        av_log(ctx, AV_LOG_ERROR, "e2enc_encode av_new_packet fail.\n");
        return ret;  // 分配失败
    }

    pkt->data = bit_stream_out->bitstream;
    pkt->size = bit_stream_out->bitstream_size;
    *got_packet = 1;  // 标记已成功生成数据包
    
    if(e2enc_idata!=NULL)
    {
        free(e2enc_idata);
        e2enc_idata=NULL;
    }

    if(pic_in!=NULL)
    {
        av_free(pic_in);
        pic_in=NULL;
    }
 
    return 0;
}


static void e2enc_flush(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_DEBUG, "e2enc_flush enter.\n");
}

static av_cold int e2enc_close(AVCodecContext *avctx) {
    // 清理编码器
    int ret = 0;
    e2eEncoderContext* ctx = (e2eEncoderContext*)avctx->priv_data;
    e2e_t* e2e_handle = ctx->e2e_hanle;
    if(e2e_handle!=NULL){
        ret = e2e_encoder_clean(e2e_handle);
    }
    if(ctx->config!=NULL)
    {
        av_free(ctx->config);
        ctx->config=NULL;
    }
    return ret;
}

static const AVClass e2enc_class = {
    .class_name = "e2enc_class",
    .item_name  = av_default_item_name,
    .option     = NULL,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault e2enc_defaults[] = {
    { "b", "2M" },
    { NULL },
};



static const enum AVPixelFormat pix_fmts_all[] = {
    AV_PIX_FMT_RGB24,     ///< packed RGB 8:8:8, 24bpp, RGBRGB...
};

const FFCodec ff_e2enc_encoder = {
    .p.name           = "e2enc",
    CODEC_LONG_NAME("End to End Video Encoder"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_E2ENC,
    .p.capabilities   = AV_CODEC_CAP_DR1 ,
    .p.priv_class     = &e2enc_class,
    .p.wrapper_name   = "e2enc",
    .priv_data_size   = sizeof(e2eEncoderContext),
    .init             = e2enc_init,
    FF_CODEC_ENCODE_CB(e2enc_encode),
    .flush            = e2enc_flush,
    .close            = e2enc_close,
    .defaults         = e2enc_defaults,
    .p.pix_fmts       = pix_fmts_all,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS,
};
