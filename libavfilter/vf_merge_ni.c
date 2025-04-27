/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2010 Baptiste Coudurier
 * Copyright (c) 2007 Bobby Bingham
 * Copyright (c) 2021 NetInt
 *
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
 * merge one video y and the other uv to a new video
 */

#include "avfilter.h"
#include "formats.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/hwcontext.h"
#include "internal.h"
#include "nifilter.h"
#include <ni_device_api.h>

typedef struct NetIntMergeContext {
    const AVClass *class;

    ni_session_context_t api_ctx;
    ni_session_data_io_t api_dst_frame;

    AVBufferRef* out_frames_ref;

    int initialized;
    int session_opened;
    int keep_alive_timeout; /* keep alive timeout setting */
    ni_scaler_params_t params;
    ni_split_context_t src_ctx;
} NetIntMergeContext;

#if !IS_FFMPEG_342_AND_ABOVE
static int config_output(AVFilterLink *outlink, AVFrame *in);
#endif

static av_cold void uninit(AVFilterContext *ctx)
{
    NetIntMergeContext *s = ctx->priv;

    if (s->api_dst_frame.data.frame.p_buffer) {
        ni_frame_buffer_free(&s->api_dst_frame.data.frame);
    }

    if (s->session_opened) {
        ni_device_session_close(&s->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear(&s->api_ctx);
    }

    av_buffer_unref(&s->out_frames_ref);
}

static int query_formats(AVFilterContext *ctx)
{
    /* We only accept hardware frames */
    static const enum AVPixelFormat pix_fmts[] =
        {AV_PIX_FMT_NI_QUAD, AV_PIX_FMT_NONE};
    AVFilterFormats *formats;

    formats = ff_make_format_list(pix_fmts);

    if (!formats)
        return AVERROR(ENOMEM);

    return ff_set_common_formats(ctx, formats);
}

static int init_out_pool(AVFilterContext *ctx)
{
    NetIntMergeContext *s = ctx->priv;
    AVHWFramesContext *out_frames_ctx;
    int pool_size = 1;

    out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;

    /* Don't check return code, this will intentionally fail */
    av_hwframe_ctx_init(s->out_frames_ref);

    if (s->api_ctx.isP2P) {
        pool_size = 1;
    }
    /* Create frame pool on device */
    return ff_ni_build_frame_pool(&s->api_ctx, out_frames_ctx->width,
                                  out_frames_ctx->height, out_frames_ctx->sw_format,
                                  pool_size);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext      *ctx = inlink->dst;
    NetIntMergeContext *s = (NetIntMergeContext *) ctx->priv;
    AVHWFramesContext    *frame_ctx;
    AVNIDeviceContext    *pAVNIDevCtx;
    AVFilterLink         *outlink;
    AVFrame              *out = NULL;
    niFrameSurface1_t    *frame_0_surface, *frame_1_surface, *new_frame_surface;
    int flags, frame_cardno;
    int frame_scaler_format;
    ni_retcode_t retcode;

    frame_ctx = (AVHWFramesContext *) frame->hw_frames_ctx->data;
    frame_scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(frame_ctx->sw_format);
    outlink = ctx->outputs[0];

    frame_cardno = ni_get_cardno(frame);
    frame_0_surface = ((niFrameSurface1_t*)(frame->buf[0]->data));
    frame_1_surface = ((niFrameSurface1_t*)(frame->buf[1]->data));
    if (s->src_ctx.h[0] == s->src_ctx.h[1] && s->src_ctx.w[0] == s->src_ctx.w[1]) {
        return ff_filter_frame(ctx->outputs[0], frame);
    }

    if (!s->initialized) {
#if !IS_FFMPEG_342_AND_ABOVE
        retcode = config_output(outlink, frame_0);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "ni merge filter config output failure\n");
            return retcode;
        }
#endif

        retcode = ni_device_session_context_init(&s->api_ctx);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "ni merge filter session context init failure\n");
            return retcode;
        }

        pAVNIDevCtx = (AVNIDeviceContext *)frame_ctx->device_ctx->hwctx;
        s->api_ctx.device_handle = pAVNIDevCtx->cards[frame_cardno];
        s->api_ctx.blk_io_handle = pAVNIDevCtx->cards[frame_cardno];

        s->api_ctx.hw_id              = frame_cardno;
        s->api_ctx.device_type        = NI_DEVICE_TYPE_SCALER;
        s->api_ctx.scaler_operation   = NI_SCALER_OPCODE_MERGE;
        s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
        s->api_ctx.isP2P              = 0;

        av_log(ctx, AV_LOG_ERROR,
                       "Open merge session to card %d, hdl %d, blk_hdl %d\n", frame_cardno,
                       s->api_ctx.device_handle, s->api_ctx.blk_io_handle);

        retcode = ni_device_session_open(&s->api_ctx, NI_DEVICE_TYPE_SCALER);
        if (retcode != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Can't open device session on card %d\n",
                   frame_cardno);
            return retcode;
        }

        s->session_opened = 1;
        if (s->params.filterblit) {
            retcode = ni_scaler_set_params(&s->api_ctx, &(s->params.filterblit));
            if (retcode < 0)
            {
                av_log(ctx, AV_LOG_ERROR,
                   "Set params error %d\n", retcode);
                return retcode;
            }
        }

        retcode = init_out_pool(inlink->dst);
        if (retcode < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Internal output allocation failed rc = %d\n", retcode);
            return retcode;
        }

        ff_ni_clone_hwframe_ctx(frame_ctx,
                                (AVHWFramesContext *)s->out_frames_ref->data,
                                &s->api_ctx);

        if (frame && frame->color_range == AVCOL_RANGE_JPEG) {
            av_log(ctx, AV_LOG_ERROR,
                   "WARNING: Full color range input, limited color output\n");
        }

        s->initialized = 1;
    }

    /* Allocate a ni_frame for the merge output */
    retcode = ni_frame_buffer_alloc_hwenc(&s->api_dst_frame.data.frame,
                                          outlink->w,
                                          outlink->h,
                                          0);

    if (retcode != NI_RETCODE_SUCCESS) {
        return AVERROR(ENOMEM);
    }

#ifdef NI_MEASURE_LATENCY
    ff_ni_update_benchmark(NULL);
#endif

    /*
     * Assign an input frame for merge picture. Send the
     * incoming hardware frame index to the scaler manager.
     */
    retcode = ni_device_alloc_frame(
        &s->api_ctx,                                             //
        FFALIGN(frame->width, 2),                //
        FFALIGN(frame->height, 2),               //
        frame_scaler_format,                                      //
        (frame_0_surface && frame_1_surface->encoding_type == 2) ? NI_SCALER_FLAG_CMP : 0,                                                       //
        FFALIGN(frame->width, 2),                //
        FFALIGN(frame->height, 2),               //
        0,                                                    //
        0,                                                    //
        frame_0_surface->ui32nodeAddress, //
        frame_0_surface->ui16FrameIdx,         //
        NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_DEBUG, "Can't assign frame for merge input %d\n",
               retcode);
        return AVERROR(ENOMEM);
    }

    /*
     * Allocate device output frame from the pool. We also send down the frame index
     * of the background frame to the scaler manager.
     */
    flags =  NI_SCALER_FLAG_IO;
    flags |= (frame_1_surface && frame_1_surface->encoding_type == 2) ? NI_SCALER_FLAG_CMP : 0;
    retcode = ni_device_alloc_frame(&s->api_ctx,                    //
                                    FFALIGN(outlink->w, 2),       //
                                    FFALIGN(outlink->h, 2),      //
                                    frame_scaler_format,             //
                                    flags,                          //
                                    FFALIGN(outlink->w, 2),       //
                                    FFALIGN(outlink->h, 2),      //
                                    0,                              // x
                                    0,                              // y
                                    frame_1_surface->ui32nodeAddress, //
                                    frame_1_surface->ui16FrameIdx,    //
                                    NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS)
    {
        av_log(ctx, AV_LOG_DEBUG, "Can't allocate frame for output %d\n",
               retcode);
        return AVERROR(ENOMEM);
    }

    /* Set the new frame index */
    retcode = ni_device_session_read_hwdesc(&s->api_ctx, &s->api_dst_frame,
                                            NI_DEVICE_TYPE_SCALER);
    if (retcode != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR,
               "Can't acquire output frame %d\n", retcode);
        av_frame_free(&out);
        return AVERROR(ENOMEM);
    }

#ifdef NI_MEASURE_LATENCY
    ff_ni_update_benchmark("ni_quadra_merge");
#endif

    out = av_frame_alloc();

    if (!out) {
        av_log(ctx, AV_LOG_ERROR, "Cannot clone frame\n");
        return AVERROR(ENOMEM);
    }

    av_frame_copy_props(out,frame);
    out->width  = outlink->w;
    out->height = outlink->h;
    out->format = AV_PIX_FMT_NI_QUAD;
#if !IS_FFMPEG_342_AND_ABOVE
        out->sample_aspect_ratio = outlink->sample_aspect_ratio;
#endif

    out->buf[0]        = av_buffer_ref(frame->buf[1]);
    out->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);
    out->data[3] = out->buf[0]->data;

    frame_1_surface = (niFrameSurface1_t *) out->data[3];
    new_frame_surface = (niFrameSurface1_t *) s->api_dst_frame.data.frame.p_data[3];
    frame_1_surface->ui16FrameIdx   = new_frame_surface->ui16FrameIdx;
    frame_1_surface->ui16session_ID = new_frame_surface->ui16session_ID;
    frame_1_surface->device_handle  = new_frame_surface->device_handle;
    frame_1_surface->output_idx     = new_frame_surface->output_idx;
    frame_1_surface->src_cpu        = new_frame_surface->src_cpu;
    frame_1_surface->dma_buf_fd     = 0;

    ff_ni_set_bit_depth_and_encoding_type(&frame_1_surface->bit_depth,
                                          &frame_1_surface->encoding_type,
                                          frame_ctx->sw_format);

    /* Remove ni-split specific assets */
    frame_1_surface->ui32nodeAddress = 0;

    frame_1_surface->ui16width = out->width;
    frame_1_surface->ui16height = out->height;
    av_log(inlink->dst, AV_LOG_DEBUG,
               "%s:IN trace ui16FrameIdx = [%d] --> out [%d]\n",
               __func__, frame_0_surface->ui16FrameIdx, frame_1_surface->ui16FrameIdx);

    //out->buf[0] = av_buffer_create(out->data[3], sizeof(niFrameSurface1_t), ff_ni_frame_free, NULL, 0);
    av_frame_free(&frame);

    return ff_filter_frame(ctx->outputs[0], out);
}

#if IS_FFMPEG_342_AND_ABOVE
static int config_input(AVFilterLink *inlink)
#else
static int config_input_0(AVFilterLink *inlink, AVFrame *in)
#endif
{
    NetIntMergeContext *s = inlink->dst->priv;
    AVHWFramesContext *in_frames_ctx;
    NIFramesContext *src_ctx;
    ni_split_context_t *p_split_ctx_src;

#if IS_FFMPEG_342_AND_ABOVE
    if (inlink->hw_frames_ctx == NULL) {
        av_log(inlink->dst, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }
    in_frames_ctx = (AVHWFramesContext *)inlink->hw_frames_ctx->data;
#else
    in_frames_ctx = (AVHWFramesContext *)in->hw_frames_ctx->data;
#endif
    if (!in_frames_ctx) {
        return AVERROR(EINVAL);
    }

    src_ctx  = in_frames_ctx->internal->priv;
    p_split_ctx_src = &src_ctx->split_ctx;
    if(!p_split_ctx_src->enabled)
    {
        av_log(inlink->dst, AV_LOG_ERROR, "There is no extra ppu output\n");
        return AVERROR(EINVAL);
    }
    memcpy(&s->src_ctx, p_split_ctx_src, sizeof(ni_split_context_t));

    if (in_frames_ctx->sw_format != AV_PIX_FMT_YUV420P &&
        in_frames_ctx->sw_format != AV_PIX_FMT_NV12 &&
        in_frames_ctx->sw_format != AV_PIX_FMT_YUV420P10LE &&
        in_frames_ctx->sw_format != AV_PIX_FMT_P010LE) {
        av_log(inlink->dst, AV_LOG_ERROR,
               "merge filter does not support this format: %s\n", av_get_pix_fmt_name(in_frames_ctx->sw_format));
        return AVERROR(EINVAL);
    }
    if((s->src_ctx.f[0] != s->src_ctx.f[1]) || (s->src_ctx.f[0] != s->src_ctx.f[1]))
    {
        av_log(inlink->dst, AV_LOG_ERROR, "The PPU0 and PPU1 must have the same format\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

#if IS_FFMPEG_342_AND_ABOVE
static int config_output(AVFilterLink *outlink)
#else
static int config_output_dummy(AVFilterLink *outlink)
{
    return 0;
}

static int config_output(AVFilterLink *outlink, AVFrame *in)
#endif
{
    AVFilterContext *ctx = outlink->src;
    NetIntMergeContext *s = ctx->priv;
    AVHWFramesContext *in_frames_ctx;
    AVHWFramesContext *out_frames_ctx;
    int ret = 0;

    outlink->w = s->src_ctx.w[1];
    outlink->h = s->src_ctx.h[1];
    outlink->sample_aspect_ratio = outlink->src->inputs[0]->sample_aspect_ratio;

#if IS_FFMPEG_342_AND_ABOVE

    in_frames_ctx = (AVHWFramesContext *)ctx->inputs[0]->hw_frames_ctx->data;
#else
    in_frames_ctx = (AVHWFramesContext *)in->hw_frames_ctx->data;
#endif
    if(s->src_ctx.h[0] == s->src_ctx.h[1] && s->src_ctx.w[0] == s->src_ctx.w[1])
    {
        s->out_frames_ref = av_buffer_ref(ctx->inputs[0]->hw_frames_ctx);
    }
    else
    {
        s->out_frames_ref = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
        if (!s->out_frames_ref)
            return AVERROR(ENOMEM);

        out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;
        out_frames_ctx->format    = AV_PIX_FMT_NI_QUAD;
        out_frames_ctx->width     = outlink->w;
        out_frames_ctx->height    = outlink->h;
        out_frames_ctx->sw_format = in_frames_ctx->sw_format;
        out_frames_ctx->initial_pool_size =
            NI_MERGE_ID; // Repurposed as identity code
    }

    av_buffer_unref(&outlink->hw_frames_ctx);
    outlink->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);
    if (!outlink->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return ret;
}

#define OFFSET(x) offsetof(NetIntMergeContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption merge_options[] = {
    { "keep_alive_timeout",
      "Specify a custom session keep alive timeout in seconds.",
        OFFSET(keep_alive_timeout),
        AV_OPT_TYPE_INT,
          {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
        NI_MIN_KEEP_ALIVE_TIMEOUT,
        NI_MAX_KEEP_ALIVE_TIMEOUT,
        FLAGS,
        "keep_alive_timeout"},
    { "filterblit", "filterblit enable", OFFSET(params.filterblit), AV_OPT_TYPE_INT, {.i64=0}, 0, 2, FLAGS },

    { NULL }
};

AVFILTER_DEFINE_CLASS(merge);

static const AVFilterPad avfilter_vf_merge_inputs[] = {
    {
        .name         = "input",
        .type         = AVMEDIA_TYPE_VIDEO,
#if IS_FFMPEG_342_AND_ABOVE
        .config_props = config_input,
#endif
        .filter_frame = filter_frame,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad avfilter_vf_merge_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
#if IS_FFMPEG_342_AND_ABOVE
        .config_props  = config_output,
#else
        .config_props = config_output_dummy,
#endif
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_merge_ni_quadra = {
    .name          = "ni_quadra_merge",
    .description   = NULL_IF_CONFIG_SMALL("NetInt Quadra merge a video source on top of the input v" NI_XCODER_REVISION),
    .uninit        = uninit,
    .priv_size     = sizeof(NetIntMergeContext),
    .priv_class    = &merge_class,
#if IS_FFMPEG_342_AND_ABOVE
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
#endif
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_merge_inputs),
    FILTER_OUTPUTS(avfilter_vf_merge_outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs        = avfilter_vf_merge_inputs,
    .outputs       = avfilter_vf_merge_outputs,
    .query_formats = query_formats,
#endif
};
