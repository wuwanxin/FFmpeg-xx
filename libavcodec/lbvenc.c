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

static int __base_encode_callback_function(unsigned char *yuv,  unsigned char *recon,int w,int h,unsigned char *str,int *str_len){
    printf("enter __base_encode_callback_function %dx%d  \n",w,h);
    if(yuv && recon){
        char command[200];
        sprintf(command, "./baseenc/TAppEncoderStatic -c ./baseenc/encoder_intra_main.cfg -c ./baseenc/sequence.cfg");
        system(command);
        

        
        
        FILE* fp_recon = fopen("./rec.yuv", "rb");
        fread(recon, 1, w * h * 3 / 2 , fp_recon);
        fclose(fp_recon);

        FILE* fp_bin = fopen("./str.bin", "rb");
        fseek(fp_bin, 0, SEEK_END);
        int file_size = ftell(fp_bin);
        rewind(fp_bin);
        fread(str, 1, file_size , fp_bin);
        fclose(fp_bin);
        *str_len = file_size;
        
    }else {
        printf("buffer can not be null \n");
    }
    return 0;
}

static av_cold int lbvc_init(AVCodecContext *avctx) {

    printf("lbvc_init enter! \n");
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    // 初始化编码器

    ctx->bypass = 0;
    int bypass = ctx->bypass;
    system("mkdir ./testout");
    
            
    printf("yuv file loading... \n");
    //init uncompressed data context
    int _width = avctx->width;
    int _height = avctx->height;
    int _coded_width = avctx->coded_width;
    int _coded_height = avctx->coded_height;
            
    //open file

    //init sevc 
            
    sevc_encode_init(_coded_width,_coded_height);

    SET_CALLBACK_DO_BASE_ENC(__base_encode_callback_function);
    
    return 0;
}

static int lbvc_encode(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    AVFrame *tmp;
    int ret = -1;
    if(!frame){
        *got_packet = 0;
        return 0;
    }

    tmp = av_frame_alloc();
    if(!tmp){
        printf("av_frame_alloc error. \n");
        return ret;
    }
    ret = av_frame_ref(tmp,frame);
    if (ret < 0) {
        printf("av_frame_ref error. \n");
        return ret;
    }
    ret = av_frame_copy(tmp,frame);
    if (ret < 0) {
        printf("av_frame_copy error. \n");
        return ret;
    }

    printf("==============>lbvc_encode<============== \n");
    printf("width :%d \n",tmp->width);
    printf("height:%d \n",tmp->height);
    for(int i = 0;i<AV_NUM_DATA_POINTERS;i++){
        if(tmp->data[i]){
            if(i==0)
                printf("stride(linsize)-LUMA          :%d \n",tmp->linesize[i]);
            else
                printf("stride(linsize)-CHROMA(U/V/UV):%d \n",tmp->linesize[i]);
        }
    }
    printf("========================================= \n");

    static FILE *fp;
    switch(frame->format){
        case AV_PIX_FMT_YUV420P:
#if 0
            if(!fp) fp = fopen("testout/ffmpeg_yuv_in.yuv","wb");
            if(fp){
                fwrite(tmp->data[0], 1, avctx->coded_width * avctx->coded_height  , fp);
                fwrite(tmp->data[1], 1, avctx->coded_width * avctx->coded_height / 4 , fp);
                fwrite(tmp->data[2], 1, avctx->coded_width * avctx->coded_height / 4 , fp);
                fflush(fp);
            }
#endif
            sevc_encode_push_one_frame(tmp->data[0],tmp->data[1],tmp->data[2]);
            break;
    }
    printf("sevc_encode_one_frame start\n");
    ret = sevc_encode_one_frame();
    printf("sevc_encode_one_frame down\n");
    if(ret == SEVC_ERRORCODE_NONE_ERROR){
        int frame_size = 1024*1024*300;
        ret = av_new_packet(pkt , frame_size);
        if(ret < 0){
            return ret;
        }
        sevc_encode_get_frame(pkt->data,&pkt->size);
        //av_image_copy(pkt->data,pkt->size);
        printf("sevc_encode_get_frame size:%d\n",pkt->size);
        *got_packet = 1;
    }else if(ret == SEVC_ERRORCODE_RECON_WAIT){
        printf("sevc_encode_one_frame wait.... \n");
        *got_packet = 0;
        usleep(1000);
    }else{
        printf("sevc_encode_one_frame error.(%d) \n",ret);
        av_frame_free(&tmp);
        return ret;
    }
    
    if(tmp) av_frame_free(&tmp);
    return 0;
}

static void lbvc_flush(AVCodecContext *avctx)
{
    printf("lbvc_flush enter! \n");
}

static av_cold int lbvc_close(AVCodecContext *avctx) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    // 清理编码器
    return 0;
}

static const AVClass lbvc_class = {
    .class_name = "lbvc",
    .item_name  = av_default_item_name,
    .option     = NULL,
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
