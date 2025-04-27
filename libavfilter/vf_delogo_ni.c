/*
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
 * video delogo filter
 */

#include <stdio.h>

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "nifilter.h"
#include <ni_device_api.h>

static const char * const var_names[] = {
    "x",
    "y",
    "w",
    "h",
    "n",            ///< number of frame
    "t",            ///< timestamp expressed in seconds
    NULL
};

enum var_name {
    VAR_X,
    VAR_Y,
    VAR_W,
    VAR_H,
    VAR_N,
    VAR_T,
    VAR_VARS_NB
};


typedef struct NetIntDelogoContext {
    const AVClass *class;
    int x;             ///< x offset of the delogo area with respect to the input area
    int y;             ///< y offset of the delogo area with respect to the input area
    int w;             ///< width of the delogo area
    int h;             ///< height of the delogo area

    char *x_expr, *y_expr, *w_expr, *h_expr;
    AVExpr *x_pexpr, *y_pexpr;  /* parsed expressions for x and y */
    AVExpr *w_pexpr, *h_pexpr; /* parsed expressions for width and height */
    double var_values[VAR_VARS_NB];

    AVBufferRef *out_frames_ref;

    ni_session_context_t api_ctx;
    ni_session_data_io_t api_dst_frame;

    int initialized;
    int session_opened;
    int keep_alive_timeout; /* keep alive timeout setting */
} NetIntDelogoContext;

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = NULL;

    if (*pexpr)
        old = *pexpr;
    ret = av_expr_parse(pexpr, expr, var_names, NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when parsing the expression '%s' for %s\n",
               expr, option);
        *pexpr = old;
        return ret;
    }

    av_expr_free(old);
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] =
        {AV_PIX_FMT_NI_QUAD, AV_PIX_FMT_NONE};
    AVFilterFormats *formats;

    formats = ff_make_format_list(pix_fmts);

    if (!formats)
        return AVERROR(ENOMEM);

    return ff_set_common_formats(ctx, formats);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    NetIntDelogoContext *s = ctx->priv;

    av_expr_free(s->x_pexpr);
    s->x_pexpr = NULL;
    av_expr_free(s->y_pexpr);
    s->y_pexpr = NULL;
    av_expr_free(s->w_pexpr);
    s->w_pexpr = NULL;
    av_expr_free(s->h_pexpr);
    s->h_pexpr = NULL;

    if (s->api_dst_frame.data.frame.p_buffer)
        ni_frame_buffer_free(&s->api_dst_frame.data.frame);

    if (s->session_opened) {
        /* Close operation will free the device frames */
        ni_device_session_close(&s->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear(&s->api_ctx);
    }

    av_buffer_unref(&s->out_frames_ref);
}

static inline int normalize_double(int *n, double d)
{
    int ret = 0;

    if (isnan(d)) {
        ret = AVERROR(EINVAL);
    } else if (d > INT_MAX || d < INT_MIN) {
        *n = d > INT_MAX ? INT_MAX : INT_MIN;
        ret = AVERROR(EINVAL);
    } else
        *n = (int)lrint(d);

    return ret;
}

#if IS_FFMPEG_342_AND_ABOVE
static int config_input(AVFilterLink *link)
#else
static int config_input(AVFilterLink *link, AVFrame *in)
#endif
{
    AVFilterContext *ctx = link->dst;
    if (link->hw_frames_ctx == NULL) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }
    NetIntDelogoContext *s = ctx->priv;
    int ret;

    if ((ret = set_expr(&s->x_pexpr, s->x_expr, "x", ctx)) < 0 ||
        (ret = set_expr(&s->y_pexpr, s->y_expr, "y", ctx)) < 0 ||
        (ret = set_expr(&s->w_pexpr, s->w_expr, "w", ctx)) < 0 ||
        (ret = set_expr(&s->h_pexpr, s->h_expr, "h", ctx)) < 0 )
        return ret;

    s->x = av_expr_eval(s->x_pexpr, s->var_values, s);
    s->y = av_expr_eval(s->y_pexpr, s->var_values, s);
    s->w = av_expr_eval(s->w_pexpr, s->var_values, s);
    s->h = av_expr_eval(s->h_pexpr, s->var_values, s);
    s->x = FFALIGN(s->x,2);
    s->y = FFALIGN(s->y,2);
    s->w = FFALIGN(s->w,2);
    s->h = FFALIGN(s->h,2);

    if (s->x < 0 || s->y < 0 ||
        s->x >= link->w || s->h >= link->h) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid negtive value for x '%d' or y '%d'\n",
               s->x, s->y);
        return AVERROR(EINVAL);
    }
    if (s->w <= 0 || s->h <= 0 ||
        s->w > link->w || s->h > link->h) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid too big or non positive size for width '%d' or height '%d'\n",
               s->w, s->h);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int init_out_pool(AVFilterContext *ctx)
{
    NetIntDelogoContext *s = ctx->priv;
    AVHWFramesContext *out_frames_ctx;
    int pool_size = DEFAULT_NI_FILTER_POOL_SIZE;

    out_frames_ctx = (AVHWFramesContext*)s->out_frames_ref->data;

    /* Don't check return code, this will intentionally fail */
    av_hwframe_ctx_init(s->out_frames_ref);

    if (s->api_ctx.isP2P) {
        pool_size = 1;
    }
    /* Create frame pool on device */
    return ff_ni_build_frame_pool(&s->api_ctx, out_frames_ctx->width,
                                  out_frames_ctx->height,
                                  out_frames_ctx->sw_format, pool_size);
}

#if IS_FFMPEG_342_AND_ABOVE
static int config_output(AVFilterLink *link)
#else
static int config_output(AVFilterLink *link, AVFrame *in)
#endif
{
    NetIntDelogoContext *s = link->src->priv;
    AVHWFramesContext *in_frames_ctx;
    AVHWFramesContext *out_frames_ctx;
    AVFilterContext *ctx;

    ctx           = (AVFilterContext *)link->src;
#if IS_FFMPEG_342_AND_ABOVE
    in_frames_ctx = (AVHWFramesContext *)ctx->inputs[0]->hw_frames_ctx->data;
#else
    in_frames_ctx = (AVHWFramesContext *)in->hw_frames_ctx->data;
#endif
    link->w = in_frames_ctx->width;
    link->h = in_frames_ctx->height;
    link->w = FFALIGN(link->w,2);
    link->h = FFALIGN(link->h,2);

    if (in_frames_ctx->sw_format == AV_PIX_FMT_BGRP) {
        av_log(ctx, AV_LOG_ERROR, "bgrp not supported\n");
        return AVERROR(EINVAL);
    }
    if (in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
        in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
        av_log(ctx, AV_LOG_ERROR, "tile4x4 not supported\n");
        return AVERROR(EINVAL);
    }

    s->out_frames_ref = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
    if (!s->out_frames_ref)
        return AVERROR(ENOMEM);

    out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;

    out_frames_ctx->format    = AV_PIX_FMT_NI_QUAD;
    out_frames_ctx->width     = link->w;
    out_frames_ctx->height    = link->h;
    out_frames_ctx->sw_format = in_frames_ctx->sw_format;
    out_frames_ctx->initial_pool_size =
        NI_DELOGO_ID; // Repurposed as identity code

    av_buffer_unref(&link->hw_frames_ctx);

    link->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);
    if (!link->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
    NetIntDelogoContext *s = ctx->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFrame *out = NULL;
    niFrameSurface1_t* frame_surface,*new_frame_surface;
    AVHWFramesContext *pAVHFWCtx;
    AVNIDeviceContext *pAVNIDevCtx;
    ni_retcode_t retcode;
    uint32_t scaler_format;
    int cardno;
    uint16_t tempFID;

    pAVHFWCtx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    if (!pAVHFWCtx) {
	return AVERROR(EINVAL);
    }

    pAVNIDevCtx = (AVNIDeviceContext *)pAVHFWCtx->device_ctx->hwctx;
    if (!pAVNIDevCtx) {
        return AVERROR(EINVAL);
    }

    cardno = ni_get_cardno(frame);

    if (!s->initialized) {
#if !IS_FFMPEG_342_AND_ABOVE
        retcode = config_input(link, frame);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR, "ni delogo filter config input failure\n");
            return retcode;
        }

        retcode = config_output(outlink, frame);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR, "ni delogo filter config output failure\n");
            return retcode;
        }
#endif

        retcode = ni_device_session_context_init(&s->api_ctx);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "ni delogo filter session context init failure\n");
            goto fail;
        }

        s->api_ctx.device_handle = pAVNIDevCtx->cards[cardno];
        s->api_ctx.blk_io_handle = pAVNIDevCtx->cards[cardno];

        s->api_ctx.hw_id             = cardno;
        s->api_ctx.device_type       = NI_DEVICE_TYPE_SCALER;
        s->api_ctx.scaler_operation  = NI_SCALER_OPCODE_DELOGO;
        s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
        s->api_ctx.isP2P              = 0;

        retcode = ni_device_session_open(&s->api_ctx, NI_DEVICE_TYPE_SCALER);
        if (retcode != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Can't open device session on card %d\n",
                   cardno);
            goto fail;
        }

        s->session_opened = 1;

        retcode = init_out_pool(ctx);
        if (retcode < 0)
        {
            av_log(ctx, AV_LOG_ERROR,
                   "Internal output allocation failed rc = %d\n", retcode);
            goto fail;
        }

        ff_ni_clone_hwframe_ctx(pAVHFWCtx,
                                (AVHWFramesContext *)s->out_frames_ref->data,
                                &s->api_ctx);

        if (frame->color_range == AVCOL_RANGE_JPEG) {
            av_log(ctx, AV_LOG_ERROR,
                   "WARNING: Full color range input, limited color output\n");
        }

        s->initialized = 1;
    }

#if IS_FFMPEG_342_AND_ABOVE
    s->var_values[VAR_N] = link->frame_count_out;
#else
    s->var_values[VAR_N] = link->frame_count;
#endif

    if ((unsigned)s->x + (unsigned)s->w > link->w)
    {
        s->x = link->w - s->w;
        s->x = FFALIGN(s->x,2);
    }
    if ((unsigned)s->y + (unsigned)s->h > link->h)
    {
        s->y = link->h - s->h;
        s->y = FFALIGN(s->y,2);
    }

    av_log(ctx, AV_LOG_TRACE, "n:%d t:%f x:%d y:%d w:%d h:%d\n",
            (int)s->var_values[VAR_N], s->var_values[VAR_T],
            s->x, s->y, s->w, s->h);

    frame_surface = (niFrameSurface1_t *) frame->data[3];
    if (frame_surface == NULL) {
        return AVERROR(EINVAL);
    }

    scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(pAVHFWCtx->sw_format);

    retcode = ni_frame_buffer_alloc_hwenc(&s->api_dst_frame.data.frame,
                                          outlink->w,
                                          outlink->h,
                                          0);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

#ifdef NI_MEASURE_LATENCY
    ff_ni_update_benchmark(NULL);
#endif

    /*
     * Allocate device input frame. This call won't actually allocate a frame,
     * but sends the incoming hardware frame index to the scaler manager
     */
    retcode = ni_device_alloc_frame(&s->api_ctx,               //
                                    FFALIGN(frame->width, 2),  //
                                    FFALIGN(frame->height, 2), //
                                    scaler_format,             //
                                    0,                         // input frame
                                    s->w, // src rectangle width
                                    s->h, // src rectangle height
                                    s->x, // src rectangle x
                                    s->y, // src rectangle y
                                    frame_surface->ui32nodeAddress,
                                    frame_surface->ui16FrameIdx,
                                    NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_DEBUG, "Can't assign input frame %d\n",
               retcode);
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    /* Allocate device destination frame This will acquire a frame from the pool */
    retcode = ni_device_alloc_frame(&s->api_ctx,
                        FFALIGN(outlink->w,2),
                        FFALIGN(outlink->h,2),
                        scaler_format,
                        NI_SCALER_FLAG_IO,
                        0,
                        0,
                        0,
                        0,
                        0,
                        -1,
                        NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_DEBUG, "Can't allocate device output frame %d\n",
               retcode);

        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    out = av_frame_alloc();
    if (!out)
    {
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    av_frame_copy_props(out,frame);

    out->width  = outlink->w;
    out->height = outlink->h;

    out->format = AV_PIX_FMT_NI_QUAD;

    /* Quadra 2D engine always outputs limited color range */
    out->color_range = AVCOL_RANGE_MPEG;

    /* Reference the new hw frames context */
    out->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);

    out->data[3] = av_malloc(sizeof(niFrameSurface1_t));

    if (!out->data[3])
    {
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

    /* Copy the frame surface from the incoming frame */
    memcpy(out->data[3], frame->data[3], sizeof(niFrameSurface1_t));

    /* Set the new frame index */
    retcode = ni_device_session_read_hwdesc(&s->api_ctx, &s->api_dst_frame,
                                            NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR,
               "Can't acquire output frame %d\n",retcode);
        retcode = AVERROR(ENOMEM);
        goto fail;
    }

#ifdef NI_MEASURE_LATENCY
    ff_ni_update_benchmark("ni_quadra_delogo");
#endif

    tempFID           = frame_surface->ui16FrameIdx;
    frame_surface = (niFrameSurface1_t *) out->data[3];
    new_frame_surface = (niFrameSurface1_t *) s->api_dst_frame.data.frame.p_data[3];
    frame_surface->ui16FrameIdx   = new_frame_surface->ui16FrameIdx;
    frame_surface->ui16session_ID = new_frame_surface->ui16session_ID;
    frame_surface->device_handle  = pAVNIDevCtx->cards[cardno];
    frame_surface->output_idx     = new_frame_surface->output_idx;
    frame_surface->src_cpu        = new_frame_surface->src_cpu;
    frame_surface->dma_buf_fd     = 0;

    ff_ni_set_bit_depth_and_encoding_type(&frame_surface->bit_depth,
                                          &frame_surface->encoding_type,
                                          pAVHFWCtx->sw_format);

    /* Remove ni-split specific assets */
    frame_surface->ui32nodeAddress = 0;

    frame_surface->ui16width = out->width;
    frame_surface->ui16height = out->height;

    av_log(ctx, AV_LOG_DEBUG,
           "vf_delogo_ni.c:IN trace ui16FrameIdx = [%d] --> out = [%d] \n",
           tempFID, frame_surface->ui16FrameIdx);

    out->buf[0] = av_buffer_create(out->data[3], sizeof(niFrameSurface1_t), ff_ni_frame_free, NULL, 0);

    av_frame_free(&frame);

    return ff_filter_frame(link->dst->outputs[0], out);

fail:
    av_frame_free(&frame);
    av_frame_free(&out);
    return retcode;
}

#define OFFSET(x) offsetof(NetIntDelogoContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption delogo_options[] = {
    { "x",           "set the x delogo area expression",       OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "y",           "set the y delogo area expression",       OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "w",           "set the width delogo area expression",   OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "h",           "set the height delogo area expression",  OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, CHAR_MIN, CHAR_MAX, FLAGS },
    {"keep_alive_timeout",
     "Specify a custom session keep alive timeout in seconds.",
     OFFSET(keep_alive_timeout),
     AV_OPT_TYPE_INT,
     {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
     NI_MIN_KEEP_ALIVE_TIMEOUT,
     NI_MAX_KEEP_ALIVE_TIMEOUT,
     FLAGS,
     "keep_alive_timeout"},
    { NULL }
};

AVFILTER_DEFINE_CLASS(delogo);

static const AVFilterPad avfilter_vf_delogo_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
#if IS_FFMPEG_342_AND_ABOVE
        .config_props = config_input,
#endif
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad avfilter_vf_delogo_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
#if IS_FFMPEG_342_AND_ABOVE
        .config_props = config_output,
#endif
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_delogo_ni_quadra = {
    .name            = "ni_quadra_delogo",
    .description     = NULL_IF_CONFIG_SMALL("NetInt Quadra delogo the input video v" NI_XCODER_REVISION),
    .priv_size       = sizeof(NetIntDelogoContext),
    .priv_class      = &delogo_class,
    .uninit          = uninit,
#if IS_FFMPEG_342_AND_ABOVE
    .flags_internal  = FF_FILTER_FLAG_HWFRAME_AWARE,
#endif
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_delogo_inputs),
    FILTER_OUTPUTS(avfilter_vf_delogo_outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs          = avfilter_vf_delogo_inputs,
    .outputs         = avfilter_vf_delogo_outputs,
    .query_formats   = query_formats,
#endif
};
