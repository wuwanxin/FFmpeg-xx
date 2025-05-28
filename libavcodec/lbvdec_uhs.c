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
#include "h2645_parse.h"
#include "h264.h"
#include "hevc.h"
#include "decode.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bytestream.h"


typedef struct {
    AVClass *class;
    // and other encoder params
    int inited;

    //encoder options
    int base_codec;
	
	int set_blk_w;
	int set_blk_h;
	int num_blk;

    // basedec_ctx
    AVCodecContext *basedec_ctx; 
	enum AVCodecID base_codec_id;
	
	int counter;

} LowBitrateDecoderUHSContext;

// Dump YUV data to file
static void dump_yuv_to_file(AVFrame* frame, const char* filename) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        av_log(NULL, AV_LOG_DEBUG, "Could not open file %s for writing\n", filename);
        return;
    }

    // Write Y component
    fwrite(frame->data[0], 1, frame->linesize[0] * frame->height, file);
    // Write U component
    fwrite(frame->data[1], 1, frame->linesize[1] * (frame->height / 2), file);
    // Write V component
    fwrite(frame->data[2], 1, frame->linesize[2] * (frame->height / 2), file);

    fclose(file);
}

static int __lbvdec_uhs_init_basecodec(AVCodecContext *avctx) {
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    AVCodec *basedec_codec;
    enum AVCodecID base_codec_id = ctx->base_codec_id;
    if(base_codec_id == AV_CODEC_ID_H264){
#ifdef __Xilinx_ZCU106__
        //zcu106 use hw codec by openmax
        basedec_codec = avcodec_find_decoder_by_name("h264_omx");
#else
        #if CONFIG_H264_NI_QUADRA_DECODER_off
        av_log(avctx, AV_LOG_DEBUG,"codec h264_ni_quadra_dec \n");
        basedec_codec = avcodec_find_decoder_by_name("h264_ni_quadra_dec");
        #else
        basedec_codec = avcodec_find_decoder(base_codec_id);
        #endif
        if (!basedec_codec) {
            av_log(avctx, AV_LOG_ERROR,"264 decoder init error \n");
            return AVERROR_UNKNOWN;
        }
#endif
    }else if(base_codec_id == AV_CODEC_ID_H265){
#ifdef __Xilinx_ZCU106__
        //zcu106 use hw codec by openmax
        basedec_codec = NULL;
    
#else
        #if CONFIG_H265_NI_QUADRA_DECODER_off
        av_log(avctx, AV_LOG_DEBUG,"codec h265_ni_quadra_dec \n");
        basedec_codec = avcodec_find_decoder_by_name("h265_ni_quadra_dec");
        #else
        basedec_codec = avcodec_find_decoder(base_codec_id);
        #endif
        if (!basedec_codec) {
            av_log(avctx, AV_LOG_ERROR,"265 decoder init error \n");
            return AVERROR_UNKNOWN;
        }
#endif
    }else{
        av_log(avctx, AV_LOG_ERROR,"codec not support(%d) \n",base_codec_id);
        return AVERROR_UNKNOWN;
    }
    ctx->basedec_ctx = avcodec_alloc_context3(basedec_codec);
    if (!ctx->basedec_ctx) {
        return AVERROR(ENOMEM);
    }
    
    //init baseenc ctx
   
    ctx->basedec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->basedec_ctx->width = ctx->set_blk_w;
    ctx->basedec_ctx->height= ctx->set_blk_h;
    
    if (avcodec_open2(ctx->basedec_ctx, basedec_codec, NULL) < 0) {
        avcodec_free_context(&ctx->basedec_ctx);
        return AVERROR_UNKNOWN;
    }
    return 0;
}

static int __lbvdec_uhs_free_basecodec(LowBitrateDecoderUHSContext *ctx){
    avcodec_free_context(&ctx->basedec_ctx);
    return 0;
}

static int __lbvdec_uhs_init(AVCodecContext *avctx) {
    enum AVCodecID base_codec_id;
    
    int ret;
    av_log(avctx, AV_LOG_DEBUG,"lbvdec_uhs_init enter! \n");
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;

    system("mkdir ./testout");
    
    av_log(avctx, AV_LOG_DEBUG,"yuv file loading...base_codec:%d \n",ctx->base_codec);
    

    //init uncompressed data context
    int _width = avctx->width;
    int _height = avctx->height;
    int _coded_width = avctx->coded_width;
    int _coded_height = avctx->coded_height;
    
    //alloc
    base_codec_id = lbvenc_common_trans_internal_base_codecid_to_codecid(ctx->base_codec);
    if(base_codec_id < 0){
        return -1;
    }
    av_log(avctx, AV_LOG_DEBUG,"base_codec_id %d \n",base_codec_id);
    
    ctx->base_codec_id = base_codec_id;

    ctx->num_blk = 0;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;   

    av_log(avctx, AV_LOG_DEBUG,"lbvdec_uhs_init down! \n");
    return 0;
}

static av_cold int lbvdec_uhs_init(AVCodecContext *avctx) {
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    // init encoders
    ctx->base_codec = 0; 
    return __lbvdec_uhs_init(avctx);
}

static av_cold int hlbvdec_uhs_init(AVCodecContext *avctx) {
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    // init encoders
    ctx->base_codec = 1; 
    return __lbvdec_uhs_init(avctx);
}



static AVFrame* assemble_yuv420p_frames(AVFrame** small_frames, int num_frames, int blk_w, int blk_h, int width, int height,void *logctx) {
    AVFrame* big_frame = av_frame_alloc();
    if (!big_frame) {
        av_log(logctx, AV_LOG_DEBUG, "Could not allocate big frame\n");
        return NULL;
    }

    big_frame->width = width;
    big_frame->height = height;
    big_frame->format = AV_PIX_FMT_YUV420P;
    big_frame->key_frame = 1;

    if (av_frame_get_buffer(big_frame, 32) < 0) {
        av_log(logctx, AV_LOG_DEBUG, "Could not allocate big frame data\n");
        av_frame_free(&big_frame);
        return NULL;
    }

    int num_x_blocks = (width + blk_w - 1) / blk_w; // Ensure rounding up
    int num_y_blocks = (height + blk_h - 1) / blk_h; // Ensure rounding up
    // Check if small frame exists
    if ((num_x_blocks*num_y_blocks) < num_frames) {
        
    }

    // Copy Y data
    
    for (int y = 0; y < num_y_blocks; y++) {
        for(int i = 0;i<blk_h;i++){
            for (int x = 0; x < num_x_blocks; x++) {
                AVFrame* small_frame = small_frames[y * num_x_blocks + x];
                // Copy Y data line
                int line_offset = ((y*blk_h)+i)*(num_x_blocks*small_frame->linesize[0]);
                memcpy(big_frame->data[0]+line_offset+(x*small_frame->linesize[0]),small_frame->data[0]+(i*small_frame->linesize[0]),small_frame->linesize[0]);
        
            }
        }
    }
    // Copy U V data
    for (int y = 0; y < num_y_blocks; y++) {
        for(int i = 0;i<blk_h/2;i++){
            for (int x = 0; x < num_x_blocks; x++) {
                AVFrame* small_frame = small_frames[y * num_x_blocks + x];
                // Copy Y data line
                int line_offset = ((y*blk_h/2)+i)*(num_x_blocks*small_frame->linesize[1]);
                memcpy(big_frame->data[1]+line_offset+(x*small_frame->linesize[1]),small_frame->data[1]+(i*small_frame->linesize[1]),small_frame->linesize[1]);
                memcpy(big_frame->data[2]+line_offset+(x*small_frame->linesize[2]),small_frame->data[2]+(i*small_frame->linesize[2]),small_frame->linesize[2]);
        
            }
        }
    }
    return big_frame;
}

static int add_yuv420p_frame(AVFrame* frame, AVFrame** small_frames, int num_frames, int* current_count,void *logctx) {
    // Check if the maximum number of frames has been reached
    if (*current_count >= num_frames) {
        return 0; // Return 0, indicating failure to add
    }

    // Allocate and copy data to a new small frame
    AVFrame* new_frame = av_frame_alloc();
    if (!new_frame) {
        av_log(logctx, AV_LOG_DEBUG, "Could not allocate frame\n");
        return 0; // Return 0, indicating failure to add
    }

    // Set the properties of the new frame
    new_frame->width = frame->width;
    new_frame->height = frame->height;
    new_frame->format = AV_PIX_FMT_YUV420P;

    // Allocate data buffer
    if (av_frame_get_buffer(new_frame, 32) < 0) {
        av_log(logctx, AV_LOG_DEBUG, "Could not allocate frame data\n");
        av_frame_free(&new_frame);
        return 0; // Return 0, indicating failure to add
    }

    // Copy Y data
    for (int y = 0; y < frame->height; y++) {
        memcpy(new_frame->data[0] + y * new_frame->linesize[0], 
               frame->data[0] + y * frame->linesize[0], 
               frame->width);
    }

    // Copy U data
    for (int y = 0; y < (frame->height + 1) / 2; y++) {
        memcpy(new_frame->data[1] + y * new_frame->linesize[1], 
               frame->data[1] + y * frame->linesize[1], 
               (frame->width + 1) / 2);
    }

    // Copy V data
    for (int y = 0; y < (frame->height + 1) / 2; y++) {
        memcpy(new_frame->data[2] + y * new_frame->linesize[2], 
               frame->data[2] + y * frame->linesize[2], 
               (frame->width + 1) / 2);
    }

    // Add the new frame to the small frames array
    small_frames[*current_count] = new_frame;
    (*current_count)++; // Increment the current frame count

    // Check if the array is full
    return (*current_count == num_frames) ? 1 : 0; // Return 1 if full, otherwise return 0
}

// Function to crop a YUV420P frame
static int crop_yuv420p_frame(AVCodecContext *avctx, AVFrame* frame, AVFrame* cropped_frame, int x, int y, int crop_width, int crop_height) {
    //cropped_frame = av_frame_alloc();
    if (!cropped_frame) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate cropped frame\n");
        return NULL;
    }

    // Set properties for the cropped frame
    cropped_frame->width = crop_width;
    cropped_frame->height = crop_height;
    cropped_frame->format = AV_PIX_FMT_YUV420P;
    cropped_frame->key_frame = frame->key_frame;

    // Allocate buffer for the cropped frame
    if (ff_get_buffer(avctx,cropped_frame, 0) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate frame data for cropped frame\n");
        av_frame_free(&cropped_frame);
        return NULL;
    }

    // Copy Y data
    for (int h = 0; h < crop_height; h++) {
        memcpy(cropped_frame->data[0] + h * cropped_frame->linesize[0],
               frame->data[0] + (h * frame->linesize[0]) ,
               crop_width);
    }

    // Copy U V data
    for (int h = 0; h < (crop_height) / 2; h++) {
        memcpy(cropped_frame->data[1] + (h * cropped_frame->linesize[1]), frame->data[1] + (h * frame->linesize[1]) ,cropped_frame->linesize[1]);
        memcpy(cropped_frame->data[2] + (h * cropped_frame->linesize[2]), frame->data[2] + (h * frame->linesize[2]) ,cropped_frame->linesize[2]);
    }

    return 0;
}

static void __debug_dump_frame(AVFrame *frame, const char *filename) {
    FILE *file = fopen(filename, "wb");
    if (!file) {
        av_log(NULL, AV_LOG_ERROR, "Could not open %s for writing\n", filename);
        return;
    }

    switch (frame->format) {
        case AV_PIX_FMT_YUV420P: {
            // Handle YUV420P format
            // Write Y component
            fwrite(frame->data[0], 1, frame->linesize[0] * frame->height, file);
            // Write U component
            fwrite(frame->data[1], 1, frame->linesize[1] * (frame->height / 2), file);
            // Write V component
            fwrite(frame->data[2], 1, frame->linesize[2] * (frame->height / 2), file);
            break;
        }
        case AV_PIX_FMT_NV12: {
            // Handle NV12 format
            for (int y = 0; y < frame->height; y++) {
                fwrite(frame->data[0] + y * frame->linesize[0], 1, frame->width, file); // Y plane
            }
            for (int y = 0; y < frame->height / 2; y++) {
                fwrite(frame->data[1] + y * frame->linesize[1], 1, frame->width, file); // UV plane
            }
            break;
        }
        case AV_PIX_FMT_RGB24: {
            // Handle RGB24 format
            for (int y = 0; y < frame->height; y++) {
                fwrite(frame->data[0] + y * frame->linesize[0], 1, frame->width * 3, file); // RGB plane
            }
            break;
        }
        // Additional format handling can be implemented as needed
        default:
            av_log(NULL, AV_LOG_ERROR, "Unsupported pixel format: %d\n", frame->format);
            break;
    }

    fclose(file);
}
static int lbvdec_uhs_decode(AVCodecContext *avctx, AVFrame *pict,
    int *got_frame, AVPacket *avpkt) {
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    AVCodecContext *basedec_ctx = NULL;
    int ret ;
	int current_count;
    av_log(avctx, AV_LOG_DEBUG,"lbvdec_uhs_decode enter\n");
    
    *got_frame = 0;
    if(!avpkt->data || (avpkt->size <= 0) ){
        return 0;
    }

    if(!ctx->num_blk){
        if((ctx->set_blk_w==0) || (ctx->set_blk_h==0)){
            LBVC_UHS_DEC_SIDEDATA data = {0};
            if(lbvc_read_dec_block_size_data(avpkt,&data,avctx) < 0){
                return -1;
            }
            ctx->set_blk_w = data.blk_w;
            ctx->set_blk_h = data.blk_h;
            avctx->coded_width = data.coded_w;
            avctx->coded_height = data.coded_h;
        }
        ctx->num_blk = ((avctx->coded_width + ( ctx->set_blk_w - 1 )) / ctx->set_blk_w) * ((avctx->coded_height + ( ctx->set_blk_h - 1 )) / ctx->set_blk_h);
        av_log(avctx, AV_LOG_DEBUG,"yuv file num_blks %d \n",ctx->num_blk);
    }

    ret = __lbvdec_uhs_init_basecodec(avctx);
    if(ret < 0){
        av_log(avctx, AV_LOG_ERROR,"__lbvdec_uhs_init_basecodec error\n");
        return ret;
    }
    
    basedec_ctx = ctx->basedec_ctx;

#if 0
    static FILE *base_bin_fp;
    if(!base_bin_fp) base_bin_fp = fopen("testout/base_str_rx.bin","wb");
    if(base_bin_fp){
        fwrite(avpkt->data, 1, avpkt->size , base_bin_fp);
        fflush(base_bin_fp);
    }
    //av_usleep(10000000);
#endif
    H2645Packet h264_pkts;
    memset(&h264_pkts,0x0,sizeof(H2645Packet));
    ret = ff_h2645_packet_split(&h264_pkts, avpkt->data, avpkt->size, avctx, 0, 0 ,ctx->base_codec_id, 0, 0);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,"Error splitting the input into NAL units.\n");
        return ret;
    }
	
	AVFrame** blks = av_malloc(ctx->num_blk * sizeof(AVFrame*));
    current_count = ctx->counter;
    AVFrame *decoded_frame = av_frame_alloc();
    AVPacket *spkt = NULL;
    int found_counter = 0;
    for (int i = 0; i < (h264_pkts.nb_nals + 1); i++) {
        if(i < h264_pkts.nb_nals){
            H2645NAL *nal = &h264_pkts.nals[i];
            
            if(!spkt){
                spkt = av_packet_alloc();
                av_init_packet(spkt);
                av_new_packet(spkt, MAX_LBVC_UHS_BITRATE/8);
                spkt->size = 0;
            }
            int found_data = 0;
            
            switch (nal->type) {
                case H264_NAL_IDR_SLICE:
                case H264_NAL_SLICE: 
                case HEVC_NAL_IDR_N_LP: 
                case HEVC_NAL_TRAIL_N:
                case HEVC_NAL_IDR_W_RADL:
                //HEVC_NAL_TRAIL_R is the same value with H264_NAL_SLICE
                //case HEVC_NAL_TRAIL_R:
                    found_data = 1;
                    found_counter++;
                default:
                    *(spkt->data + spkt->size) = 0x0;
                    *(spkt->data + spkt->size + 1) = 0x0;
                    *(spkt->data + spkt->size + 2) = 0x0;
                    *(spkt->data + spkt->size + 3) = 0x1;
                    spkt->size += 4;
                    memcpy(spkt->data + spkt->size, nal->raw_data, nal->raw_size);
                    spkt->size += nal->raw_size;
                    break;
            }
            
            if(!found_data){
                continue;
            }
    #if 0
            static FILE *base_bin_fp_spl;
            if(!base_bin_fp_spl) base_bin_fp_spl = fopen("testout/base_str_spl.bin","wb");
            if(base_bin_fp_spl){
                fwrite(spkt->data, 1, spkt->size , base_bin_fp_spl);
                fflush(base_bin_fp_spl);
            }
            //continue;
            //av_usleep(10000000);
    #endif

            ret = avcodec_send_packet(basedec_ctx, spkt);
            if ((ret < 0) && (ret != AVERROR(EAGAIN))) {
                av_log(avctx, AV_LOG_ERROR, "Dec error happened.\n");
                return -1; 
            }
        }else{
            //flush frame
            if(found_counter != ctx->num_blk){
                av_log(avctx,AV_LOG_ERROR," not enough blks has been receieved. \n ");
                return -1;
            }
            av_log(avctx,AV_LOG_DEBUG," base decoder flush all frames \n ");
            ret = avcodec_send_packet(basedec_ctx, NULL);
            if ((ret < 0) && (ret != AVERROR(EAGAIN))) {
                av_log(avctx, AV_LOG_ERROR, "Dec error happened.\n");
                return -1; 
            }
        }

        

        while( avcodec_receive_frame(basedec_ctx, decoded_frame) >= 0){
            // Add a frame and check the return value
#if 0//dump frames
            static int frames = 1;
            char filename[256];
            snprintf(filename, sizeof(filename), "testout/output%d_recon_block_%d.yuv",frames, i);
            __debug_dump_frame(decoded_frame, filename);
            frames++;
#endif
            if (add_yuv420p_frame(decoded_frame, blks, ctx->num_blk, &current_count,avctx)) {
                av_log(avctx, AV_LOG_DEBUG,"Successfully filled the small frame array.\n");
                // Assemble the small frames into a large frame
                AVFrame *decoded_big_pict = assemble_yuv420p_frames(blks, current_count, ctx->set_blk_w, ctx->set_blk_h, 
                                                                    (avctx->coded_width + (ctx->set_blk_w - 1)) / ctx->set_blk_w * ctx->set_blk_w , 
                                                                    (avctx->coded_height + (ctx->set_blk_h - 1)) / ctx->set_blk_h * ctx->set_blk_h,avctx );
                if (decoded_big_pict) {
                    av_log(avctx, AV_LOG_DEBUG,"Successfully assembled the big frame.\n");
                    // Use decoded_big_pict for further processing
                    // Define crop dimensions
#if 0
                    char filename2[256];
                    snprintf(filename2, sizeof(filename2), "testout/decoded_big_pict_%d.yuv",frames);
					__debug_dump_frame(decoded_big_pict,filename2);
#endif
                    int crop_x = decoded_big_pict->width; // X coordinate of the crop start
                    int crop_y = decoded_big_pict->height; // Y coordinate of the crop start
                    int crop_width = avctx->width; // Desired crop width
                    int crop_height = avctx->height; // Desired crop height

                    // Crop the big frame
                    ret = crop_yuv420p_frame(avctx, decoded_big_pict, pict, crop_x, crop_y, crop_width, crop_height);
                    if (ret < 0) {
                        av_log(avctx, AV_LOG_DEBUG,"Successfully cropped the frame to %dx%d.\n", crop_width, crop_height);
                    }
#if 0
                    char filename3[256];
                    snprintf(filename3, sizeof(filename3), "testout/decoded_big_pict_%d_cropped.yuv",frames);
					__debug_dump_frame(pict,filename3);
#endif
                    av_frame_free(&decoded_big_pict); // Don't forget to free it after use

                    if(current_count!=ctx->num_blk){
                        av_log(avctx, AV_LOG_ERROR,"error!!!!!\n");
                        *got_frame = 0;
                    }else{
                        *got_frame = 1;
                    }
                } else {
                    av_log(avctx, AV_LOG_ERROR,"Failed to assemble the big frame.\n");
                }
            } else {
                av_log(avctx, AV_LOG_DEBUG,"Added a frame but not full yet. now get %d blks.\n",current_count);
            }
        }
		av_packet_free(&spkt);
        spkt = NULL;

    }
    if(decoded_frame) av_frame_free(decoded_frame);

	
	// Free small frames
    for (int i = 0; i < current_count; i++) {
        av_frame_free(&blks[i]);
    }
    free(blks);
	__lbvdec_uhs_free_basecodec(ctx);


end:
	
    return 0;
    
}


static av_cold int lbvdec_uhs_close(AVCodecContext *avctx) {
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    // 清理编码器

    return 0;
}

#define OFFSET(x) offsetof(LowBitrateDecoderUHSContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption lbvdec_uhs_options[] = {
    {"blk_w", "set the w of enc blk ", OFFSET(set_blk_w), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 7680, VE, "set_blk_w"},
    {"blk_h", "set the h of enc blk", OFFSET(set_blk_h), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 4320, VE, "set_blk_h"},
    {NULL} // end flag
};

static const AVClass lbvdec_uhs_class = {
    .class_name = "lbvdec_uhs",
    .item_name  = av_default_item_name,
    .option     = lbvdec_uhs_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

FFCodec ff_liblbvc_uhs_decoder = {
    .p.name           = "lbvdec_uhs",
    CODEC_LONG_NAME("libhqbo lbvenc Low Bitrate Video Decoder :: Version-Ultra High Resolution"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_LBVC_UHS,
    .priv_data_size   = sizeof(LowBitrateDecoderUHSContext),
    .init             = lbvdec_uhs_init,
    FF_CODEC_DECODE_CB(lbvdec_uhs_decode),
    .close            = lbvdec_uhs_close,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS,
    .p.priv_class     = &lbvdec_uhs_class,
    .bsfs           = "nuhd_to_normal",
    .p.wrapper_name = "lbvdec_uhs",
};

FFCodec ff_libhlbvc_uhs_decoder = {
    .p.name           = "hlbvdec_uhs",
    CODEC_LONG_NAME("libhqbo lbvenc High Effective Low Bitrate Video Decoder :: Version-Ultra High Resolution"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_HLBVC_UHS,
    .priv_data_size   = sizeof(LowBitrateDecoderUHSContext),
    .init             = hlbvdec_uhs_init,
    FF_CODEC_DECODE_CB(lbvdec_uhs_decode),
    .close            = lbvdec_uhs_close,
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS,
    .p.priv_class     = &lbvdec_uhs_class,
    .bsfs           = "nuhd_to_normal",
    .p.wrapper_name = "hlbvdec_uhs",
};

