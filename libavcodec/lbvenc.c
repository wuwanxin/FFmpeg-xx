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

#include "bytestream.h"

//baseenc vcu
//#define hw_vcu
#define vcu_src_use_shm 0

#ifdef hw_vcu
#include "lib_common/PixMapBuffer.h"
#include "lib_common/BufferStreamMeta.h"
#include "lib_common/BufferPictureMeta.h"
#include "lib_common/StreamBuffer.h"
#include "lib_common/Error.h"
#include "lib_encode/lib_encoder.h"
#include "lib_rtos/lib_rtos.h"
#include "lib_common_enc/RateCtrlMeta.h"
#include "lib_common_enc/IpEncFourCC.h"
#include "lib_common_enc/EncBuffers.h"
#include "lib_ffmpeg_wrapper/vcu_c_warpper.h"

#if vcu_src_use_shm
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHARE_MEM_NAME "/my_shared_memory"
#define SHARE_MEM_SIZE (1920 * 1088 * 3)
static int shm_fd;
static uint8_t *ptr;
static uint8_t smem_tmp_buffer[SHARE_MEM_SIZE] = {0};
#endif
#endif

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

    // baseenc_ctx
    AVCodecContext *baseenc_ctx; 
    AVCodecContext *basedec_ctx; 
    BaseEncoderContext p_base_ctx;
} LowBitrateEncoderContext;

#if vcu_src_use_shm

static void mmap_shared_memory(){
    shm_fd = shm_open(SHARE_MEM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
    }

    ptr = (uint8_t *)mmap(0, SHARE_MEM_SIZE, PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
}

static void close_shared_memory(){
    if (shm_fd == -1) {
        perror("close_shared_memory error");
        exit(1);
    }

    if (munmap(ptr, SHARE_MEM_SIZE) == -1) {
        perror("munmap");
        exit(1);
    }

    if (close(shm_fd) == -1) {
        perror("close");
        exit(1);
    }
}


static void write_to_shared_memory(uint8_t *ori,int size) {

    if (shm_fd == -1) {
        perror("write_to_shared_memory error");
        exit(1);
    }

    memcpy(ptr,ori,size);
    
}

#endif

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

static void install_baseenc_yuv420p_recon(AVFrame *frame,uint8_t *buffer) {
    int y_plane_size = frame->width * frame->height;
    int uv_plane_size = (frame->width / 2) * (frame->height / 2);

    // Copy Y plane data
    memcpy(buffer,frame->data[0], y_plane_size);

    // Copy U plane data
    memcpy(buffer + y_plane_size,frame->data[1], uv_plane_size);

    // Copy V plane data
    memcpy(buffer + y_plane_size + uv_plane_size,frame->data[2], uv_plane_size);

}


static int __base_encode_callback_function(void *basectx, unsigned char *yuv,  unsigned char *recon,int w,int h,unsigned char *str,int *str_len,int framenum){
    if(yuv && recon){
        int ret;
        BaseEncoderContext *p_base_ctx = (BaseEncoderContext *)basectx;
        AVCodecContext *enc_ctx = p_base_ctx->baseenc_ctx; 
        AVCodecContext *dec_ctx = p_base_ctx->basedec_ctx; 
#if 0
#if vcu_src_use_shm
        write_to_shared_memory((uint8_t *)yuv,w * h * 3 / 2 * framenum);
        FILE* fp_src = fopen("./src-shm.yuv", "wb");
        fwrite(ptr, 1, w * h * 3 / 2 , fp_src);           
        fclose(fp_src);
#else
        FILE* fp_src = fopen("./src.yuv", "wb");
        fwrite(yuv, 1, w * h * 3 / 2 * framenum, fp_src);           
        fclose(fp_src);
#endif
        char command[200];
        //sprintf(command, "./baseenc/TAppEncoderStatic -c ./baseenc/encoder_intra_main.cfg -c ./baseenc/sequence.cfg");
        sprintf(command, "bash ./base_encode_test.sh");
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
#else
        AVFrame *frame = create_baseenc_yuv420p_frame(yuv,w,h);
        
        ret = avcodec_send_frame((AVCodecContext*)enc_ctx, frame);
        if (ret < 0) {
            return ret;
        }

        // Create a new AVPacket
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            fprintf(stderr, "Could not allocate AVPacket\n");
            return -1; // Handle allocation error appropriately
        }

        ret = avcodec_receive_packet((AVCodecContext*)enc_ctx, pkt);
        if (ret == 0) {

            // Copy data from pkt to str if pkt has data
            if (pkt->size > 0) {
                memcpy(str, pkt->data, pkt->size); // Copy packet data to str
                *str_len = pkt->size;              // Set str_len to the size of the packet
            }

            pkt->stream_index = 0; // Set the stream index to video

            ret = avcodec_send_packet((AVCodecContext*)dec_ctx, pkt);
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

            //memcpy(recon, decoded_frame->data[0], decoded_frame->linesize[0] * decoded_frame->height); // Y
            //memcpy(recon + decoded_frame->linesize[0] * decoded_frame->height, decoded_frame->data[1], decoded_frame->linesize[1]); // U
            //memcpy(recon + decoded_frame->linesize[0] * decoded_frame->height + decoded_frame->linesize[1], decoded_frame->data[2], decoded_frame->linesize[2]); // V

            // free 
            av_frame_free(&frame);
            av_frame_free(&decoded_frame);
        } else {
             // No data generated
        }

        // Free the packet after use
        av_packet_free(&pkt);
#endif
    }else {
        printf("buffer can not be null \n");
    }
    return 0;
}

static av_cold int lbvc_init(AVCodecContext *avctx) {

    av_log(avctx, AV_LOG_DEBUG,"lbvc_init enter! \n");
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    // 初始化编码器

    ctx->bypass = 0;
    int bypass = ctx->bypass;
    system("mkdir ./testout");
    
            
    av_log(avctx, AV_LOG_DEBUG,"yuv file loading...layers:%d \n",ctx->layers);

    //init uncompressed data context
    int _width = avctx->width;
    int _height = avctx->height;
    int _coded_width = avctx->coded_width;
    int _coded_height = avctx->coded_height;
            
    //alloc
    AVCodec *baseenc_codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!baseenc_codec) {
        return AVERROR_UNKNOWN;
    }

    ctx->baseenc_ctx = avcodec_alloc_context3(baseenc_codec);
    if (!ctx->baseenc_ctx) {
        return AVERROR(ENOMEM);
    }

    AVCodec *basedec_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
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
    if(sevc_encode_init(get_cfg) != SEVC_ERRORCODE_NONE_ERROR){
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_init error \n");
        return -1;
    }

    SET_CALLBACK_DO_BASE_ENC(__base_encode_callback_function);

#if vcu_src_use_shm
    mmap_shared_memory();
#endif
    SEVC_CODECPARAM baseenc_get_param = {0};
    sevc_encode_get_codecparam(&baseenc_get_param);
    
    //init baseenc ctx
    ctx->baseenc_ctx->bit_rate = 400000;
    ctx->baseenc_ctx->width = baseenc_get_param.base_layer_enc_w;
    ctx->baseenc_ctx->height = baseenc_get_param.base_layer_enc_h;
    ctx->baseenc_ctx->time_base = (AVRational){1, 25};
    //ctx->baseenc_ctx->gop_size = 10;
    //ctx->baseenc_ctx->max_b_frames = 1;
    ctx->baseenc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_init avcodec_open2 start. \n");
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "preset", "veryfast", 0); 
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

#if 0//def hw_vcu
    //baseenc
    vcu_ffmpeg_init();

    AL_ELibEncoderArch eArch = AL_LIB_ENCODER_ARCH_HOST;
    if(AL_Lib_Encoder_Init(eArch) != AL_SUCCESS){
        av_log(avctx, AV_LOG_DEBUG,"error AL_Lib_Encoder_Init\n");
        return -1;
    }
#endif
    return 0;
}

static int lbvc_encode(AVCodecContext *avctx, AVPacket *pkt,
                             const AVFrame *frame, int *got_packet) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    AVFrame *tmp;
    int ret = -1;
    PutByteContext pb;
    PutByteContext pb_base;
    SEVC_BASEENC_OUTPUT_FRAME out;
    if(!frame){
        *got_packet = 0;
        return 0;
    }

    int file_size = 0;
    //av_log(avctx, AV_LOG_DEBUG,"new frame lbvc_encode \n");
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

    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame start\n");
    ret = sevc_encode_one_frame();
    av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame down\n");

    if(ret == SEVC_ERRORCODE_NONE_ERROR){
        
        
        
        sevc_encode_new_output_frame(&out);
        sevc_encode_get_frame(&out);

        // FILE* fp_bin = NULL;
        //base
        // fp_bin = fopen("./str.bin", "rb");
        // fseek(fp_bin, 0, SEEK_END);
        // file_size = ftell(fp_bin);
        // rewind(fp_bin);
        // unsigned char *base_tmp_buf = (unsigned char *)malloc(file_size);
        // fread(base_tmp_buf, 1, file_size , fp_bin);
        // fclose(fp_bin);
        // out.base_size = file_size;
    
        ret = av_new_packet(pkt , out.base_size + out.enlayer1_size + out.enlayer2_size  + 1024*100);
        if(ret < 0){
            av_log(avctx, AV_LOG_DEBUG,"av_new_packet error\n");
            return ret;
        }
        av_log(avctx, AV_LOG_DEBUG,"lbvenc packet size:%d \n",pkt->size);
        bytestream2_init_writer(&pb, pkt->data, pkt->size);
        
        //base
        bytestream2_put_be32(&pb, out.base_size);
        bytestream2_put_byte(&pb, 0x00);
        bytestream2_put_buffer(&pb,out.base_buf,out.base_size);

        //enhance size and header
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_new_output_frame: layer1 size-%d layer2 size-%d \n",out.enlayer1_size,out.enlayer2_size);
        bytestream2_put_be32(&pb, out.enlayer1_size+out.enlayer2_size);
        bytestream2_put_byte(&pb, 0x01);
        
        
        //en1
        bytestream2_put_be32(&pb, out.enlayer1_size);
        bytestream2_put_byte(&pb, 0x10);
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_new_output_frame: layer1 position-%dx%d \n",out.enlayer1_roi_x,out.enlayer1_roi_y); //enhance position
        bytestream2_put_be32(&pb, out.enlayer1_roi_x);
        bytestream2_put_be32(&pb, out.enlayer1_roi_y);
        bytestream2_put_buffer(&pb,out.enlayer1_buf,out.enlayer1_size);

        //en2
        bytestream2_put_be32(&pb, out.enlayer2_size);
        bytestream2_put_byte(&pb, 0x11);
        bytestream2_put_buffer(&pb,out.enlayer2_buf,out.enlayer2_size);
        sevc_encode_free_output_frame(&out);
        pkt->size = bytestream2_tell_p(&pb);
        //av_image_copy(pkt->data,pkt->size);
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_get_frame size:%d\n",pkt->size);
        *got_packet = 1;
        //sleep(1000);
    }else if(ret == SEVC_ERRORCODE_RECON_WAIT){
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame wait.... \n");
        *got_packet = 0;
        usleep(1000);
    }else{
        av_log(avctx, AV_LOG_DEBUG,"sevc_encode_one_frame error.(%d) \n",ret);
        if(tmp) av_frame_free(&tmp);
        return ret;
    }

    if(tmp) av_frame_free(&tmp);
    return 0;
}

static void lbvc_flush(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_DEBUG,"lbvc_flush enter! \n");
}

static av_cold int lbvc_close(AVCodecContext *avctx) {
    LowBitrateEncoderContext *ctx = avctx->priv_data;
    // 清理编码器

#if vcu_src_use_shm
    close_shared_memory();
#endif
    return 0;
}

#define OFFSET(x) offsetof(LowBitrateEncoderContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption lbvc_options[] = {
    {"layers", "set the number of enc layers", OFFSET(layers), AV_OPT_TYPE_INT, {.i64 = 2}, 0, 2, VE, "layers"},
    { "1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0,  VE, "layers" },
    { "2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0,  VE, "layers" },
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
