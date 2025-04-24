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



#define MAX_MERGE_BLK_PKTS_SIZE (40 * 1024 * 1024) // 4 MB
#define MIN_MERGE_PACKET_SIZE (10 * 1024 * 1024) // Minimum packet size


#define PKT_COUNT_POS_H 2
#define PKT_COUNT_POS_L 3
typedef struct {
    AVPacket *merged_packet; // Merged AVPacket to hold combined data
    int is_initialized; // Flag to indicate if the merged_packet has been initialized
    size_t buffer_size; // Size of the allocated buffer
	int pkt_count;

    //header
    int frame_w;
    int frame_h;

    int blk_w;
    int blk_h;
    

} MergeContext;

typedef struct {
    AVClass *class;
    // and other encoder params
    int inited;
    int bypass;

    //encoder options
    int base_codec;

    int w;
    int h;
	
    int set_bitrate;
	int set_blk_w;
	int set_blk_h;

	int num_blk;

    // baseenc_ctx
    AVCodecContext *baseenc_ctx; 
	
	MergeContext *last_merge_pkt;

    //int pts;
} LowBitrateEncoderUHSContext;

// Initialize the merge context
static int init_merge_context(MergeContext *ctx,LowBitrateEncoderUHSContext *lb_ctx) {
    memset(ctx, 0, sizeof(MergeContext)); // Clear the context
    
    ctx->merged_packet = av_packet_alloc(); // Allocate and initialize the packet
    if (!ctx->merged_packet) {
        return -1; // Memory allocation failed
    }
    
    ctx->merged_packet->size = 0; // Initialize size to 0
    ctx->is_initialized = 0; // Set the initialization flag to false
    ctx->buffer_size = 0; // Initialize buffer size
	
	ctx->pkt_count = 0;

    ctx->frame_w = lb_ctx->w;
    ctx->frame_h = lb_ctx->h;
    ctx->blk_w = lb_ctx->set_blk_w;
    ctx->blk_h = lb_ctx->set_blk_h;
    return 0;
}

static void add_frame_header(MergeContext *ctx) {
    *(ctx->merged_packet->data+PKT_COUNT_POS_H) = (ctx->pkt_count+1) & 0xFF00;
    *(ctx->merged_packet->data+PKT_COUNT_POS_L) = (ctx->pkt_count+1) & 0x00FF; 
}

// Add a single AVPacket to the merged AVPacket
static int add_packet_to_merge(MergeContext *ctx, AVPacket *pkt) {
    PutByteContext pb;
    if (!ctx || !pkt) {
        return -1; // Invalid parameters
    }
    av_log(NULL, AV_LOG_DEBUG,"add_packet_to_merge pkt->size:%d \n",pkt->size);
    // If not initialized, allocate initial memory for the merged_packet
    if (!ctx->is_initialized) {
        int pos;
        ctx->buffer_size = pkt->size > MIN_MERGE_PACKET_SIZE ? pkt->size : MIN_MERGE_PACKET_SIZE; // Allocate enough space
        ctx->merged_packet->data = av_malloc(ctx->buffer_size); // Initial allocation
        if (!ctx->merged_packet->data) {
            return -1; // Memory allocation failed
        }
        *(ctx->merged_packet->data) = 0xFF;
        *(ctx->merged_packet->data+1) = 0xFE;
        *(ctx->merged_packet->data+2) = 0x00; // PKT_COUNT_POS_H,reserve for end of pkt
        *(ctx->merged_packet->data+3) = 0x00; // PKT_COUNT_POS_L,reserve for end of pkt
        pos = 4;
        bytestream2_init_writer(&pb, ctx->merged_packet->data+4, ctx->buffer_size-4);
        bytestream2_put_be16(&pb, ctx->frame_w);
        bytestream2_put_be16(&pb, ctx->frame_h);
        bytestream2_put_be16(&pb, ctx->blk_w);
        bytestream2_put_be16(&pb, ctx->blk_h);
        av_log(NULL, AV_LOG_DEBUG,"write to header:%d %d %d %d\n",ctx->frame_w,ctx->frame_h,ctx->blk_w,ctx->blk_h);
        pos += bytestream2_tell_p(&pb);
        memcpy(ctx->merged_packet->data + pos, pkt->data, pkt->size); // Copy the packet data
        ctx->merged_packet->size = pkt->size + pos; // Set the size
        ctx->merged_packet->pts = pkt->pts; // Set PTS from the first packet
        ctx->merged_packet->dts = pkt->dts; // Set DTS from the first packet
        ctx->merged_packet->duration = pkt->duration; // Set duration from the first packet
        ctx->is_initialized = 1; // Mark as initialized
    } else {
        // Check if more space is needed, if so, reallocate
        av_log(NULL, AV_LOG_DEBUG,"add_packet_to_merge reallocate pkt->size:%d \n",pkt->size);
        if (ctx->merged_packet->size + pkt->size > ctx->buffer_size) {
            size_t new_buffer_size = ctx->buffer_size * 2; // Double the buffer size
            while (ctx->merged_packet->size + pkt->size  > new_buffer_size) {
				if((new_buffer_size * 2) < MAX_MERGE_BLK_PKTS_SIZE){
					new_buffer_size *= 2; // Keep doubling until it fits
				}else{
					return -1; // Memory allocation failed
				}
            }
            uint8_t *new_data = av_realloc(ctx->merged_packet->data, new_buffer_size);
            if (!new_data) {
                return -1; // Memory allocation failed
            }
            ctx->merged_packet->data = new_data; // Update the data pointer
            ctx->buffer_size = new_buffer_size; // Update the buffer size
        }
        
        // Copy new packet data
        memcpy(ctx->merged_packet->data + ctx->merged_packet->size, pkt->data, pkt->size); 
        ctx->merged_packet->size += pkt->size; // Update the size
    }
	ctx->pkt_count += 1;

    return 0; // Success
}

// Cleanup the merge context
static void cleanup_merge_context(MergeContext *ctx) {
    av_log(NULL,AV_LOG_DEBUG,"cleanup_merge_context\n");
    av_free(ctx->merged_packet->data); // Unreference and free the merged packet
    ctx->merged_packet->data = NULL; // Clear pointer to avoid dangling references
    ctx->merged_packet->size = 0; // Reset size
    ctx->is_initialized = 0; // Reset the initialization flag
    ctx->buffer_size = 0; // Reset buffer size
	ctx->pkt_count = 0;
    av_packet_free(&ctx->merged_packet);
}


static AVFrame** cut_yuv420p_frame(AVFrame* input_frame, int blk_w, int blk_h, int* num_blocks) {
    int width = input_frame->width;
    int height = input_frame->height;

    int num_x_blocks = (width + blk_w - 1) / blk_w; // Ensure rounding up
    int num_y_blocks = (height + blk_h - 1) / blk_h; // Ensure rounding up

    *num_blocks = num_x_blocks * num_y_blocks;
    AVFrame** output_frames = malloc(*num_blocks * sizeof(AVFrame*));

    for (int y = 0; y < num_y_blocks; y++) {
        for (int x = 0; x < num_x_blocks; x++) {
            // Create new frame for each block
            output_frames[y * num_x_blocks + x] = av_frame_alloc();
            output_frames[y * num_x_blocks + x]->width = blk_w;
            output_frames[y * num_x_blocks + x]->height = blk_h;
            output_frames[y * num_x_blocks + x]->format = AV_PIX_FMT_YUV420P;

            if (av_frame_get_buffer(output_frames[y * num_x_blocks + x], 32) < 0) {
                fprintf(stderr, "Could not allocate output frame data\n");
                return NULL; // Handle error appropriately
            }

            // Determine the starting position in the input frame
            int start_x = x * blk_w;
            int start_y = y * blk_h;

            // Copy Y data
            for (int j = 0; j < blk_h; j++) {
                for (int i = 0; i < blk_w; i++) {
                    int src_x = start_x + i;
                    int src_y = start_y + j;

                    // Check bounds and perform pixel filling
                    if (src_x < width && src_y < height) {
                        output_frames[y * num_x_blocks + x]->data[0][j * output_frames[y * num_x_blocks + x]->linesize[0] + i] =
                            input_frame->data[0][src_y * input_frame->linesize[0] + src_x];
                    } else {
                        // Fill with the last valid pixel
                        output_frames[y * num_x_blocks + x]->data[0][j * output_frames[y * num_x_blocks + x]->linesize[0] + i] =
                            input_frame->data[0][(height - 1) * input_frame->linesize[0] + (width - 1)];
                    }
                }
            }

            // Copy U and V data (each has half the vertical resolution)
            for (int j = 0; j < (blk_h + 1) / 2; j++) {
                for (int i = 0; i < (blk_w + 1) / 2; i++) {
                    int src_x = (start_x / 2) + i; // U/V blocks are half the width
                    int src_y = (start_y / 2) + j;

                    // Check bounds and perform pixel filling
                    if (src_x < (width / 2) && src_y < (height / 2)) {
                        output_frames[y * num_x_blocks + x]->data[1][j * output_frames[y * num_x_blocks + x]->linesize[1] + i] =
                            input_frame->data[1][src_y * input_frame->linesize[1] + src_x];
                        output_frames[y * num_x_blocks + x]->data[2][j * output_frames[y * num_x_blocks + x]->linesize[2] + i] =
                            input_frame->data[2][src_y * input_frame->linesize[2] + src_x];
                    } else {
                        // Fill with the last valid pixel
                        output_frames[y * num_x_blocks + x]->data[1][j * output_frames[y * num_x_blocks + x]->linesize[1] + i] =
                            input_frame->data[1][((height / 2) - 1) * input_frame->linesize[1] + ((width / 2) - 1)];
                        output_frames[y * num_x_blocks + x]->data[2][j * output_frames[y * num_x_blocks + x]->linesize[2] + i] =
                            input_frame->data[2][((height / 2) - 1) * input_frame->linesize[2] + ((width / 2) - 1)];
                    }
                }
            }
        }
    }

    return output_frames;
}

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

// Dump YUV data to fp
static void dump_yuv_to_fp(AVFrame* frame, const FILE* file) {
    
    if (!file) {
        fprintf(stderr, "Could not open file for writing\n");
        return;
    }

    // Write Y component
    fwrite(frame->data[0], 1, frame->linesize[0] * frame->height, file);
    // Write U component
    fwrite(frame->data[1], 1, frame->linesize[1] * (frame->height / 2), file);
    // Write V component
    fwrite(frame->data[2], 1, frame->linesize[2] * (frame->height / 2), file);

    fflush(file);
}


static int base_encode_function(AVCodecContext *basectx, AVFrame *frame, AVPacket **pkt ,int receive_flag){

    int ret = -1;
    AVCodecContext *enc_ctx = basectx; 

    if(frame ){
        ret = avcodec_send_frame(enc_ctx, frame);
        if (ret < 0) {
            av_log( basectx,AV_LOG_ERROR,"baseenc send frame err \n");
            return ret;
        }
        av_log( basectx,AV_LOG_DEBUG,"baseenc send frame down \n");
    }else{
        av_log( basectx,AV_LOG_DEBUG,"baseenc send frame null \n");
    }
    
    if(!receive_flag){
        goto end;
    }

    (*pkt) = av_packet_alloc();
    if (!(*pkt)) {
        av_log( basectx,AV_LOG_DEBUG, "Could not allocate AVPacket\n");
        return -1; // Handle allocation error appropriately
    }

    ret = avcodec_receive_packet(enc_ctx, (*pkt));
    if (ret == 0) {
        // Copy data from pkt to str if pkt has data
        if (((*pkt)->size > 0) && ((*pkt)->data)) {
            av_log( basectx,AV_LOG_DEBUG,"baseenc avcodec_receive_packet key:%d\n",((*pkt)->flags & AV_PKT_FLAG_KEY));
            //ping - pong
#if 1
            static FILE *base_bin_fp;
            if(!base_bin_fp) base_bin_fp = fopen("testout/base_str.bin","wb");
            if(base_bin_fp){
                fwrite((*pkt)->data, 1, (*pkt)->size , base_bin_fp);
                fflush(base_bin_fp);
            }
            //sleep(10);
#endif

            
        } else {
            av_log( basectx,AV_LOG_DEBUG, "No data generated.\n");
        }

    }else{
        av_log( basectx,AV_LOG_DEBUG, "baseenc wait for pkt data return. \n");
        return 0;
    }

end:
    av_log( basectx,AV_LOG_DEBUG, "base_encode_function down. \n");
    return 0;
}


static av_cold int lbvc_uhs_init(AVCodecContext *avctx) {
    enum AVCodecID base_codec_id;
    AVCodec *baseenc_codec;

    av_log(avctx, AV_LOG_DEBUG,"__lbvc_uhs_init enter! \n");
    LowBitrateEncoderUHSContext *ctx = avctx->priv_data;
    // init encoders
    ctx->base_codec = 0; //defalut, now only support 264

    system("mkdir ./testout");
    
    av_log(avctx, AV_LOG_DEBUG,"yuv file loading...base_codec:%d \n",ctx->base_codec);
    

    //init uncompressed data context
    int _width = avctx->width;
    int _height = avctx->height;
    int _coded_width = avctx->coded_width;
    int _coded_height = avctx->coded_height;
    ctx->w = _width;
    ctx->h = _height;
	av_log(avctx, AV_LOG_DEBUG,"yuv file _widthx_height:%dx%d blk _widthx_height:%dx%d \n",_width,_height,ctx->set_blk_w,ctx->set_blk_h);
    
	ctx->num_blk = ((_width + ( ctx->set_blk_w - 1 )) / ctx->set_blk_w) * ((_height + ( ctx->set_blk_h - 1 )) / ctx->set_blk_h);
    av_log(avctx, AV_LOG_DEBUG,"yuv file num_blks %d \n",ctx->num_blk);
    
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
    
    //init baseenc ctx
    ctx->baseenc_ctx->bit_rate = ctx->set_bitrate;
    ctx->baseenc_ctx->width = ctx->set_blk_w;
    ctx->baseenc_ctx->height = ctx->set_blk_h;
    ctx->baseenc_ctx->time_base = (AVRational){1, ctx->num_blk*4};
    ctx->baseenc_ctx->gop_size = ctx->num_blk;
    ctx->baseenc_ctx->keyint_min = ctx->num_blk;
    ctx->baseenc_ctx->slice_count = 1;
    ctx->baseenc_ctx->refs = 3;
    ctx->baseenc_ctx->has_b_frames = 1;
    ctx->baseenc_ctx->max_b_frames = 2;
    ctx->baseenc_ctx->thread_count = 1;
#ifdef __Xilinx_ZCU106__
    ctx->baseenc_ctx->pix_fmt = AV_PIX_FMT_NV12;
#else
    ctx->baseenc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
#endif
    av_opt_set(ctx->baseenc_ctx->priv_data,"slice_mode","1",0);

    if(strcmp(baseenc_codec->name,"libx264") == 0){
        //use x264
        //ban scenecut
	    av_opt_set(ctx->baseenc_ctx->priv_data, "x264-params", "scenecut=0", 0);
    }
	

    av_log(avctx, AV_LOG_DEBUG,"lbvc_uhs_init avcodec_open2 start. \n");
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "preset", "fast", 0); 
    av_dict_set(&opts, "tune", "zerolatency", 0); 

    if (avcodec_open2(ctx->baseenc_ctx, baseenc_codec, &opts) < 0) {
        avcodec_free_context(&ctx->baseenc_ctx);
        return AVERROR_UNKNOWN;
    }
    av_dict_free(&opts);
    av_log(avctx, AV_LOG_DEBUG,"lbvc_uhs_init avcodec_open2 down. \n");
	
	ctx->last_merge_pkt = NULL;

    return 0;
}

static int lbvc_uhs_encode(AVCodecContext *avctx, AVPacket *pkt,
    const AVFrame *frame, int *got_packet) {
    LowBitrateEncoderUHSContext *ctx = avctx->priv_data;
    AVFrame *tmp;
	MergeContext *merge_ctx;
	MergeContext *next_merge_ctx;
    int ret = -1;
    if(!frame){
        *got_packet = 0;
        return 0;
    }
    AVPacket *tmp_pkt;
	
	// Initialize the merge context
	if(ctx->last_merge_pkt){
		merge_ctx = ctx->last_merge_pkt;
	}else{
		merge_ctx = (MergeContext *)av_malloc(sizeof(MergeContext));
		if(init_merge_context(merge_ctx,ctx) < 0){
			return -1;
		}
	}
	next_merge_ctx = (MergeContext *)av_malloc(sizeof(MergeContext));
	if(init_merge_context(next_merge_ctx,ctx) < 0){
		return -1;
	}
	
once:

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

    av_log(avctx, AV_LOG_DEBUG,"==============>lbvc_uhs_encode<============== \n");
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
			int blk_w = ctx->baseenc_ctx->width; // Block width
			int blk_h = ctx->baseenc_ctx->height; // Block height
			int num_blocks = 0;
            int full_flag = 0;
            int change_flag = 0;
            MergeContext *curr = merge_ctx;

			// Cut the YUV420P frame
			AVFrame** output_frames = cut_yuv420p_frame(frame, blk_w, blk_h, &num_blocks);
			if (!output_frames) {
				goto err;
			}
            
			// Save each output frame to a file
			for (int i = 0; i < num_blocks; i++) {
#if 0//dump frames
                static int frames = 1;
				char filename[256];
				snprintf(filename, sizeof(filename), "testout/output%d_block_%d.yuv",frames, i);
				dump_yuv_to_file(output_frames[i], filename);
                frames++;
#endif
#if 0//dump to file
                static FILE * fp = NULL;
                if(!fp) fp = fopen("testout/output_blocks.yuv","wb");
                if(fp){
                    dump_yuv_to_fp(output_frames[i], fp);
                }
#endif
                if(base_encode_function(ctx->baseenc_ctx,output_frames[i],&tmp_pkt,1) < 0){
                    av_log(avctx, AV_LOG_ERROR,"base_encode_function err \n");
                    av_packet_free(&tmp_pkt);
				    av_frame_free(&output_frames[i]); // Free each frame's memory
					goto err;
				}
                if(tmp_pkt->size == 0){
                    av_log(avctx, AV_LOG_DEBUG,"tmp_pkt return size 0,wait \n");
                    av_packet_free(&tmp_pkt);
				    av_frame_free(&output_frames[i]); // Free each frame's memory
                    continue;
                }

                // ping - pong
                if(!full_flag && (tmp_pkt->flags & AV_PKT_FLAG_KEY) && (curr->pkt_count>0)){
                    if(change_flag){
                        full_flag = 1;
                    }else{
                        change_flag = 1;
                    }
                    add_frame_header(curr);
                    
                    av_log(avctx, AV_LOG_DEBUG,"cut_yuv420p_frame down merge_ctx->merged_packet->size:%d\n",merge_ctx->merged_packet->size);
                    //malloc pkt
                    ret = av_new_packet(pkt , curr->merged_packet->size);
                    if(ret < 0){
                        av_log(avctx, AV_LOG_DEBUG,"av_new_packet error\n");
                        return ret;
                    }
                    av_log(avctx, AV_LOG_DEBUG,"lbvenc uhs packet size:%d  count:%d(ctx->num_blk:%d)\n",pkt->size,merge_ctx->pkt_count,ctx->num_blk);
                    memcpy(pkt->data,curr->merged_packet->data,pkt->size);
                    *got_packet = 1;
                    //pkt->pts = ctx->pts;
                    //ctx->pts++;

                    cleanup_merge_context(curr);

                    curr = next_merge_ctx;
                    ctx->last_merge_pkt = next_merge_ctx;

				}
                
                if(add_packet_to_merge(curr, tmp_pkt) < 0){
                    av_log(avctx, AV_LOG_ERROR,"add_packet_to_merge err, curr 0x%08x \n",curr);
                }else{
                    av_log(avctx, AV_LOG_DEBUG,"add_packet_to_merge down, curr 0x%08x \n",curr);
                }
             
                // Free the packet after use
                av_packet_free(&tmp_pkt);
				av_frame_free(&output_frames[i]); // Free each frame's memory
			}
            
			// Release resources
			if(output_frames) free(output_frames);

            break;
        default:
            av_log(avctx, AV_LOG_DEBUG,"cut_yuv420p_frame not support yuv format .(%d) \n",frame->format);
            goto err;
    }
    av_log(avctx, AV_LOG_DEBUG,"cut_yuv420p_frame down \n");
	
	if(*got_packet){
        
        ctx->last_merge_pkt = next_merge_ctx;
	}else{
        av_log(avctx, AV_LOG_DEBUG,"lbvenc uhs  count:%d(ctx->num_blk:%d)\n",pkt->size,merge_ctx->pkt_count,ctx->num_blk);
		
        ctx->last_merge_pkt = merge_ctx;
        goto got_no_data;
    }
    
    //enc
    
    if(tmp) av_frame_free(&tmp);
    //if(pkt) av_packet_free(&pkt);
    return 0;

got_no_data:
    av_log(avctx, AV_LOG_ERROR,"lbvc_uhs_encode got no data\n");
    if(tmp) av_frame_free(&tmp);
    //if(pkt) av_packet_free(&pkt);
    *got_packet = 0;
    return 0;
err:    
    av_log(avctx, AV_LOG_ERROR,"lbvc_uhs_encode error happened\n");
    if(tmp) av_frame_free(&tmp);
    //if(pkt) av_packet_free(&pkt);
    return -1;
    
}

static void lbvc_uhs_flush(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_DEBUG,"lbvc_uhs_flush enter! \n");
}

static av_cold int lbvc_uhs_close(AVCodecContext *avctx) {
    LowBitrateEncoderUHSContext *ctx = avctx->priv_data;
    // 清理编码器

    return 0;
}

#define OFFSET(x) offsetof(LowBitrateEncoderUHSContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption lbvc_uhs_options[] = {
    {"bitrate", "set bitrate ", OFFSET(set_bitrate), AV_OPT_TYPE_INT, {.i64 = 4000000}, 800000, 40000000, VE, "set_bitrate"},
    {"blk_w", "set the w of enc blk ", OFFSET(set_blk_w), AV_OPT_TYPE_INT, {.i64 = 1920}, 0, 7680, VE, "set_blk_w"},
    {"blk_h", "set the h of enc blk", OFFSET(set_blk_h), AV_OPT_TYPE_INT, {.i64 = 1088}, 0, 4320, VE, "set_blk_h"},
    {NULL} // end flag
};

static const AVClass lbvc_uhs_class = {
    .class_name = "lbvc_uhs",
    .item_name  = av_default_item_name,
    .option     = lbvc_uhs_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const FFCodecDefault lbvc_uhs_defaults[] = {
    { "b", "2M" },
    { NULL },
};



static const enum AVPixelFormat pix_fmts_all[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_NV21,
    AV_PIX_FMT_NONE
};

FFCodec ff_liblbvc_uhs_encoder = {
    .p.name           = "lbvc_uhs",
    CODEC_LONG_NAME("libhqbo lbvenc Low Bitrate Video Encoder :: Version-Ultra High Resolution"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_LBVC_UHS,
    .p.capabilities   = AV_CODEC_CAP_DR1 ,
    .p.priv_class     = &lbvc_uhs_class,
    .p.wrapper_name   = "lbvc_uhs",
    .priv_data_size   = sizeof(LowBitrateEncoderUHSContext),
    .init             = lbvc_uhs_init,
    FF_CODEC_ENCODE_CB(lbvc_uhs_encode),
    .flush            = lbvc_uhs_flush,
    .close            = lbvc_uhs_close,
    .defaults         = lbvc_uhs_defaults,
    .p.pix_fmts       = pix_fmts_all,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_AUTO_THREADS
                      ,
};


