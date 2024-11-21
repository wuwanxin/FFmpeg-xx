/*
 * HEVC Supplementary Enhancement Information messages
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2013 Vittorio Giovara
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

#include "bytestream.h"
#include "golomb.h"
#include "hevc_ps.h"
#include "hevc_sei.h"

static int decode_nal_sei_decoded_picture_hash(HEVCSEIPictureHash *s,
                                               GetByteContext *gb)
{
    int cIdx;
    uint8_t hash_type;
    //uint16_t picture_crc;
    //uint32_t picture_checksum;
    hash_type = bytestream2_get_byte(gb);

    for (cIdx = 0; cIdx < 3/*((s->sps->chroma_format_idc == 0) ? 1 : 3)*/; cIdx++) {
        if (hash_type == 0) {
            s->is_md5 = 1;
            bytestream2_get_buffer(gb, s->md5[cIdx], sizeof(s->md5[cIdx]));
        } else if (hash_type == 1) {
            // picture_crc = get_bits(gb, 16);
        } else if (hash_type == 2) {
            // picture_checksum = get_bits_long(gb, 32);
        }
    }
    return 0;
}

//nuhd add

static void modify_normal_bytestream_to_nuhd(GetByteContext gb,int start,int size) {
    
    const uint8_t seq1[] = {0xFF, 0xFE, 0xFD};
    const uint8_t seq2[] = {0xFF, 0xFE, 0xFD, 0xFC};
    
    const uint8_t repl2[] = {0x00, 0x00, 0x00, 0x01};
    uint8_t *pos = gb.buffer + start; 
    size_t i = 0;
    while (i < size) {

        if (i <= (size - 4) && (AV_RB32(pos + i) == 0xFFFEFDFC)) {
            
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
        else if (i <= (size - 3) && (AV_RB24(pos + i) == 0xFFFEFD)) {
            
            //printf("pos[%d]:0x%02x 0x%02x 0x%02x\n ",i,*(pos + i), *(pos + i + 1),*(pos + i + 2)); 
            *(pos + i) = repl2[1];
            *(pos + i + 1) = repl2[2];
            *(pos + i + 2) = repl2[3];
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
static int decode_nal_sei_decoded_nuhd_lbvenc_enhance_data(HEVCSEILbvencEnhanceData *s,
                                               GetByteContext *gb)
{

    uint8_t lbvenc_enhance_type;
    uint32_t size;
    uint8_t *buffer = NULL;
    int roi_x;
    int roi_y;
    
    lbvenc_enhance_type = bytestream2_get_byte(gb);

    if (lbvenc_enhance_type == 0x00) {
        size = bytestream2_get_be32(gb);
        roi_x = bytestream2_get_be32(gb);
        roi_y = bytestream2_get_be32(gb);
        printf("lbvenc_enhance_data layer1 data...size=%d roi(%d,%d)\n",size,roi_x,roi_y);
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

    } else if (lbvenc_enhance_type == 0x01) {
        printf("lbvenc_enhance_data layer2 data...\n");
        size = bytestream2_get_be32(gb);
        printf("lbvenc_enhance_data layer1 data...size=%d\n",size);
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

static int decode_nal_sei_mastering_display_info(HEVCSEIMasteringDisplay *s,
                                                 GetByteContext *gb)
{
    int i;

    if (bytestream2_get_bytes_left(gb) < 24)
        return AVERROR_INVALIDDATA;

    // Mastering primaries
    for (i = 0; i < 3; i++) {
        s->display_primaries[i][0] = bytestream2_get_be16u(gb);
        s->display_primaries[i][1] = bytestream2_get_be16u(gb);
    }
    // White point (x, y)
    s->white_point[0] = bytestream2_get_be16u(gb);
    s->white_point[1] = bytestream2_get_be16u(gb);

    // Max and min luminance of mastering display
    s->max_luminance = bytestream2_get_be32u(gb);
    s->min_luminance = bytestream2_get_be32u(gb);

    // As this SEI message comes before the first frame that references it,
    // initialize the flag to 2 and decrement on IRAP access unit so it
    // persists for the coded video sequence (e.g., between two IRAPs)
    s->present = 2;

    return 0;
}

static int decode_nal_sei_content_light_info(HEVCSEIContentLight *s,
                                             GetByteContext *gb)
{
    if (bytestream2_get_bytes_left(gb) < 4)
        return AVERROR_INVALIDDATA;

    // Max and average light levels
    s->max_content_light_level     = bytestream2_get_be16u(gb);
    s->max_pic_average_light_level = bytestream2_get_be16u(gb);
    // As this SEI message comes before the first frame that references it,
    // initialize the flag to 2 and decrement on IRAP access unit so it
    // persists for the coded video sequence (e.g., between two IRAPs)
    s->present = 2;

    return  0;
}

static int decode_nal_sei_pic_timing(HEVCSEI *s, GetBitContext *gb,
                                     const HEVCParamSets *ps, void *logctx)
{
    HEVCSEIPictureTiming *h = &s->picture_timing;
    HEVCSPS *sps;

    if (!ps->sps_list[s->active_seq_parameter_set_id])
        return(AVERROR(ENOMEM));
    sps = (HEVCSPS*)ps->sps_list[s->active_seq_parameter_set_id]->data;

    if (sps->vui.frame_field_info_present_flag) {
        int pic_struct = get_bits(gb, 4);
        h->picture_struct = AV_PICTURE_STRUCTURE_UNKNOWN;
        if (pic_struct == 2 || pic_struct == 10 || pic_struct == 12) {
            av_log(logctx, AV_LOG_DEBUG, "BOTTOM Field\n");
            h->picture_struct = AV_PICTURE_STRUCTURE_BOTTOM_FIELD;
        } else if (pic_struct == 1 || pic_struct == 9 || pic_struct == 11) {
            av_log(logctx, AV_LOG_DEBUG, "TOP Field\n");
            h->picture_struct = AV_PICTURE_STRUCTURE_TOP_FIELD;
        } else if (pic_struct == 7) {
            av_log(logctx, AV_LOG_DEBUG, "Frame/Field Doubling\n");
            h->picture_struct = HEVC_SEI_PIC_STRUCT_FRAME_DOUBLING;
        } else if (pic_struct == 8) {
            av_log(logctx, AV_LOG_DEBUG, "Frame/Field Tripling\n");
            h->picture_struct = HEVC_SEI_PIC_STRUCT_FRAME_TRIPLING;
        }
    }

    return 0;
}

static int decode_nal_sei_active_parameter_sets(HEVCSEI *s, GetBitContext *gb, void *logctx)
{
    int num_sps_ids_minus1;
    unsigned active_seq_parameter_set_id;

    get_bits(gb, 4); // active_video_parameter_set_id
    get_bits(gb, 1); // self_contained_cvs_flag
    get_bits(gb, 1); // num_sps_ids_minus1
    num_sps_ids_minus1 = get_ue_golomb_long(gb); // num_sps_ids_minus1

    if (num_sps_ids_minus1 < 0 || num_sps_ids_minus1 > 15) {
        av_log(logctx, AV_LOG_ERROR, "num_sps_ids_minus1 %d invalid\n", num_sps_ids_minus1);
        return AVERROR_INVALIDDATA;
    }

    active_seq_parameter_set_id = get_ue_golomb_long(gb);
    if (active_seq_parameter_set_id >= HEVC_MAX_SPS_COUNT) {
        av_log(logctx, AV_LOG_ERROR, "active_parameter_set_id %d invalid\n", active_seq_parameter_set_id);
        return AVERROR_INVALIDDATA;
    }
    s->active_seq_parameter_set_id = active_seq_parameter_set_id;

    return 0;
}

static int decode_nal_sei_timecode(HEVCSEITimeCode *s, GetBitContext *gb)
{
    s->num_clock_ts = get_bits(gb, 2);

    for (int i = 0; i < s->num_clock_ts; i++) {
        s->clock_timestamp_flag[i] =  get_bits(gb, 1);

        if (s->clock_timestamp_flag[i]) {
            s->units_field_based_flag[i] = get_bits(gb, 1);
            s->counting_type[i]          = get_bits(gb, 5);
            s->full_timestamp_flag[i]    = get_bits(gb, 1);
            s->discontinuity_flag[i]     = get_bits(gb, 1);
            s->cnt_dropped_flag[i]       = get_bits(gb, 1);

            s->n_frames[i]               = get_bits(gb, 9);

            if (s->full_timestamp_flag[i]) {
                s->seconds_value[i]      = av_clip(get_bits(gb, 6), 0, 59);
                s->minutes_value[i]      = av_clip(get_bits(gb, 6), 0, 59);
                s->hours_value[i]        = av_clip(get_bits(gb, 5), 0, 23);
            } else {
                s->seconds_flag[i] = get_bits(gb, 1);
                if (s->seconds_flag[i]) {
                    s->seconds_value[i] = av_clip(get_bits(gb, 6), 0, 59);
                    s->minutes_flag[i]  = get_bits(gb, 1);
                    if (s->minutes_flag[i]) {
                        s->minutes_value[i] = av_clip(get_bits(gb, 6), 0, 59);
                        s->hours_flag[i] =  get_bits(gb, 1);
                        if (s->hours_flag[i]) {
                            s->hours_value[i] = av_clip(get_bits(gb, 5), 0, 23);
                        }
                    }
                }
            }

            s->time_offset_length[i] = get_bits(gb, 5);
            if (s->time_offset_length[i] > 0) {
                s->time_offset_value[i] = get_bits_long(gb, s->time_offset_length[i]);
            }
        }
    }

    s->present = 1;
    return 0;
}

static int decode_nal_sei_prefix(GetBitContext *gb, GetByteContext *gbyte,
                                 void *logctx, HEVCSEI *s,
                                 const HEVCParamSets *ps, int type)
{
    switch (type) {
    case 256:  // Mismatched value from HM 8.1
        return decode_nal_sei_decoded_picture_hash(&s->picture_hash, gbyte);
    case SEI_TYPE_PIC_TIMING:
        return decode_nal_sei_pic_timing(s, gb, ps, logctx);
    case SEI_TYPE_MASTERING_DISPLAY_COLOUR_VOLUME:
        return decode_nal_sei_mastering_display_info(&s->mastering_display, gbyte);
    case SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO:
        return decode_nal_sei_content_light_info(&s->content_light, gbyte);
    case SEI_TYPE_ACTIVE_PARAMETER_SETS:
        return decode_nal_sei_active_parameter_sets(s, gb, logctx);
    case SEI_TYPE_TIME_CODE:
        return decode_nal_sei_timecode(&s->timecode, gb);
    default: {
        int ret = ff_h2645_sei_message_decode(&s->common, type, AV_CODEC_ID_HEVC,
                                              gb, gbyte, logctx);
        if (ret == FF_H2645_SEI_MESSAGE_UNHANDLED)
            av_log(logctx, AV_LOG_DEBUG, "Skipped PREFIX SEI %d\n", type);
        return ret;
    }
    }
}

static int decode_nal_sei_suffix(GetBitContext *gb, GetByteContext *gbyte,
                                 void *logctx, HEVCSEI *s, int type)
{
    switch (type) {
    case SEI_TYPE_DECODED_PICTURE_HASH:
        return decode_nal_sei_decoded_picture_hash(&s->picture_hash, gbyte);
    case SEI_TYPE_NUHD_LBVENC_ENHANCE_DATA:
        return decode_nal_sei_decoded_nuhd_lbvenc_enhance_data(&s->lbvenc_enhance_data, gbyte);
    default:
        av_log(logctx, AV_LOG_DEBUG, "Skipped SUFFIX SEI %d\n", type);
        return 0;
    }
}

static int decode_nal_sei_message(GetByteContext *gb, void *logctx, HEVCSEI *s,
                                  const HEVCParamSets *ps, int nal_unit_type)
{
    GetByteContext message_gbyte;
    GetBitContext message_gb;
    int payload_type = 0;
    int payload_size = 0;
    int byte = 0xFF;
    av_unused int ret;
    av_log(logctx, AV_LOG_DEBUG, "Decoding SEI\n");

    while (byte == 0xFF) {
        if (bytestream2_get_bytes_left(gb) < 2 || payload_type > INT_MAX - 255)
            return AVERROR_INVALIDDATA;
        byte          = bytestream2_get_byteu(gb);
        //av_log(logctx, AV_LOG_DEBUG, ">>byte:%d\n",byte);
        payload_type += byte;
        //av_log(logctx, AV_LOG_DEBUG, ">>payload_type:%d\n",payload_type);
    }
    av_log(logctx, AV_LOG_DEBUG, "payload_type:%d\n",payload_type);
    byte = 0xFF;
    while (byte == 0xFF) {
        //av_log(logctx, AV_LOG_DEBUG, "bytestream2_get_bytes_left(gb)=%d :: (1 + payload_size)=%d\n",bytestream2_get_bytes_left(gb),1 + payload_size);
        if (bytestream2_get_bytes_left(gb) < 1 + payload_size)
            return AVERROR_INVALIDDATA;
        byte          = bytestream2_get_byteu(gb);
        //av_log(logctx, AV_LOG_DEBUG, ">>byte:%d\n",byte);
        payload_size += byte;
        //av_log(logctx, AV_LOG_DEBUG, ">>payload_size:%d\n",payload_size);
    }
    av_log(logctx, AV_LOG_DEBUG, "payload_size:%d\n",payload_size);
    if (bytestream2_get_bytes_left(gb) < payload_size)
        return AVERROR_INVALIDDATA;
    bytestream2_init(&message_gbyte, gb->buffer, payload_size);
    ret = init_get_bits8(&message_gb, gb->buffer, payload_size);
    av_assert1(ret >= 0);
    bytestream2_skipu(gb, payload_size);
    if (nal_unit_type == HEVC_NAL_SEI_PREFIX) {
        return decode_nal_sei_prefix(&message_gb, &message_gbyte,
                                     logctx, s, ps, payload_type);
    } else { /* nal_unit_type == NAL_SEI_SUFFIX */
        return decode_nal_sei_suffix(&message_gb, &message_gbyte,
                                     logctx, s, payload_type);
    }
}

int ff_hevc_decode_nal_sei(GetBitContext *gb, void *logctx, HEVCSEI *s,
                           const HEVCParamSets *ps, enum HEVCNALUnitType type)
{
    GetByteContext gbyte;
    int ret;
    printf("ff_hevc_decode_nal_sei sei size:%d\n",get_bits_left(gb) / 8);
#if 1
    FILE *fp = fopen("testout/debug_ff_hevc_decode_nal_sei.bin","wb");
    fwrite(gb->buffer,1,get_bits_left(gb) / 8,fp);
    fclose(fp);
#endif
    av_assert1((get_bits_count(gb) % 8) == 0);
    bytestream2_init(&gbyte, gb->buffer + get_bits_count(gb) / 8,
                     get_bits_left(gb) / 8);

    do {
        ret = decode_nal_sei_message(&gbyte, logctx, s, ps, type);
        if (ret < 0)
            return ret;
        printf("decode_nal_sei_message down,left size:%d\n",bytestream2_get_bytes_left(&gbyte));    
    } while (bytestream2_get_bytes_left(&gbyte) > 0);
    return 1;
}
