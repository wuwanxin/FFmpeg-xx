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
#include "decode.h"
#include "internal.h"
#include "packet_internal.h"
#include "atsc_a53.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "e2e/e2e_dec.h"

typedef struct {
    AVClass *class;
    e2e_t* e2e_hanle;
    e2e_init_t* config;
} e2edecoderContext;


static av_cold int e2edec_init(AVCodecContext *avctx) {
    e2e_init_t* config = NULL;
    e2e_t* e2e_handle = NULL;
    e2edecoderContext* ctx = (e2edecoderContext*)avctx->priv_data;

    config = av_malloc(sizeof(e2e_init_t));
    if (!config) {
        av_log(ctx, AV_LOG_ERROR, "config av_malloc failed!\n");
        return AVERROR(ENOMEM);  // 使用FFmpeg的错误码
    }

    e2e_handle = e2e_decoder_init(config);
    if(!e2e_handle)
    {
        av_log(ctx, AV_LOG_ERROR, "config malloc failed. \n");
        return -1;
    }

    av_log(ctx, AV_LOG_DEBUG, "e2e_decoder_init e2e_handle is %p\n",e2e_handle);

    ctx->e2e_hanle = e2e_handle;
    ctx->config = config;
    return 0;
}

static int e2edec_decode(AVCodecContext *avctx, AVFrame *pict,
    int *got_frame, AVPacket *avpkt)
{
    av_log(avctx, AV_LOG_DEBUG, "e2edec_decode enter!\n");
    int ret = 0;
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    e2edecoderContext* ctx = avctx->priv_data;
    e2e_t* e2e_handle = ctx->e2e_hanle;

    e2e_bitsteam_t* bit_stream_input = NULL;
    bit_stream_input = av_malloc(sizeof(e2e_bitsteam_t));
    bit_stream_input->bitstream = buf;
    bit_stream_input->bitstream_size = buf_size;

#if 0
    FILE *in_file = fopen("e2edecode_in.bin", "wb");
    if (in_file != NULL) {
        fwrite(bit_stream_input->bitstream, 1, bit_stream_input->bitstream_size, in_file);
        fclose(in_file);
    } else {
        av_log(ctx, AV_LOG_ERROR, "open e2edecode_in.bin failed%d\n",ret);
    }
#endif
    
    e2e_pic_t* pic_output = NULL;

    // 调用解码函数
    ret = e2e_decode(e2e_handle, bit_stream_input, &pic_output);
    if(ret!=0)
    {
        av_log(ctx, AV_LOG_ERROR, "e2e_decode failed,ret is :%d\n",ret);
        return ret;
    }
    
    // 检查解码输出是否有效
    if (!pic_output || !pic_output->data) {
        av_log(avctx, AV_LOG_ERROR, "No picture data returned from decoder\n");
        free(bit_stream_input);
        return AVERROR_INVALIDDATA;
    }

#if 0
    const char *output_filename = "e2edecode_out.rgb";
    FILE *output_file = fopen(output_filename, "wb");
    if (output_file != NULL) {
        fwrite(pic_output->data, 1, pic_output->data_size, output_file);
        fclose(output_file);
    } else {
        av_log(avctx, AV_LOG_ERROR, "open e2edecode_out.rgb fail.\n");
    }
#endif

    if ((ret = ff_get_buffer(avctx, pict, 0)) < 0)
    {
        av_log(avctx, AV_LOG_ERROR, "e2edec_decode ff_get_buffer failed. ret is %d\n",ret);
        return ret;
    }

    // 其他设置（pts/dts等）
    pict->pts    = avpkt->pts;
    pict->pkt_dts = avpkt->dts;
    pict->key_frame = 1;
    pict->pict_type = AV_PICTURE_TYPE_I;
    pict->flags |= AV_FRAME_FLAG_KEY;

    if (pict->format == AV_PIX_FMT_RGB24) {
        for (int i = 0; i < pict->height; i++) {
            memcpy(pict->data[0] + i * pict->linesize[0],  // 目标行起始地址
                   pic_output->data + i * avctx->width * 3,  // 源行起始地址
                   avctx->width * 3);  // 拷贝有效数据长度
        }
    }


    // 标记成功解码一帧
    *got_frame = 1;

    // 释放资源
    free(bit_stream_input);
    return buf_size;  // 返回消耗的字节数
}

static void e2edec_flush(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_DEBUG, "e2edec_flush enter\n");
}

static av_cold int e2edec_close(AVCodecContext *avctx) {
    // 清理编码器
    int ret = 0;
    e2edecoderContext* ctx = (e2edecoderContext*)avctx->priv_data;
    e2e_t* e2e_handle = ctx->e2e_hanle;
    if(e2e_handle!=NULL){
        ret = e2e_decoder_clean(e2e_handle);
    }

    if(ctx->config!=NULL)
    {
        av_free(ctx->config);
        ctx->config=NULL;
    }
    return ret;
}

static const AVClass e2edec_class = {
    .class_name = "e2edec",
    .item_name  = av_default_item_name,
    .option     = NULL,
    .version    = LIBAVUTIL_VERSION_INT,
};



const FFCodec ff_libe2e_decoder = {
    .p.name           = "e2edec",
    CODEC_LONG_NAME("End to End Video Decoder"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_E2ENC,
    .p.capabilities   = AV_CODEC_CAP_DR1 ,
    .p.priv_class     = &e2edec_class,
    .p.wrapper_name   = "e2edec",
    .priv_data_size   = sizeof(e2edecoderContext),
    .init             = e2edec_init,
    FF_CODEC_DECODE_CB(e2edec_decode),
    .flush            = e2edec_flush,
    .close            = e2edec_close,
    .p.pix_fmts       = NULL,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS,
};

