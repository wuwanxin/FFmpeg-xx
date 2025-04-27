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

    // baseenc_ctx
    AVCodecContext *baseenc_ctx; 
	
	
	int counter;

} LowBitrateDecoderUHSContext;

// Dump YUV data to file
static void dump_yuv_to_file(AVFrame* frame, const char* filename) {
    FILE* file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Could not open file %s for writing\n", filename);
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

static av_cold int lbvdec_uhs_init(AVCodecContext *avctx) {
    enum AVCodecID base_codec_id;
    AVCodec *basedec_codec;

    av_log(avctx, AV_LOG_DEBUG,"lbvdec_uhs_init enter! \n");
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    // init encoders
    ctx->base_codec = 0; //defalut, now only support 264
    ctx->set_blk_w = 1920;
    ctx->set_blk_h = 1088;
    system("mkdir ./testout");
    
    av_log(avctx, AV_LOG_DEBUG,"yuv file loading...base_codec:%d \n",ctx->base_codec);
    

    //init uncompressed data context
    int _width = avctx->width;
    int _height = avctx->height;
    int _coded_width = avctx->coded_width;
    int _coded_height = avctx->coded_height;
	av_log(avctx, AV_LOG_DEBUG,"yuv file _widthx_height:%dx%d blk _widthx_height:%dx%d \n",_width,_height,ctx->set_blk_w,ctx->set_blk_h);
    if((ctx->set_blk_w==0) || (ctx->set_blk_h==0)){
        return -1;
    }
	ctx->num_blk = ((_width + ( ctx->set_blk_w - 1 )) / ctx->set_blk_w) * ((_height + ( ctx->set_blk_h - 1 )) / ctx->set_blk_h);
    av_log(avctx, AV_LOG_DEBUG,"yuv file num_blks %d \n",ctx->num_blk);
    
    //alloc
    base_codec_id = lbvenc_common_trans_internal_base_codecid_to_codecid(ctx->base_codec);
    if(base_codec_id < 0){
        return -1;
    }
    av_log(avctx, AV_LOG_DEBUG,"base_codec_id %d \n",base_codec_id);
    
#ifdef __Xilinx_ZCU106__
    //zcu106 use hw codec by openmax
    if(base_codec_id == AV_CODEC_ID_H264){
        basedec_codec = avcodec_find_encoder_by_name("h264_omx");
    }else{
        av_log(avctx, AV_LOG_ERROR,"codec not support(%d) \n",base_codec_id);
        return AVERROR_UNKNOWN;
    }
#else
    basedec_codec = avcodec_find_decoder(base_codec_id);
    if (!basedec_codec) {
        return AVERROR_UNKNOWN;
    }
#endif
    ctx->baseenc_ctx = avcodec_alloc_context3(basedec_codec);
    if (!ctx->baseenc_ctx) {
        return AVERROR(ENOMEM);
    }
    
    //init baseenc ctx
   
    ctx->baseenc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    
    if (avcodec_open2(ctx->baseenc_ctx, basedec_codec, NULL) < 0) {
        avcodec_free_context(&basedec_codec);
        return AVERROR_UNKNOWN;
    }

    av_log(avctx, AV_LOG_DEBUG,"lbvdec_uhs_init down! \n");
    return 0;
}



static AVFrame* assemble_yuv420p_frames(AVFrame** small_frames, int num_frames, int blk_w, int blk_h, int width, int height) {
    AVFrame* big_frame = av_frame_alloc();
    if (!big_frame) {
        fprintf(stderr, "Could not allocate big frame\n");
        return NULL;
    }

    big_frame->width = width;
    big_frame->height = height;
    big_frame->format = AV_PIX_FMT_YUV420P;

    if (av_frame_get_buffer(big_frame, 32) < 0) {
        fprintf(stderr, "Could not allocate big frame data\n");
        av_frame_free(&big_frame);
        return NULL;
    }

    int num_x_blocks = (width + blk_w - 1) / blk_w; // Ensure rounding up
    int num_y_blocks = (height + blk_h - 1) / blk_h; // Ensure rounding up

    for (int y = 0; y < num_y_blocks; y++) {
        for (int x = 0; x < num_x_blocks; x++) {
            // Calculate the index of the small frame
            int small_frame_index = y * num_x_blocks + x;

            // Check if small frame exists
            if (small_frame_index < num_frames) {
                AVFrame* small_frame = small_frames[small_frame_index];

                // Copy Y data
                for (int j = 0; j < blk_h; j++) {
                    for (int i = 0; i < blk_w; i++) {
                        int dest_x = x * blk_w + i;
                        int dest_y = y * blk_h + j;
                        if (dest_x < width && dest_y < height) {
                            // Check bounds and copy Y
                            big_frame->data[0][dest_y * big_frame->linesize[0] + dest_x] =
                                small_frame->data[0][j * small_frame->linesize[0] + i];
                        }
                    }
                }

                // Copy U and V data (each has half the vertical and horizontal resolution)
                for (int j = 0; j < (blk_h + 1) / 2; j++) {
                    for (int i = 0; i < (blk_w + 1) / 2; i++) {
                        int dest_x = x * (blk_w / 2) + i; // U/V blocks are half the width
                        int dest_y = y * (blk_h / 2) + j;
                        if (dest_x < (width / 2) && dest_y < (height / 2)) {
                            // Check bounds and copy U
                            big_frame->data[1][dest_y * big_frame->linesize[1] + dest_x] =
                                small_frame->data[1][j * small_frame->linesize[1] + i];

                            // Check bounds and copy V
                            big_frame->data[2][dest_y * big_frame->linesize[2] + dest_x] =
                                small_frame->data[2][j * small_frame->linesize[2] + i];
                        }
                    }
                }
            }
        }
    }

    return big_frame;
}

static int add_yuv420p_frame(AVFrame* frame, AVFrame** small_frames, int num_frames, int* current_count) {
    // Check if the maximum number of frames has been reached
    if (*current_count >= num_frames) {
        return 0; // Return 0, indicating failure to add
    }

    // Allocate and copy data to a new small frame
    AVFrame* new_frame = av_frame_alloc();
    if (!new_frame) {
        fprintf(stderr, "Could not allocate frame\n");
        return 0; // Return 0, indicating failure to add
    }

    // Set the properties of the new frame
    new_frame->width = frame->width;
    new_frame->height = frame->height;
    new_frame->format = AV_PIX_FMT_YUV420P;

    // Allocate data buffer
    if (av_frame_get_buffer(new_frame, 32) < 0) {
        fprintf(stderr, "Could not allocate frame data\n");
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
static AVFrame* crop_yuv420p_frame(AVFrame* frame, int x, int y, int crop_width, int crop_height) {
    AVFrame* cropped_frame = av_frame_alloc();
    if (!cropped_frame) {
        fprintf(stderr, "Could not allocate cropped frame\n");
        return NULL;
    }

    // Set properties for the cropped frame
    cropped_frame->width = crop_width;
    cropped_frame->height = crop_height;
    cropped_frame->format = AV_PIX_FMT_YUV420P;

    // Allocate buffer for the cropped frame
    if (av_frame_get_buffer(cropped_frame, 32) < 0) {
        fprintf(stderr, "Could not allocate frame data for cropped frame\n");
        av_frame_free(&cropped_frame);
        return NULL;
    }

    // Copy Y data
    for (int h = 0; h < crop_height; h++) {
        memcpy(cropped_frame->data[0] + h * cropped_frame->linesize[0],
               frame->data[0] + (y + h) * frame->linesize[0] + x,
               crop_width);
    }

    // Copy U data
    for (int h = 0; h < (crop_height + 1) / 2; h++) {
        memcpy(cropped_frame->data[1] + h * cropped_frame->linesize[1],
               frame->data[1] + ((y + h) / 2) * frame->linesize[1] + (x / 2),
               (crop_width + 1) / 2);
    }

    // Copy V data
    for (int h = 0; h < (crop_height + 1) / 2; h++) {
        memcpy(cropped_frame->data[2] + h * cropped_frame->linesize[2],
               frame->data[2] + ((y + h) / 2) * frame->linesize[2] + (x / 2),
               (crop_width + 1) / 2);
    }

    return cropped_frame;
}

static int lbvdec_uhs_decode(AVCodecContext *avctx, AVFrame *pict,
    int *got_frame, AVPacket *avpkt) {
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    int ret ;
	int current_count;
    fprintf(stderr, "lbvdec_uhs_decode enter\n");
#if 0
    static FILE *base_bin_fp;
    if(!base_bin_fp) base_bin_fp = fopen("testout/base_str_rx.bin","wb");
    if(base_bin_fp){
        fwrite(avpkt->data, 1, avpkt->size , base_bin_fp);
        fflush(base_bin_fp);
    }
    //av_usleep(10000000);
#endif
    *got_frame = 0;
    ret = avcodec_send_packet(avctx, avpkt);
    if (ret < 0) {
        fprintf(stderr, "Dec error happened.\n");
        return -1; 
    }
    AVFrame *decoded_frame = av_frame_alloc();
    ret = avcodec_receive_frame(avctx, decoded_frame);
    if (ret < 0) {
        fprintf(stderr, "Dec receive frame error happened.\n");
        return 0; 
    }else{
        *got_frame = 1;
    }
    AVFrame** blks = av_malloc(ctx->num_blk * sizeof(AVFrame*));
    current_count = ctx->counter;
	// Add a frame and check the return value
    if (add_yuv420p_frame(decoded_frame, blks, ctx->num_blk, &current_count)) {
        printf("Successfully filled the small frame array.\n");
    } else {
        printf("Added a frame but not full yet.\n");
		*got_frame = 0;
		goto end;
    }

    *got_frame = 1;
	if(current_count!=ctx->num_blk){
		printf("error!!!!!\n");
		goto end;
	}
	// Assemble the small frames into a large frame
	AVFrame *decoded_big_pict = assemble_yuv420p_frames(blks, current_count, ctx->set_blk_w, ctx->set_blk_h, avctx->coded_width, avctx->coded_height);
	if (decoded_big_pict) {
		printf("Successfully assembled the big frame.\n");
		// Use decoded_big_pict for further processing
		// Define crop dimensions
		int crop_x = avctx->coded_width; // X coordinate of the crop start
		int crop_y = avctx->coded_height; // Y coordinate of the crop start
		int crop_width = avctx->width; // Desired crop width
		int crop_height = avctx->width; // Desired crop height

		// Crop the big frame
		pict = crop_yuv420p_frame(decoded_big_pict, crop_x, crop_y, crop_width, crop_height);
		if (pict) {
			printf("Successfully cropped the frame to %dx%d.\n", crop_width, crop_height);
		}

		av_frame_free(&decoded_big_pict); // Don't forget to free it after use
	} else {
		printf("Failed to assemble the big frame.\n");
	}
	// Free small frames
    for (int i = 0; i < current_count; i++) {
        av_frame_free(&blks[i]);
    }
    free(blks);
	


end:
	if(decoded_frame) av_frame_free(decoded_frame);
    return 0;
    
}


static av_cold int lbvdec_uhs_close(AVCodecContext *avctx) {
    LowBitrateDecoderUHSContext *ctx = avctx->priv_data;
    // 清理编码器

    return 0;
}

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
    .bsfs           = "nuhd_to_normal",
    .p.wrapper_name = "lbvdec_uhs",
};

