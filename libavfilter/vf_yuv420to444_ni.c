/*
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
 * yuv420 to yuv444
 */

#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/timestamp.h"
#include "avfilter.h"
#include "internal.h"
#include "drawutils.h"
#include "formats.h"
#include "framesync.h"
#include "video.h"


#define IS_FFMPEG_342_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 6) ||                                        \
     (LIBAVFILTER_VERSION_MAJOR == 6 && LIBAVFILTER_VERSION_MINOR >= 107))

#if !IS_FFMPEG_342_AND_ABOVE
enum EOFAction {
    EOF_ACTION_REPEAT,
    EOF_ACTION_ENDALL,
    EOF_ACTION_PASS
};
#endif

typedef struct YUVTransContext {
    const AVClass *class;
    FFFrameSync fs;
    int mode;
#if !IS_FFMPEG_342_AND_ABOVE
    int opt_repeatlast;
    int opt_shortest;
    int opt_eof_action;
#endif
} YUVTransContext;

static av_cold void uninit(AVFilterContext *ctx)
{
    YUVTransContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    int ret;
    enum AVPixelFormat input_pix_fmt = AV_PIX_FMT_YUV420P;
    enum AVPixelFormat output_pix_fmt = AV_PIX_FMT_YUV444P;

    if (ctx->inputs[0]) {
        formats = NULL;
        if ((ret = ff_add_format(&formats, input_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->outcfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->out_formats)) < 0)
#endif
            return ret;
    }
    if (ctx->inputs[1]) {
        formats = NULL;
        if ((ret = ff_add_format(&formats, input_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->inputs[1]->outcfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->inputs[1]->out_formats)) < 0)
#endif
            return ret;
    }
    if (ctx->outputs[0]) {
        formats = NULL;

        if ((ret = ff_add_format(&formats, output_pix_fmt)) < 0)
            return ret;
#if (LIBAVFILTER_VERSION_MAJOR >= 8 || LIBAVFILTER_VERSION_MAJOR >= 7 && LIBAVFILTER_VERSION_MINOR >= 110)
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->incfg.formats)) < 0)
#else
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->in_formats)) < 0)
#endif
            return ret;
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    YUVTransContext *s = ctx->priv;
    int i, ret;

    ret = ff_framesync_init(&s->fs, ctx, ctx->nb_inputs);
    if (ret < 0)
        return ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        FFFrameSyncIn *in = &s->fs.in[i];
        in->before    = EXT_STOP;
        in->after     = EXT_INFINITY;
        in->sync      = i ? 1 : 2;
        in->time_base = ctx->inputs[i]->time_base;
    }
 
    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->format = AV_PIX_FMT_YUV444P;
    outlink->time_base = ctx->inputs[0]->time_base;
    av_log(ctx, AV_LOG_INFO, "output w:%d h:%d fmt:%s\n",
           outlink->w, outlink->h, av_get_pix_fmt_name(outlink->format));

#if !IS_FFMPEG_342_AND_ABOVE
    if (!s->opt_repeatlast || s->opt_eof_action == EOF_ACTION_PASS) {
        s->opt_repeatlast = 0;
        s->opt_eof_action = EOF_ACTION_PASS;
    }
    if (s->opt_shortest || s->opt_eof_action == EOF_ACTION_ENDALL) {
        s->opt_shortest = 1;
        s->opt_eof_action = EOF_ACTION_ENDALL;
    }
    if (!s->opt_repeatlast) {
        for (i = 1; i < s->fs.nb_in; i++) {
            s->fs.in[i].after = EXT_NULL;
            s->fs.in[i].sync  = 0;
        }
    }
    if (s->opt_shortest) {
        for (i = 0; i < s->fs.nb_in; i++)
            s->fs.in[i].after = EXT_STOP;
    }
#endif

    return ff_framesync_configure(&s->fs);
}

static int do_blend(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    YUVTransContext *trans_ctx = ctx->priv;
    AVFrame *mainpic, *second, *out;
    int uv_420_linesize, uv_444_linesize;
    int i, j;

    ff_framesync_get_frame(fs, 0, &mainpic, 0);
    ff_framesync_get_frame(fs, 1, &second, 0);

    mainpic->pts =
        av_rescale_q(fs->pts, fs->time_base, ctx->outputs[0]->time_base);
    {
        //allocate a new buffer, data is null
        out = ff_get_video_buffer(ctx->outputs[0], ctx->outputs[0]->w, ctx->outputs[0]->h);
        if (!out) {
            return AVERROR(ENOMEM);
        }

        av_frame_copy_props(out, mainpic);
        out->format = ctx->outputs[0]->format;

        uv_420_linesize = mainpic->linesize[1];
        uv_444_linesize = out->linesize[1];

        //y compnent
        for (i = 0; i < out->height; i++) {
            memcpy(out->data[0] + i * out->linesize[0],
                   mainpic->data[0] + i * mainpic->linesize[0],
                   out->linesize[0]);
        }

        if (trans_ctx->mode == 0) {
            //u compnent
            for (i = 0; i < out->height; i++) {
                memcpy(out->data[1] + i * out->linesize[0],
                       second->data[0] + i * second->linesize[0],
                       out->linesize[0]);
            }

            //v compnent
            for (i = 0; i < out->height / 2; i++) {
                for (j = 0; j < out->width / 2; j++) {
                    memcpy(out->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                           mainpic->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + 2 * (i * uv_444_linesize) +
                               (2 * j + 1),
                           mainpic->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                               2 * j,
                           second->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                               (2 * j + 1),
                           second->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                }
            }
        } else if (trans_ctx->mode == 1) {
            // uv compnent
            for (i = 0; i < out->height / 2; i++) {
                for (j = 0; j < out->width / 2; j++) {
                    memcpy(out->data[1] + (2 * i * uv_444_linesize) + 2 * j,
                           mainpic->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[1] + (2 * i * uv_444_linesize) +
                               (2 * j + 1),
                           second->data[1] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[1] + ((2 * i + 1) * uv_444_linesize) +
                               2 * j,
                           second->data[0] + 2 * i * uv_444_linesize +
                               2 * j,
                           sizeof(char) * 2);

                    memcpy(out->data[2] + (2 * i * uv_444_linesize) + 2 * j,
                           mainpic->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + 2 * (i * uv_444_linesize) +
                               (2 * j + 1),
                           second->data[2] + i * uv_420_linesize + j,
                           sizeof(char));
                    memcpy(out->data[2] + ((2 * i + 1) * uv_444_linesize) +
                               2 * j,
                           second->data[0] + (2 * i + 1) * uv_444_linesize +
                               2 * j,
                           sizeof(char) * 2);
                }
            }
        }
    }

    return ff_filter_frame(ctx->outputs[0], out);
}

static av_cold int init(AVFilterContext *ctx)
{
    YUVTransContext *s = ctx->priv;

    s->fs.on_event = do_blend;
    s->fs.opaque = s;

    return 0;
}

#if IS_FFMPEG_342_AND_ABOVE
static int activate(AVFilterContext *ctx)
{
    YUVTransContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}
#else
static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    YUVTransContext *s = inlink->dst->priv;
    av_log(inlink->dst, AV_LOG_DEBUG, "Incoming frame (time:%s) from link #%d\n", av_ts2timestr(inpicref->pts, &inlink->time_base), FF_INLINK_IDX(inlink));
    return ff_framesync_filter_frame(&s->fs, inlink, inpicref);
}

static int request_frame(AVFilterLink *outlink)
{
    YUVTransContext *s = outlink->src->priv;
    return ff_framesync_request_frame(&s->fs, outlink);
}
#endif

#define OFFSET(x) offsetof(YUVTransContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption YUVTrans_options[] = {
    {"mode",
     "filter mode 0 have better PSNR 1 can decode as 420.",
     OFFSET(mode),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     FLAGS,
     "mode"},
#if !IS_FFMPEG_342_AND_ABOVE
    {"eof_action",
     "Action to take when encountering EOF from secondary input ",
     OFFSET(opt_eof_action),
     AV_OPT_TYPE_INT,
     { .i64 = EOF_ACTION_REPEAT },
     EOF_ACTION_REPEAT,
     EOF_ACTION_PASS,
     FLAGS},
        {"repeat",
         "Repeat the previous frame.",
         0,
         AV_OPT_TYPE_CONST,
         { .i64 = EOF_ACTION_REPEAT },
         FLAGS},
        {"endall",
         "End both streams.",
         0,
         AV_OPT_TYPE_CONST,
         { .i64 = EOF_ACTION_ENDALL },
         FLAGS},
        {"pass",
         "Pass through the main input.",
         0,
         AV_OPT_TYPE_CONST,
         { .i64 = EOF_ACTION_PASS },
         FLAGS},
    {"shortest",
     "force termination when the shortest input terminates",
     OFFSET(opt_shortest),
     AV_OPT_TYPE_BOOL,
     { .i64 = 0 },
     0,
     1,
     FLAGS},
    {"repeatlast",
     "extend last frame of secondary streams beyond EOF",
     OFFSET(opt_repeatlast),
     AV_OPT_TYPE_BOOL,
     { .i64 = 1 },
     0,
     1,
     FLAGS},
#endif
    {NULL}
};

#if IS_FFMPEG_342_AND_ABOVE
// NOLINTNEXTLINE(clang-diagnostic-deprecated-declarations)
FRAMESYNC_DEFINE_CLASS(YUVTrans, YUVTransContext, fs);
#else
AVFILTER_DEFINE_CLASS(YUVTrans);
#endif

static const AVFilterPad avfilter_vf_YUVTrans_inputs[] = {
    {
        .name         = "input0",
        .type         = AVMEDIA_TYPE_VIDEO,
#if !IS_FFMPEG_342_AND_ABOVE
        .filter_frame  = filter_frame,
#endif
    },
    {
        .name         = "input1",
        .type         = AVMEDIA_TYPE_VIDEO,
#if !IS_FFMPEG_342_AND_ABOVE
        .filter_frame  = filter_frame,
#endif
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

static const AVFilterPad avfilter_vf_YUVTrans_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
#if !IS_FFMPEG_342_AND_ABOVE
        .request_frame  = request_frame,
#endif
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    { NULL }
#endif
};

AVFilter ff_vf_yuv420to444_ni_quadra = {
    .name          = "ni_quadra_yuv420to444",
    .description   = NULL_IF_CONFIG_SMALL("NetInt Quadra YUV420 to YUV444."),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(YUVTransContext),
    .priv_class    = &YUVTrans_class,
#if IS_FFMPEG_342_AND_ABOVE
    .preinit       = YUVTrans_framesync_preinit,
    .activate      = activate,
#endif
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_YUVTrans_inputs),
    FILTER_OUTPUTS(avfilter_vf_YUVTrans_outputs),
    FILTER_QUERY_FUNC(query_formats),
#else
    .inputs        = avfilter_vf_YUVTrans_inputs,
    .outputs       = avfilter_vf_YUVTrans_outputs,
    .query_formats = query_formats,
#endif
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                     AVFILTER_FLAG_SLICE_THREADS,
};
