/*
 * NetInt AV1 RBSP parser common code header
 * Copyright (c) 2018-2023 NetInt
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

#ifndef AVCODEC_NI_AV1_RBSP_H
#define AVCODEC_NI_AV1_RBSP_H

#include "cbs_av1.h"
#include "put_bits.h"

#define MAX_PUT_BUF_SIZE (3 * 1024 * 1024)
#define MAX_NUM_OBU_PER_FRAME 9
#define MAX_NUM_TILE_PER_FRAME 128
#define MAX_MUM_TILE_GROUP_OBU_PER_FRAME (MAX_NUM_OBU_PER_FRAME / 3)

#define OBU_HEADER_SIZE 1
#define TILE_SIZE_BYTES 4

#define NOFRAMEOBU

typedef struct AV1TileInfo {
    CodedBitstreamUnitType type[MAX_NUM_OBU_PER_FRAME];
    int unit_size[MAX_NUM_OBU_PER_FRAME];
    int obu_size[MAX_NUM_OBU_PER_FRAME];
    int num_obu;
    int width;
    int height;
    int column; //total tile number in column
    int row;    //total tile number in row
    int x;
    int y;
    int x_w;
    int y_h;
    int total_raw_data_pos;
    int num_tile_group;
    int tile_raw_data_size[MAX_MUM_TILE_GROUP_OBU_PER_FRAME];
    int tile_raw_data_pos[MAX_MUM_TILE_GROUP_OBU_PER_FRAME];
} AV1TileInfo;

void av1_bitstream_fetch(const PutBitContext *stream, AVPacket *pkt, size_t size);
void av1_bitstream_reset(PutBitContext *stream);
int av1_write_obu_header(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawOBUHeader *current);
int av1_write_sequence_header_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *current);
int av1_write_trailing_bits(CodedBitstreamContext *ctx, PutBitContext *s, int bits);
int av1_write_le32(CodedBitstreamContext *ctx, PutBitContext *s, const char *name, uint32_t value);
int av1_write_leb128(CodedBitstreamContext *ctx, PutBitContext *s, const char *name, uint64_t value);
int av1_update_obu_data_length(CodedBitstreamContext *ctx, PutBitContext *s,
        int start_pos, /*AV1RawTileData *td,*/ AV1RawOBU *obu, PutBitContext *pbc_tmp, int add_trailing_bits);
int av1_write_temporal_delimiter_obu(CodedBitstreamContext *ctx, PutBitContext *s);
#ifdef NOFRAMEOBU
int av1_write_frame_header_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *seq, AV1RawFrameHeader *current);
int av1_write_tile_group_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *seq, AV1RawTileGroup *current);
#else
int av1_write_frame_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *seq, AV1RawFrame *current);
#endif

int ni_av1_write_uvlc(CodedBitstreamContext *ctx, PutBitContext *pbc, const char *name, uint32_t value, uint32_t range_min, uint32_t range_max);

#endif /* AVCODEC_NI_AV1_RBSP_H */
