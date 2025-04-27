/*
 * NetInt AV1 tile rawtotile common source code
 * Copyright (c) 2023 NetInt
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
 * This bitstream filter splits AV1 Temporal Units into packets containing
 * just one frame, plus any leading and trailing OBUs that may be present at
 * the beginning or end, respectively.
 *
 * Temporal Units already containing only one frame will be passed through
 * unchanged. When splitting can't be performed, the Temporal Unit will be
 * passed through containing only the remaining OBUs starting from the first
 * one after the last successfully split frame.
 */

#include "libavutil/opt.h"

#include "avcodec.h"
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 91)
#include "bsf_internal.h"
#else
#include "bsf.h"
#endif
#include "cbs.h"
#include "cbs_av1.h"
#include "ni_av1_rbsp.h"


typedef struct AV1FtoTileContext {
    AVPacket *buffer_pkt;
    CodedBitstreamContext *cbc;
    CodedBitstreamFragment temporal_unit;

    int width;
    int height;
    int column; //total tile number in column
    int row;    //total tile number in row
    int x;
    int y;
    int x_w;
    int y_h;

    int nb_frames;
    int cur_frame;
    int cur_frame_idx;
    int last_frame_idx;
} AV1FtoTileContext;

// called by raw_to_tile_bsf
static int av1_rawtotile_filter(AVBSFContext *ctx, AVPacket *out)
{
    AV1FtoTileContext *s = ctx->priv_data;
    CodedBitstreamFragment *td = &s->temporal_unit;
    int i, ret;
    AV1RawOBU *obu;
    int out_size = 0;
    AV1TileInfo tileinfo = {0};
    AVPacket *pkt_in = s->buffer_pkt;

    av_log(ctx, AV_LOG_DEBUG, "### %s line %d %s: width %d height %d c %d r %d x %d y %d nb_number %d cur_frame %d %d %d\n",
            __FILE__, __LINE__, __func__,
            s->width, s->height, s->column, s->row, s->x, s->y, s->nb_frames, s->cur_frame, s->cur_frame_idx, s->last_frame_idx);

    tileinfo.width = s->width;
    tileinfo.height = s->height;
    tileinfo.column = s->column;
    tileinfo.row = s->row;
    tileinfo.x = s->x;
    tileinfo.y = s->y;
    tileinfo.x_w = s->x_w;
    tileinfo.y_h = s->y_h;

    if (!s->buffer_pkt->data) {
        ret = ff_bsf_get_packet_ref(ctx, s->buffer_pkt);
        if (ret < 0)
            return ret;

        // read headers and save in ctx->priv_data in cbs_av1_read_unit()
        ret = ff_cbs_read_packet(s->cbc, td, s->buffer_pkt);
        if (ret < 0) {
            av_log(ctx, AV_LOG_INFO, "Failed to parse temporal unit.\n");
            goto passthrough;
        }

        av_log(ctx, AV_LOG_DEBUG, "### %s line %d %s: nb_units %d pkt_in->size %d sizeof AV1TileInfo %ld\n",
                __FILE__, __LINE__, __func__, td->nb_units, pkt_in->size, sizeof(AV1TileInfo));

        tileinfo.num_obu = td->nb_units;
        for (i = 0; i < td->nb_units; i++) {
            CodedBitstreamUnit *unit = &td->units[i];
            obu = (AV1RawOBU *) unit->content;

            tileinfo.type[i] = unit->type;
            tileinfo.unit_size[i] = unit->data_size;
            tileinfo.obu_size[i] = obu->obu_size;

            if(unit->type == AV1_OBU_TILE_GROUP) {
                tileinfo.tile_raw_data_size[tileinfo.num_tile_group] = obu->obu_size;
                tileinfo.tile_raw_data_pos[tileinfo.num_tile_group] = (unit->data_size - obu->obu_size) + tileinfo.total_raw_data_pos;
                tileinfo.num_tile_group++;
            }
            tileinfo.total_raw_data_pos += unit->data_size;

            av_log(ctx, AV_LOG_DEBUG, "### %s line %d %s: unit %d type %d unit_size %ld obu_size %ld raw_data_pos %d\n",
                __FILE__, __LINE__, __func__, i, unit->type, unit->data_size, obu->obu_size, tileinfo.tile_raw_data_pos[0]);
        }
    }

    out_size = pkt_in->size + sizeof(AV1TileInfo);
    ret = av_new_packet(out, out_size);
    if (ret < 0) {
        return ret;
    }

    av_packet_copy_props(out, s->buffer_pkt);

    // out data structure is AV1TileInfo + AV1 frame data
    memcpy(out->data, (uint8_t *)&tileinfo, sizeof(AV1TileInfo));

    memcpy(out->data + sizeof(AV1TileInfo), s->buffer_pkt->data, pkt_in->size);
    out->size = pkt_in->size + sizeof(AV1TileInfo);

passthrough:
    av_packet_unref(s->buffer_pkt);
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_reset(td);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_reset(s->cbc, td);
#else
    ff_cbs_fragment_uninit(s->cbc, td);
#endif
    return 0;
}

static const CodedBitstreamUnitType decompose_unit_types[] = {
    AV1_OBU_TEMPORAL_DELIMITER,
    AV1_OBU_SEQUENCE_HEADER,
    AV1_OBU_FRAME_HEADER,
    AV1_OBU_TILE_GROUP,
    AV1_OBU_FRAME,
};

static int av1_rawtotile_init(AVBSFContext *ctx)
{
    AV1FtoTileContext *s = ctx->priv_data;
    CodedBitstreamFragment *td = &s->temporal_unit;
    void *pb_buf;
    int ret;

    s->buffer_pkt = av_packet_alloc();
    if (!s->buffer_pkt)
        return AVERROR(ENOMEM);

    ret = ff_cbs_init(&s->cbc, AV_CODEC_ID_AV1, ctx);
    if (ret < 0)
        return ret;

    s->cbc->decompose_unit_types    = (CodedBitstreamUnitType*)decompose_unit_types;
    s->cbc->nb_decompose_unit_types = FF_ARRAY_ELEMS(decompose_unit_types);

#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_reset(td);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_reset(s->cbc, td);
#else
    ff_cbs_fragment_uninit(s->cbc, td);
#endif

    pb_buf = av_mallocz(MAX_PUT_BUF_SIZE);
    if (!pb_buf) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

static void av1_rawtotile_flush(AVBSFContext *ctx)
{
    AV1FtoTileContext *s = ctx->priv_data;

    av_packet_unref(s->buffer_pkt);

#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_reset(&s->temporal_unit);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_reset(s->cbc, &s->temporal_unit);
#else
    ff_cbs_fragment_uninit(s->cbc, &s->temporal_unit);
#endif
}

static void av1_rawtotile_close(AVBSFContext *ctx)
{
    AV1FtoTileContext *s = ctx->priv_data;

    av_packet_free(&s->buffer_pkt);
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
    ff_cbs_fragment_free(&s->temporal_unit);
#elif (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 54)
    ff_cbs_fragment_free(s->cbc, &s->temporal_unit);
#else
    ff_cbs_fragment_uninit(s->cbc, &s->temporal_unit);
#endif
    ff_cbs_close(&s->cbc);
}

static const enum AVCodecID av1_rawtotile_codec_ids[] = {
    AV_CODEC_ID_AV1, AV_CODEC_ID_NONE,
};

#define OFFSET(x) offsetof(AV1FtoTileContext, x)

static const AVOption options[] = {
        {"width", "set width", OFFSET(width), AV_OPT_TYPE_INT, {.i64 = 1280}, 0, 8192, 0, 0},
        {"height", "set height", OFFSET(height), AV_OPT_TYPE_INT, {.i64 = 720}, 0, 8192, 0, 0},
        {"column", "set column", OFFSET(column), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 128, 0, 0},  //support 128 columns max
        {"row", "set row", OFFSET(row), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 128, 0, 0},  //support 128 rows max
        {"x", "set x", OFFSET(x), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 8192, 0, 0},  //support 8192 columns max
        {"y", "set y", OFFSET(y), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 8192, 0, 0},  //support 8192 rows max
        {"x_w", "set x_w", OFFSET(x_w), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 8192, 0, 0},  //support 8192 columns max
        {"y_h", "set y_h", OFFSET(y_h), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 8192, 0, 0},  //support 8192 rows max
        { NULL },
};

static const AVClass av1_rawtotile_class = {
        .class_name = "av1_rawtotile",
        .item_name  = av_default_item_name,
        .option     = options,
        .version    = LIBAVUTIL_VERSION_INT,
};

const AVBitStreamFilter ff_av1_rawtotile_bsf = {
    .name           = "av1_rawtotile",
    .priv_data_size = sizeof(AV1FtoTileContext),
	.priv_class     = &av1_rawtotile_class,
    .init           = av1_rawtotile_init,
    .flush          = av1_rawtotile_flush,
    .close          = av1_rawtotile_close,
    .filter         = av1_rawtotile_filter,
    .codec_ids      = av1_rawtotile_codec_ids,
};
