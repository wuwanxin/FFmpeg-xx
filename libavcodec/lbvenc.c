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
#include "lbvenc.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mediaass/sevc_enc.h>

#include "bytestream.h"

typedef struct {
    AVCodecContext *baseenc_ctx; 
    AVCodecContext *basedec_ctx; 
}BaseEncoderContext;

typedef struct {
    AVClass *class;
    // and other encoder params
    int inited;
    int bypass;

    //encoder options
    int layers;
    int base_codec;

    // baseenc_ctx
    AVCodecContext *baseenc_ctx; 
    AVCodecContext *basedec_ctx; 
    BaseEncoderContext p_base_ctx;
} LowBitrateEncoderContext;


static AVFrame* create_baseenc_yuv420p_frame(uint8_t *buffer, int width, int height) {
    // Allocate an AVFrame instance
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate memory for AVFrame\n");
        return NULL;
    }

    // Set parameters for the AVFrame
    frame->format = AV_PIX_FMT_YUV420P; // YUV 420P format
    frame->width = width;
    frame->height = height;

    // Allocate buffer for the image data
    int ret = av_frame_get_buffer(frame, 1); // 1 is the alignment requirement; can be adjusted as needed
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data\n");
        av_frame_free(&frame);
        return NULL;
    }

    // Now copy the data from buffer into the AVFrame's data
    // Assuming the data in buffer is in the order of Y, U, V (YUV 420P format)
    int y_plane_size = width * height;
    int uv_plane_size = (width / 2) * (height / 2);

    // Copy Y plane data
    memcpy(frame->data[0], buffer, y_plane_size);

    // Copy U plane data
    memcpy(frame->data[1], buffer + y_plane_size, uv_plane_size);

    // Copy V plane data
    memcpy(frame->data[2], buffer + y_plane_size + uv_plane_size, uv_plane_size);

    // Set timestamp information (optional, depending on use case)
    frame->pts = 0;  // Set the presentation timestamp (PTS) for the frame

    return frame;
}

static AVFrame* create_baseenc_nv12_frame(uint8_t *buffer, int width, int height) {
    // Allocate an AVFrame instance
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate memory for AVFrame\n");
        return NULL;
    }

    // Set parameters for the AVFrame
    frame->format = AV_PIX_FMT_NV12; // NV12 format
    frame->width = width;
    frame->height = height;

    // Allocate buffer for the image data
    int ret = av_frame_get_buffer(frame, 1); // 1 is the alignment requirement; can be adjusted as needed
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data\n");
        av_frame_free(&frame);
        return NULL;
    }

    // Assuming the data in buffer is in the order of Y, U, V (YUV 420P format)
    int y_plane_size = width * height;
    int uv_plane_size = (width / 2) * (height / 2);

    // Copy Y plane data
    memcpy(frame->data[0], buffer, y_plane_size);

    // Prepare UV plane data in NV12 format
    uint8_t *uv_plane = frame->data[1];
    for (int h = 0; h < height / 2; h++) {
        for (int w = 0; w < width / 2; w++) {
            // U and V values are interleaved in NV12 format
            uv_plane[2 * (h * (width / 2) + w)] = buffer[y_plane_size + (h * (width / 2) + w)];        // U
            uv_plane[2 * (h * (width / 2) + w) + 1] = buffer[y_plane_size + uv_plane_size + (h * (width / 2) + w)]; // V
        }
    }

    // Set timestamp information (optional, depending on use case)
    frame->pts = 0;  // Set the presentation timestamp (PTS) for the frame

    return frame;
}

static void install_baseenc_yuv420p_recon(AVFrame *frame,uint8_t *buffer) {
    int y_width = frame->width;
    int y_height = frame->height;
    int y_size = y_width * y_height;
    
    int uv_width = y_width / 2;
    int uv_height = y_height / 2;
    int uv_size = uv_width * uv_height;

    uint8_t *dst = buffer;

    // Copy Y plane data
	for (int i = 0; i < y_height; i++, dst += y_width) {
        memcpy(dst, frame->data[0] + i * frame->linesize[0], y_width);
    }

    // Copy U plane data
	uint8_t *u_src = frame->data[1];
    for (int i = 0; i < uv_height; i++, dst += uv_width) {
        memcpy(dst, u_src + i * frame->linesize[1], uv_width);
    }
    // Copy V plane data
    uint8_t *v_src = frame->data[2];
    for (int i = 0; i < uv_height; i++, dst += uv_width) {
        memcpy(dst, v_src + i * frame->linesize[2], uv_width);
    }

}


static int __base_encode_callback_function(void *basectx, unsigned char *yuv,  unsigned char *recon,int w,int h,unsigned char *str,int *str_len,int flag){

    int ret = -1;
    BaseEncoderContext *p_base_ctx = (BaseEncoderContext *)basectx;
    AVCodecContext *enc_ctx = p_base_ctx->baseenc_ctx; 
    AVCodecContext *dec_ctx = p_base_ctx->basedec_ctx; 

    AVFrame *frame;
    

    if(flag > 3){
        fprintf(stderr, "not support enc flag(%d) > 3 , please check the version of sevc. \n",flag);
        return -1; // Handle allocation error appropriately
    }
    
    if(((flag==0)) && yuv ){
        //only send frame
#ifdef __Xilinx_ZCU106__
        frame = create_baseenc_nv12_frame(yuv,w,h);
#else
        frame = create_baseenc_yuv420p_frame(yuv,w,h);
#endif
        
        ret = avcodec_send_frame((AVCodecContext*)enc_ctx, frame);
        if (ret < 0) {
            return ret;
        }
        printf( "baseenc send frame down \n");

        av_frame_free(&frame);
    }

    if((flag >= 1) && recon){
        //printf( "baseenc try to get recon and str.. \n");
        //only receive frame
        // Create a new AVPacket

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            fprintf(stderr, "Could not allocate AVPacket\n");
            return -1; // Handle allocation error appropriately
        }

        ret = avcodec_receive_packet((AVCodecContext*)enc_ctx, pkt);
        if (ret == 0) {
            // Copy data from pkt to str if pkt has data
            if ((pkt->size > 0) && (pkt->data)) {
                //printf("!data generated! (size %d)\n",pkt->size);
                memcpy(str, pkt->data, pkt->size); // Copy packet data to str
                *str_len = pkt->size;              // Set str_len to the size of the packet
#if 0
                static FILE *base_bin_fp;
                if(!base_bin_fp) base_bin_fp = fopen("testout/base_str.bin","wb");
                if(base_bin_fp){
                    fwrite(pkt->data, 1, pkt->size , base_bin_fp);
                    fflush(base_bin_fp);
                }
                //sleep(10);
#endif
                pkt->stream_index = 0; // Set the stream index to video

                if(dec_ctx){
                    ret = avcodec_send_packet((AVCodecContext*)dec_ctx, pkt);
                }else{
                    fprintf(stderr, "dec_ctx error happened.\n");
                    return -1; 
                }
                
                if (ret < 0) {
                    fprintf(stderr, "Dec error happened.\n");
                    return -1; 
                }

                AVFrame *decoded_frame = av_frame_alloc();
                ret = avcodec_receive_frame((AVCodecContext*)dec_ctx, decoded_frame);
                if (ret < 0) {
                    fprintf(stderr, "Dec receive frame error happened.\n");
                    return -1; 
                }
                
                install_baseenc_yuv420p_recon(decoded_frame,recon);
        
#if 0
                static FILE *base_recon_fp;
                if(!base_recon_fp) base_recon_fp = fopen("testout/base_recon.yuv","wb");
                if(base_recon_fp){
                    fwrite(recon, 1, decoded_frame->height*decoded_frame->linesize[0]*3/2 , base_recon_fp);
                    fflush(base_recon_fp);
                }
#endif
                av_frame_free(&decoded_frame);
                                
                printf("Dec receive frame down.\n");
            
            } else {
                fprintf(stderr, "No data generated.\n");
            }
            // Free the packet after use
            av_packet_free(&pkt);
        }else{
            printf( "baseenc wait for pkt data return. \n");
            return -1;
        }
    
    }
    return 0;
}

static av_cold int __lbvc_init(AVCodecContext *avctx) {
    enum AVCodecID base_codec_id;
    AVCodec *baseenc_codec;

    av_log(avctx, AV_LOG_DEBUG,"__lbvc_init enter! \n");
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    // 初始化编码器

    ctx->bypass = 0;
    int bypass = ctx->bypass;
    system("mkdir ./testout");
    
            
    av_log(avctx, AV_LOG_DEBUG,"yuv file loading...layers:%d \n",ctx->layers);
    av_log(avctx, AV_LOG_DEBUG,"yuv file loading...base_codec:%d \n",ctx->base_codec);
    

    //init uncompressed data context
    int _width = avctx->width;
    int _height = avctx->height;
    int _coded_width = avctx->coded_width;
    int _coded_height = avctx->coded_height;

            
    //alloc
    base_codec_id = lbvenc_common_trans_internal_base_codecid_to_codecid(ctx->base_codec);

#ifdef __Xilinx_ZCU106__
    //zcu106 use hw codec by openmax
    if(base_codec_id == AV_CODEC_ID_H264){
        baseenc_codec = avcodec_find_encoder_by_name("h264_omx");
    }else{
        av_log(avctx, AV_LOG_ERROR,"codec not support(%d) \n",base_codec_id);
        return AVERROR_UNKNOWN;
    }
#else
    baseenc_codec = avcodec_find_encoder(base_codec_id);
    if (!baseenc_codec) {
        return AVERROR_UNKNOWN;
    }
#endif
    ctx->baseenc_ctx = avcodec_alloc_context3(baseenc_codec);
    if (!ctx->baseenc_ctx) {
        return AVERROR(ENOMEM);
    }

    AVCodec *basedec_codec = avcodec_find_decoder(base_codec_id);
    ctx->basedec_ctx = avcodec_alloc_context3(basedec_codec);
    if (!ctx->basedec_ctx) {
        return AVERROR(ENOMEM);
    }
    
    ctx->p_base_ctx.baseenc_ctx = ctx->baseenc_ctx;
    ctx->p_base_ctx.basedec_ctx = ctx->basedec_ctx;

    //init sevc 
    SEVC_CONFIGURE get_cfg = {
        .width = _coded_width,
        .height = _coded_height,
        .layer_enc = ctx->layers,
        .base_ctx = (void *)&ctx->p_base_ctx,
    };

    av_log(avctx, AV_LOG_DEBUG,"__lbvc_init sevc_encode_init ! \n");
    
    SET_CALLBACK_DO_BASE_ENC(__base_encode_callback_function);
    av_log(avctx, AV_LOG_DEBUG,"__base_encode_callback_function sevc callback init down! 0x%08x\n",__base_encode_callback_function);

    if(sevc_encode_init(get_cfg) != SEVC_ERRORCODE_NONE_ERROR){
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_init error \n");
        return -1;
    }
    av_log(avctx, AV_LOG_DEBUG,"__lbvc_init sevc encode init down! \n");


    SEVC_CODECPARAM baseenc_get_param = {0};
    sevc_encode_get_codecparam(&baseenc_get_param);
    
    //init baseenc ctx
    ctx->baseenc_ctx->bit_rate = 400000;
    ctx->baseenc_ctx->width = baseenc_get_param.base_layer_enc_w;
    ctx->baseenc_ctx->height = baseenc_get_param.base_layer_enc_h;
    ctx->baseenc_ctx->time_base = (AVRational){1, 25};
    ctx->baseenc_ctx->gop_size = 25;
    ctx->baseenc_ctx->keyint_min = 25;
    ctx->baseenc_ctx->slice_count = 1;
    ctx->baseenc_ctx->refs = 1;
    ctx->baseenc_ctx->has_b_frames = 0;
    ctx->baseenc_ctx->max_b_frames = 0;
    ctx->baseenc_ctx->thread_count = 1;
#ifdef __Xilinx_ZCU106__
    ctx->baseenc_ctx->pix_fmt = AV_PIX_FMT_NV12;
#else
    ctx->baseenc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
#endif
    av_opt_set(ctx->baseenc_ctx->priv_data,"slice_mode","1",0);

    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_init avcodec_open2 start. \n");
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "preset", "fast", 0); 
    av_dict_set(&opts, "tune", "zerolatency", 0); 

    if (avcodec_open2(ctx->baseenc_ctx, baseenc_codec, &opts) < 0) {
        avcodec_free_context(&ctx->baseenc_ctx);
        return AVERROR_UNKNOWN;
    }
    av_dict_free(&opts);
    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_init avcodec_open2 down. \n");

    //init basedec ctx
    ctx->basedec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (avcodec_open2(ctx->basedec_ctx, basedec_codec, NULL) < 0) {
        avcodec_free_context(&ctx->basedec_ctx);
        return AVERROR_UNKNOWN;
    }

    return 0;
}

static av_cold int lbvc_init(AVCodecContext *avctx){
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    ctx->base_codec = 0;
    return __lbvc_init(avctx);
}

static av_cold int lbvc_hevc_init(AVCodecContext *avctx){
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    ctx->base_codec = 1;
    return __lbvc_init(avctx);
}

static av_cold int hlbvc_init(AVCodecContext *avctx){
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    ctx->base_codec = 2;
    return __lbvc_init(avctx);
}

static int lbvc_encode(AVCodecContext *avctx, AVPacket *pkt,
    const AVFrame *frame, int *got_packet) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    AVFrame *tmp;
    int ret = -1;
    SEVC_ERRORCODE src_push_retcode; 

    int file_size = 0;


    if(!frame){
        *got_packet = 0;
        return 0;
    }
once:
    file_size = 0;

    tmp = av_frame_alloc();
    if(!tmp){
        av_log(avctx, AV_LOG_DEBUG,"av_frame_alloc error. \n");
        return ret;
    }
    ret = av_frame_ref(tmp,frame);
    if (ret < 0) {
        av_log(avctx, AV_LOG_DEBUG,"av_frame_ref error. \n");
        return ret;
    }
    ret = av_frame_copy(tmp,frame);
    if (ret < 0) {
        av_log(avctx, AV_LOG_DEBUG,"av_frame_copy error. \n");
        return ret;
    }

    av_log(avctx, AV_LOG_DEBUG,"==============>lbvc_encode<============== \n");
    av_log(avctx, AV_LOG_DEBUG,"width :%d \n",tmp->width);
    av_log(avctx, AV_LOG_DEBUG,"height:%d \n",tmp->height);
    for(int i = 0;i<AV_NUM_DATA_POINTERS;i++){
        if(tmp->data[i]){
            if(i==0)
                av_log(avctx, AV_LOG_DEBUG,"stride(linsize)-LUMA          :%d \n",tmp->linesize[i]);
            else
                av_log(avctx, AV_LOG_DEBUG,"stride(linsize)-CHROMA(U/V/UV):%d \n",tmp->linesize[i]);
        }
    }

    av_log(avctx, AV_LOG_DEBUG,"========================================= \n");

    switch(frame->format){
        case AV_PIX_FMT_YUV420P:
            src_push_retcode = sevc_encode_push_one_yuv420p_frame(tmp->data[0],tmp->linesize[0]*tmp->height,
                                                                    tmp->data[1],tmp->linesize[1]*tmp->height/2,
                                                                    tmp->data[2],tmp->linesize[2]*tmp->height/2);
            if(src_push_retcode == SEVC_ERRORCODE_INPUT_ERROR){
                av_log(avctx, AV_LOG_ERROR,"sevc_encode_push_one_yuv420p_frame error.\n");
                goto err;
            }
            break;
        default:
            av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame_and_get_result not support yuv format .(%d) \n",frame->format);
            goto err;
    }
    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_push_one_yuv420p_frame retcode .(%d) \n",src_push_retcode);
sevc_encode_once:
    //malloc pkt
    ret = av_new_packet(pkt , 1920 * 1080);
    if(ret < 0){
        av_log(avctx, AV_LOG_DEBUG,"av_new_packet error\n");
        return ret;
    }
    av_log(avctx, AV_LOG_DEBUG,"lbvenc packet size:%d \n",pkt->size);
    //enc
    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame_and_get_result start\n");
    ret = sevc_encode_one_frame_and_get_result(pkt->data,&pkt->size);
    switch(ret){
        case SEVC_ERRORCODE_ENCODE_ERROR:
            goto err;
        case SEVC_ERRORCODE_RECON_WAIT:
            *got_packet = 0;
            break;
        case SEVC_ERRORCODE_NONE_ERROR:
            *got_packet = 1;
            break;
    }
    
    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame_and_get_result down (%d) size (%d)\n",ret,pkt->size);

sevc_encode_down:
    if((src_push_retcode == SEVC_ERRORCODE_BASEENC_SRC_SEND_WAIT)){
        av_usleep(1000);
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame_and_get_result SEVC_ERRORCODE_BASEENC_SRC_SEND_WAIT goto once.(%d) \n",ret);
        goto once;
    }
    
    if(tmp) av_frame_free(&tmp);
    //if(pkt) av_packet_free(&pkt);
    return 0;

got_no_data:
    av_log(avctx, AV_LOG_ERROR,"lbvc_encode got no data\n");
    if(tmp) av_frame_free(&tmp);
    //if(pkt) av_packet_free(&pkt);
    *got_packet = 0;
    return 0;
err:    
    av_log(avctx, AV_LOG_ERROR,"lbvc_encode error happened\n");
    if(tmp) av_frame_free(&tmp);
    //if(pkt) av_packet_free(&pkt);
    return -1;
    
}

static void lbvc_flush(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_DEBUG,"lbvc_flush enter! \n");
}

static av_cold int lbvc_close(AVCodecContext *avctx) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    // 清理编码器

    return 0;
}

#define OFFSET(x) offsetof(LowBitrateEncoderContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption lbvc_options[] = {
    {"layers", "set the number of enc layers", OFFSET(layers), AV_OPT_TYPE_INT, {.i64 = 2}, 0, 2, VE, "layers"},
    { "1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0,  VE, "layers" },
    { "2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0,  VE, "layers" },
#if 0
    {"base_codec", "set the number of base codec", OFFSET(base_codec), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 2, VE, "base_codec"},
    { "0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0,  VE, "base_codec" },
    { "1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0,  VE, "base_codec" },
    { "2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0,  VE, "base_codec" },
#endif
    {NULL} // end flag
};

static const AVClass lbvc_class = {
    .class_name = "lbvc",
    .item_name  = av_default_item_name,
    .option     = lbvc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault lbvc_defaults[] = {
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

FFCodec ff_lbvc_encoder = {
    .p.name           = "lbvenc",
    CODEC_LONG_NAME("libhqbo lbvenc Low Bitrate Video Encoder"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_LBVC,
    .p.capabilities   = AV_CODEC_CAP_DR1 ,
    .p.priv_class     = &lbvc_class,
    .p.wrapper_name   = "lbvenc",
    .priv_data_size   = sizeof(LowBitrateEncoderContext),
    .init             = lbvc_init,
    FF_CODEC_ENCODE_CB(lbvc_encode),
    .flush            = lbvc_flush,
    .close            = lbvc_close,
    .defaults         = lbvc_defaults,
    .p.pix_fmts       = pix_fmts_all,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS
                      ,
};

FFCodec ff_lbvc_hevc_encoder = {
    .p.name           = "lbvenc_hevc",
    CODEC_LONG_NAME("libhqbo lbvenc Low Bitrate Video Encoder"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_LBVC_HEVC,
    .p.capabilities   = AV_CODEC_CAP_DR1 ,
    .p.priv_class     = &lbvc_class,
    .p.wrapper_name   = "lbvenc_hevc",
    .priv_data_size   = sizeof(LowBitrateEncoderContext),
    .init             = lbvc_hevc_init,
    FF_CODEC_ENCODE_CB(lbvc_encode),
    .flush            = lbvc_flush,
    .close            = lbvc_close,
    .defaults         = lbvc_defaults,
    .p.pix_fmts       = pix_fmts_all,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS
                      ,
};

FFCodec ff_hlbvc_encoder = {
    .p.name           = "hlbvenc",
    CODEC_LONG_NAME("libhqbo lbvenc Low Bitrate Video Encoder"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_HLBVC,
    .p.capabilities   = AV_CODEC_CAP_DR1 ,
    .p.priv_class     = &lbvc_class,
    .p.wrapper_name   = "hlbvenc",
    .priv_data_size   = sizeof(LowBitrateEncoderContext),
    .init             = hlbvc_init,
    FF_CODEC_ENCODE_CB(lbvc_encode),
    .flush            = lbvc_flush,
    .close            = lbvc_close,
    .defaults         = lbvc_defaults,
    .p.pix_fmts       = pix_fmts_all,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS
                      ,
};