#include "lbvenc.h"
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

#include "bytestream.h"
#include <limits.h>
#include "get_bits.h"


enum AVCodecID lbvenc_common_trans_internal_base_codecid_to_codecid(int internal_id){
    enum AVCodecID base_codec_id;
    switch(internal_id){
        case 0:
            base_codec_id = AV_CODEC_ID_H264;
            //printf("base codec:  AV_CODEC_ID_H264 \n");
            break;
        case 1:
            base_codec_id = AV_CODEC_ID_HEVC;
            //printf("AV_CODEC_ID_HEVC AV_CODEC_ID_HEVC \n");
            break;
        case 2:
            base_codec_id = AV_CODEC_ID_E2ENC;
            //printf("AV_CODEC_ID_E2ENC AV_CODEC_ID_E2ENC \n");
            break;
        default:
            printf("id error:%d \n",internal_id);
            return -1;
    }
    return base_codec_id;
}

static void modify_normal_bytestream_to_nuhd(GetByteContext gb,int start,int size) {
    
    const uint8_t seq1[] = {0xFF, 0xFE, 0xFD};
    const uint8_t seq2[] = {0xFF, 0xFE, 0xFD, 0xFC};
    
    const uint8_t repl2[] = {0x00, 0x00, 0x00, 0x01};
    uint8_t *pos = gb.buffer + start; 
    size_t i = 0;
    while (i < size) {

        if (i <= (size - 4) && (AV_RB32(pos + i) == 0xFFFEFDFC)) {
            
            //memmove(buf + i + 4, buf + i + 4 - 1, size - (i + 4));
            //av_log(logctx, AV_LOG_DEBUG,"pos[%d]:0x%02x 0x%02x 0x%02x 0x%02x \n ",i,*(pos + i), *(pos + i + 1),*(pos + i + 2),*(pos + i + 3)); 
            *(pos + i) = repl2[0];
            *(pos + i + 1) = repl2[1];
            *(pos + i + 2) = repl2[2];
            *(pos + i + 3) = repl2[3];
            size += 3; 
            i += 4;
        }

        else if (i <= (size - 3) && (AV_RB24(pos + i) == 0xFFFEFD)) {
            
            //av_log(logctx, AV_LOG_DEBUG,"pos[%d]:0x%02x 0x%02x 0x%02x\n ",i,*(pos + i), *(pos + i + 1),*(pos + i + 2)); 
            *(pos + i) = repl2[1];
            *(pos + i + 1) = repl2[2];
            *(pos + i + 2) = repl2[3];
            size += 2; 
            i += 3; 
        }

        else if (i <= (size - 3) && (AV_RB24(pos + i) == 0xFFFEFE)) {
            
            //av_log(logctx, AV_LOG_DEBUG,"pos[%d]:0x%02x 0x%02x 0x%02x\n ",i,*(pos + i), *(pos + i + 1),*(pos + i + 2)); 
            *(pos + i) = repl2[1];
            *(pos + i + 1) = repl2[2];
            *(pos + i + 2) = repl2[2];
            size += 2; 
            i += 3; 
        }

        else {
#if 0 //debug code
            static FILE *debug_log = NULL;
            if(!debug_log) debug_log = fopen("./debug.log","w");
            if(debug_log) {
                if(i && ((i%16)==0)) fprintf(debug_log,"\n"); 
                fprintf(debug_log,"0x%02x ",*(pos + i)); 
                fflush(debug_log);
            }
#endif
            i++;
        }

    }
}

int lbvenc_enhance_data_decode(H2645SEILbvencEnhanceData *s,GetByteContext *gb,void *logctx){
    uint8_t lbvenc_enhance_type;
    uint32_t size;
    uint8_t *buffer = NULL;
    int roi_x;
    int roi_y;
    int skip;
    
    lbvenc_enhance_type = bytestream2_get_byte(gb);
    av_log(logctx, AV_LOG_DEBUG,"decode_nal_sei_decoded_nuhd_lbvenc_enhance_data enter.\n");
    if (lbvenc_enhance_type == 0xE0) {
        size = bytestream2_get_be32(gb);
        roi_x = bytestream2_get_be16(gb);
        skip = bytestream2_get_be16(gb); // skip
        if(skip != 0xFFFE){
            av_log(logctx, AV_LOG_DEBUG,"lbvenc_enhance_data_decode error happened...\n");
            return -1;
        }
        roi_y = bytestream2_get_be16(gb);
        skip = bytestream2_get_be16(gb); // skip
        if(skip != 0xFFFE){
            av_log(logctx, AV_LOG_DEBUG,"lbvenc_enhance_data_decode error happened...\n");
            return -1;
        }
        av_log(logctx, AV_LOG_DEBUG,"lbvenc_enhance_data layer1 data...size=%d roi(%d,%d)\n",size,roi_x,roi_y);
        modify_normal_bytestream_to_nuhd(*gb,0,size);
        buffer = (uint8_t *)malloc(sizeof(uint8_t) * size);
        bytestream2_get_buffer(gb, buffer, size);
#if 0//debug
        static int enhance_data_layer1_counter = 0;
        char enhance_data_layer1_name[256];
        snprintf(enhance_data_layer1_name, sizeof(enhance_data_layer1_name), "testout/enhance_data_layer1_%d.jpg", enhance_data_layer1_counter++);
        FILE *enhance_data_layer1 = fopen(enhance_data_layer1_name,"wb");
        if(enhance_data_layer1){
            fwrite(buffer, 1, size , enhance_data_layer1);
            fclose(enhance_data_layer1);
        }
#endif
        s->layer1_data = buffer;
        s->layer1_size = size;
        s->layer1_roi_x = roi_x;
        s->layer1_roi_y = roi_y;

    } else if (lbvenc_enhance_type == 0xE1) {
        av_log(logctx, AV_LOG_DEBUG,"lbvenc_enhance_data layer2 data...\n");
        size = bytestream2_get_be32(gb);
        av_log(logctx, AV_LOG_DEBUG,"lbvenc_enhance_data layer2 data...size=%d\n",size);
        modify_normal_bytestream_to_nuhd(*gb,0,size);
        buffer = (uint8_t *)malloc(sizeof(uint8_t) * size);
        bytestream2_get_buffer(gb, buffer, size);
#if 0//debug
        static int enhance_data_layer2_counter = 0;
        char enhance_data_layer2_name[256];
        snprintf(enhance_data_layer2_name, sizeof(enhance_data_layer2_name), "testout/enhance_data_layer1_%d.jpg", enhance_data_layer1_counter++);
        FILE *enhance_data_layer2 = fopen(enhance_data_layer2_name,"wb");
        if(enhance_data_layer2){
            fwrite(buffer, 1, size , enhance_data_layer2);
            fclose(enhance_data_layer2);
        }
#endif
        s->layer2_data = buffer;
        s->layer2_size = size;
    } 
    s->present = 1;
    return 0;
}

int lbvenc_enhance_data_opaque_preprocess(H2645SEILbvencEnhanceData lbvenc_enhance_data,uint8_t** opaque){
    uint8_t *nuhd_extradata_buffer = (uint8_t *)av_malloc(lbvenc_enhance_data.layer1_size + 256);
    if (!nuhd_extradata_buffer) {
        return AVERROR(ENOMEM); // mem alloc err
    }
    AV_WB32(nuhd_extradata_buffer,lbvenc_enhance_data.layer1_roi_x);
    AV_WB32(nuhd_extradata_buffer + 4,lbvenc_enhance_data.layer1_roi_y);
    AV_WB32(nuhd_extradata_buffer + 8,lbvenc_enhance_data.layer1_size);
    //av_log(avctx, AV_LOG_DEBUG,"[nuhd]sei tx1: 0x%08x roi pos(%d,%d) data_size(%d) \n",nuhd_extradata_buffer,lbvenc_enhance_data.layer1_roi_x,lbvenc_enhance_data.layer1_roi_y,lbvenc_enhance_data.layer1_size);
    memcpy(nuhd_extradata_buffer + 12, lbvenc_enhance_data.layer1_data, lbvenc_enhance_data.layer1_size);
    (*opaque) = nuhd_extradata_buffer;
    return 0;
}

#define SIDE_DATA_TYPE_BLOCK_SIZE 1 // Custom side data type



int lbvc_add_dec_block_size_data(AVPacket *pkt, LBVC_UHS_DEC_SIDEDATA *block_size_data, void *logctx) {
    if (!block_size_data) {
        av_log(logctx, AV_LOG_ERROR,"Invalid LBVC_UHS_DEC_SIDEDATA pointer\n");
        return -1;
    }

    // Calculate the size needed for storage
    size_t size = sizeof(LBVC_UHS_DEC_SIDEDATA);

    // Allocate side data
    AVPacketSideData *side_data = av_packet_new_side_data(pkt, SIDE_DATA_TYPE_BLOCK_SIZE, size);
    if (!side_data) {
        av_log(logctx, AV_LOG_ERROR, "Failed to allocate side data\n");
        return -1;
    }

    // Copy the data to the side data
    memcpy(side_data, block_size_data, size);

    return 0;
}

int lbvc_read_dec_block_size_data(const AVPacket *pkt, LBVC_UHS_DEC_SIDEDATA *block_size_data, void *logctx) {
    if (!block_size_data) {
        av_log(logctx, AV_LOG_ERROR, "Invalid LBVC_UHS_DEC_SIDEDATA pointer\n");
        return -1;
    }

    // Retrieve side data
    size_t size = sizeof(LBVC_UHS_DEC_SIDEDATA);

    AVPacketSideData *side_data = av_packet_get_side_data(pkt, SIDE_DATA_TYPE_BLOCK_SIZE,&size);
    if (side_data && size == sizeof(LBVC_UHS_DEC_SIDEDATA)) {
        memcpy(block_size_data, side_data, sizeof(LBVC_UHS_DEC_SIDEDATA));
    } else {
        av_log(logctx, AV_LOG_ERROR, "No valid side data found\n");
        return -1;
    }
    return 0;
}
