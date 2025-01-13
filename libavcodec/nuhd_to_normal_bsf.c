/*
 * Media 100 to MJPEGB bitstream filter
 * Copyright (c) 2023 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * NUHD internal to Normal 
 */

#include "libavutil/intreadwrite.h"
#include "bsf.h"
#include "bsf_internal.h"
#include "bytestream.h"
#include "lbvenc.h"

static av_cold int init(AVBSFContext *ctx)
{
    
    switch(ctx->par_in->codec_id){
        case AV_CODEC_ID_LBVC:
            ctx->par_out->codec_id = AV_CODEC_ID_NUHD_NORMAL_H264;
            break;
        case AV_CODEC_ID_LBVC_HEVC:
        case AV_CODEC_ID_HLBVC:
            ctx->par_out->codec_id = AV_CODEC_ID_NUHD_NORMAL_HEVC;
            break;     
    }
    
    return 0;
}

static void modify_buffer(uint8_t *buf, size_t size) {
    
    const uint8_t seq1[] = {0x00, 0x00, 0x01};
    const uint8_t seq2[] = {0x00, 0x00, 0x00, 0x01};
    const uint8_t repl1[] = {0xFF, 0xFE, 0xFD};
    const uint8_t repl2[] = {0xFF, 0xFE, 0xFD, 0xFC};

    size_t i = 0;
    while (i < size) {
        
        if (i <= (size - 4) && (AV_RB32(buf + i) == 0x00000001)) {
            
            //memmove(buf + i + 4, buf + i + 4 - 1, size - (i + 4)); 
            *(buf + i) = repl2[0];
            *(buf + i + 1) = repl2[1];
            *(buf + i + 2) = repl2[2];
            *(buf + i + 3) = repl2[3];
            size += 3; 
            i += 4;
        }
#if 0
        else if (i <= size - 3 && memcmp(buf + i, seq1, 3) == 0) {
            
            memmove(buf + i + 3, buf + i + 3 - 1, size - (i + 3));
            memcpy(buf + i, repl1, 3);
            size += 2; 
            i += 3; 
        }
#endif
        else {
            i++;
        }

    }
}

static void modify_bytestream(GetByteContext gb,int start,int size) {
    
    const uint8_t seq1[] = {0x00, 0x00, 0x01};
    const uint8_t seq2[] = {0x00, 0x00, 0x00, 0x01};
    
    const uint8_t repl2[] = {0xFF, 0xFE, 0xFD, 0xFC};
    uint8_t *pos = gb.buffer + start; 
    size_t i = 0;
    while (i < size) {

        if (i <= (size - 4) && (AV_RB32(pos + i) == 0x00000001)) {
            
            //memmove(buf + i + 4, buf + i + 4 - 1, size - (i + 4));
            //printf("pos[%d]:0x%02x 0x%02x 0x%02x 0x%02x \n ",i,*(pos + i), *(pos + i + 1),*(pos + i + 2),*(pos + i + 3)); 
            *(pos + i) = repl2[0];
            *(pos + i + 1) = repl2[1];
            *(pos + i + 2) = repl2[2];
            *(pos + i + 3) = repl2[3];
            size += 3; 
            i += 4;
        }

#if 1
        else if (i <= (size - 3) && (AV_RB24(pos + i) == 0x000001)) {
            
            //printf("pos[%d]:0x%02x 0x%02x 0x%02x\n ",i,*(pos + i), *(pos + i + 1),*(pos + i + 2)); 
            *(pos + i) = repl2[0];
            *(pos + i + 1) = repl2[1];
            *(pos + i + 2) = repl2[2];
            size += 2; 
            i += 3; 
        }
#endif
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
/**
frame size:32 bits(not include type byte)
frame type:8 bits
**/
static int filter_lbvc(AVBSFContext *ctx, AVPacket *out)
{
    unsigned second_field_offset = 0;
    unsigned next_field = 0;
    unsigned field = 0;
    GetByteContext gb;
    PutByteContext pb;
    AVPacket *in;
    int ret;
    int base_codec_id_internal;

    uint8_t *pos ;
    uint32_t size ;
    uint32_t total_size = 0;
    uint32_t write_total_size = 0;
    uint8_t type ;
    int end = 0;
    int field_s = 0;
    int write_down = 0;
    enum AVCodecID base_codec_id;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    ret = av_new_packet(out, in->size + 4096);
    if (ret < 0)
        goto fail;

    
    //AVPacketSideData *side_data = av_packet_new_side_data(out,AV_PKT_DATA_SEI,in->size);
    //debug
#if 0
    static FILE *fp_in = NULL;
    if(!fp_in) fp_in = fopen("check_bsf_in.bin","w");
    if(fp_in) fwrite(in->data,1,in->size,fp_in);
    fflush(fp_in);
#endif
    bytestream2_init(&gb, in->data, in->size);
    bytestream2_init_writer(&pb, out->data, out->size);

    pos = in->data + end;
    base_codec_id_internal = AV_RB8(pos);
    end += 1;
    bytestream2_skip(&gb, 1);
    base_codec_id = lbvenc_common_trans_internal_base_codecid_to_codecid(base_codec_id_internal);
    
    
second_field:
    pos = in->data + end;
    size = AV_RB32(pos);
    type = AV_RB8(pos + 4);
    bytestream2_skip(&gb, 4);
    bytestream2_skip(&gb, 1);
    av_log(ctx, AV_LOG_DEBUG,"111type:0x%02x (0x%08x) size=%d\n",type,bytestream2_tell_p(&gb),size);
    
 
    if(type == 0x0){
        //base layer
        bytestream2_copy_buffer(&pb, &gb, size);
    }else if(type == 0x01){
        //enhance size
        write_total_size = (size+5+4+4);//+13 bytes: 4 bytes size , 1 byte type ,4 bytes roi pos x,,4 bytes roi pos y
        //write sei header
        bytestream2_put_byte(&pb, 0x00);
        bytestream2_put_byte(&pb, 0x00);
        bytestream2_put_byte(&pb, 0x00);
        bytestream2_put_byte(&pb, 0x01);
        if(base_codec_id == AV_CODEC_ID_HEVC){
            bytestream2_put_byte(&pb, 0x50);
            bytestream2_put_byte(&pb, 0x01);
        }else if(base_codec_id == AV_CODEC_ID_H264){
            bytestream2_put_byte(&pb, 0x06);
        }
        bytestream2_put_byte(&pb, 0xCD);
        //write size for sei
        av_log(ctx, AV_LOG_DEBUG,"write 0x%02x(%d) \n",write_total_size,write_total_size); 
        while(write_total_size >= 0xFF){
            bytestream2_put_byte(&pb,0xFF);
            write_total_size -= 0xFF;
            //av_log(ctx, AV_LOG_DEBUG,"write 0xFF(%d) \n",write_total_size);
        }
        bytestream2_put_byte(&pb,write_total_size);
        av_log(ctx, AV_LOG_DEBUG,"write 0x%02x(%d) \n",write_total_size,write_total_size);
    }else if(type == 0x10){ 
        int roi_x = 0;
        int roi_y = 0;
        int size2 = 0;
        bytestream2_put_byte(&pb, 0xE0);
        bytestream2_put_be32(&pb, size);
        roi_x = bytestream2_get_be32(&gb);
        roi_y = bytestream2_get_be32(&gb);
        bytestream2_put_be32(&pb, roi_x);// roi x
        bytestream2_put_be32(&pb, roi_y);// roi y
        av_log(ctx, AV_LOG_DEBUG,"get layer1 roi pos:(%d,%d) size:%d\n",roi_x,roi_y,size);
        modify_bytestream(gb,0,size);
        size2 = bytestream2_copy_buffer(&pb, &gb, size);
        if(size2 != size ){
            av_log(ctx, AV_LOG_ERROR,"error happened. 111\n");
            int loop = 1000000000000000000000000000;
            while(loop) loop--;;
        }
    }else if(type == 0x11){
        //enhance layer2
        bytestream2_put_byte(&pb, 0xE1);
        bytestream2_put_be32(&pb, size);
        if(size > 0){
            modify_bytestream(gb,0,size);
            bytestream2_copy_buffer(&pb, &gb, size);
        }
    }else{
        av_log(ctx, AV_LOG_ERROR,"error happened.\n");
        int loop = 1000000000000000000000000000;
        while(loop) loop--;;
    } 
    
    
    end = bytestream2_tell_p(&gb);
    av_log(ctx, AV_LOG_DEBUG,"end:0x%08x (0x%08x) \n",end,in->size);
    if( (in->size - end) > 0 ){
        goto second_field;
    }
    

    out->size = bytestream2_tell_p(&pb);
    av_log(ctx, AV_LOG_DEBUG,"nuhd_to_normal int:%d , out packet size:%d \n",in->size,out->size);

    //debug
#if 0
    static FILE *fp = NULL;
    if(!fp) fp = fopen("check_bsf.bin","w");
    if(fp) fwrite(out->data,1,out->size,fp);
    fflush(fp);
#endif

    ret = av_packet_copy_props(out, in);
    if (ret < 0){
        av_log(ctx, AV_LOG_ERROR,"filter_lbvc av_packet_copy_props error. \n");
        goto fail;
    }
    av_log(ctx, AV_LOG_DEBUG,"222 nuhd_to_normal int:%d , out packet size:%d \n",in->size,out->size);    

fail:
    if (ret < 0)
        av_packet_unref(out);
    av_packet_free(&in);
    return ret;
}

static uint8_t fake_hevc_frame_old[] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x08, 
    0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 
    0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 
    0x00, 0x7B, 0x00, 0x00, 0x93, 0x22, 0x95, 0xCC, 
    0x49, 0x8B, 0x02, 0x40, 0x00, 0x00, 0x00, 0x01, 
    0x42, 0x01, 0x08, 0x01, 0x60, 0x00, 0x00, 0x03, 
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 
    0x03, 0x00, 0x7B, 0x00, 0x00, 0xA0, 0x0D, 0x08, 
    0x0F, 0x1F, 0xE5, 0x93, 0x22, 0x95, 0xCC, 0x49, 
    0x8B, 0x92, 0x46, 0xD8, 0x2C, 0x48, 0x42, 0x20, 
    0x23, 0x08, 0xCC, 0x4F, 0x97, 0xEC, 0xFD, 0x7C, 
    0xD9, 0xE5, 0xFE, 0xBE, 0x65, 0x0E, 0x4B, 0xEF, 
    0xF5, 0xF3, 0x67, 0x97, 0xFA, 0xF8, 0x50, 0x86, 
    0x11, 0xC4, 0xF2, 0xF6, 0xC8, 0x00, 0x00, 0x00, 
    0x01, 0x44, 0x01, 0xC1, 0xA5, 0x58, 0x11, 0x20, 
    0x00, 0x00, 0x01, 0x26, 0x01, 0x00, 0x00, 0x00, 
    0x01, 0x4E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00
};
static uint8_t fake_hevc_frame[] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 
    0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 
    0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 
    0x00, 0x7B, 0xF0, 0x24, 0x00, 0x00, 0x00, 0x01, 
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 
    0x03, 0x00, 0x7B, 0xA0, 0x10, 0x20, 0x20, 0x7F, 
    0x97, 0xE4, 0x91, 0xB6, 0x7B, 0x64, 0x00, 0x00, 
    0x00, 0x01, 0x44, 0x01, 0xC1, 0x90, 0x95, 0x81, 
    0x12, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF, 0x19, 
    0x80, 0xA2, 0x8F, 0xAB, 0x5A, 0x03, 0x96, 0xA7, 
    0xDB, 0xC6, 0xF2, 0x50, 0xB9, 0x02, 0x4C, 0x92, 
    0x6D, 0x8D, 0xD5, 0xF0, 0xC9, 0x41, 0xFA, 0xB5, 
    0x4B, 0x28, 0xD0, 0xD8, 0xA3, 0xA5, 0x88, 0x2D, 
    0x51, 0x11, 0x27, 0x7F, 0xBD, 0x63, 0xEE, 0x12, 
    0x99, 0x44, 0x2A, 0x18, 0x66, 0x41, 0x88, 0xF4, 
    0x49, 0x39, 0x07, 0x03, 0x21, 0xD9, 0x14, 0xD2, 
    0xC7, 0x5B, 0x28, 0xB6, 0x13, 0xAE, 0x16, 0xC3, 
    0x2C, 0xCE, 0x12, 0x0B, 0x5D, 0x14, 0x6F, 0xFA, 
    0x4C, 0xE6, 0xB3, 0x84, 0xBB, 0xD1, 0x3E, 0x5F, 
    0x57, 0x34, 0x85, 0x46, 0x20, 0x14, 0xAB, 0x16, 
    0x1F, 0x60
};
static int filter_e2e(AVBSFContext *ctx, AVPacket *out)
{
    unsigned second_field_offset = 0;
    unsigned next_field = 0;
    unsigned field = 0;
    GetByteContext gb;
    PutByteContext pb;
    AVPacket *in;
    int ret;

    uint8_t *pos ;
    uint32_t size ;
    uint8_t type ;
    int end = 0;

    ret = ff_bsf_get_packet(ctx, &in);
    if (ret < 0)
        return ret;

    ret = av_new_packet(out, in->size + 1024);
    if (ret < 0)
        goto fail;

    bytestream2_init(&gb, in->data, in->size);
    bytestream2_init_writer(&pb, out->data, out->size);

second_field:
    
    
    bytestream2_put_byte(&pb, 0x00);
    bytestream2_put_byte(&pb, 0x00);
    bytestream2_put_byte(&pb, 0x00);
    bytestream2_put_byte(&pb, 0x01);
    bytestream2_put_byte(&pb, 0x06);
    bytestream2_copy_buffer(&pb, &gb, in->size);
    //fake 265
    //av_log(ctx, AV_LOG_DEBUG,"outsize:0x%x %d\n",bytestream2_tell_p(&pb),bytestream2_tell_p(&pb));
    bytestream2_put_buffer(&pb,fake_hevc_frame,sizeof(fake_hevc_frame));
    //av_log(ctx, AV_LOG_DEBUG,"fake_hevc_frame size:0x%x %d\n",sizeof(fake_hevc_frame),sizeof(fake_hevc_frame));
    //av_log(ctx, AV_LOG_DEBUG,"outsize:0x%x %d\n",bytestream2_tell_p(&pb),bytestream2_tell_p(&pb));
    
    out->size = bytestream2_tell_p(&pb);
    

    ret = av_packet_copy_props(out, in);
    if (ret < 0)
        goto fail;

fail:
    if (ret < 0)
        av_packet_unref(out);
    av_packet_free(&in);
    return ret;
}

static int filter(AVBSFContext *ctx, AVPacket *out)
{
    int ret = 0;
    switch(ctx->par_in->codec_id){
        case AV_CODEC_ID_LBVC:
        case AV_CODEC_ID_LBVC_HEVC:
            ret = filter_lbvc(ctx,out);
            break;
        case AV_CODEC_ID_E2ENC:
            ret = filter_e2e(ctx,out);
            break;
    }

    return ret;
}

const FFBitStreamFilter ff_nuhd_to_normal_bsf = {
    .p.name         = "nuhd_to_normal",
    .p.codec_ids    = (const enum AVCodecID []){ AV_CODEC_ID_LBVC, AV_CODEC_ID_LBVC_HEVC, AV_CODEC_ID_HLBVC, AV_CODEC_ID_E2ENC, AV_CODEC_ID_NONE },
    .init           = init,
    .filter         = filter,
};
