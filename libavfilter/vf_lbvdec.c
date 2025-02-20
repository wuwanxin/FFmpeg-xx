/*
 * Copyright (c) 2007 Bobby Bingham
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
 * video crop filter
 */

#include <stdio.h>
#include "libavcodec/get_bits.h"
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
#include "libavutil/avassert.h"
#include "mediaass/sevc_dec.h"

static const char *const var_names[] = {
    "in_w", "iw",   ///< width  of the input video
    "in_h", "ih",   ///< height of the input video
    "out_w", "ow",  ///< width  of the output decoded video
    "out_h", "oh",  ///< height of the output decoded video
    // "a",
    // "sar",
    // "dar",
    "hsub",
    "vsub",
    "x",
    "y",
    "n",            ///< number of frame
#if FF_API_FRAME_PKT
    "pos",          ///< position in the file
#endif
    "t",            ///< timestamp expressed in seconds
    NULL
};

enum var_name {
    VAR_IN_W,  VAR_IW,
    VAR_IN_H,  VAR_IH,
    VAR_OUT_W, VAR_OW,
    VAR_OUT_H, VAR_OH,
    // VAR_A,
    // VAR_SAR,
    // VAR_DAR,
    VAR_HSUB,
    VAR_VSUB,
    VAR_X,
    VAR_Y,
    VAR_N,
#if FF_API_FRAME_PKT
    VAR_POS,
#endif
    VAR_T,
    VAR_VARS_NB
};

typedef struct CropContext {
    const AVClass *class;

    int  roi_x;             ///< x offset of the roi area 
    int  roi_y;             ///< y offset of the roi area 
    int  roi_w;             ///< width of the lbvdec roi area
    int  roi_h;             ///< height of the lbvdec roi area

    int  w;                 ///< output decoded width 
    int  h;                 ///< output decoded height 

    int max_step[4];    ///< max pixel step for each plane, expressed as a number of bytes
    int hsub, vsub;     ///< chroma subsampling
    char *x_expr, *y_expr, *w_expr, *h_expr;
    AVExpr *x_pexpr, *y_pexpr;  /* parsed expressions for x and y */
    double var_values[VAR_VARS_NB];
} CropContext;

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

static void image_copy_plane(uint8_t *dst, int dst_linesize,
                         const uint8_t *src, int src_linesize,
                         int bytewidth, int height)
{
    if (!dst || !src)
        return;
    av_assert0(abs(src_linesize) >= bytewidth);
    av_assert0(abs(dst_linesize) >= bytewidth);
    for (;height > 0; height--) {
        memcpy(dst, src, bytewidth);
        dst += dst_linesize;
        src += src_linesize;
    }
}

//for YUV data, frame->data[0] save Y, frame->data[1] save U, frame->data[2] save V
static int frame_process_video(AVFilterContext *ctx,AVFrame *dst, const AVFrame *src)
{
    int i, planes;
    SEVC_DEC_PARAM_S dec_param;
    int get_roi_x = -1;
    int get_roi_y = -1;
    int lbvdec_enhance_data_size = 0;
    uint8_t *lbvdec_enhance_data = NULL;
    //printf("frame_process_video enter src(%dx%d) dst(%dx%d)\n",src->width,src->height,dst->width,dst->height);

    planes = av_pix_fmt_count_planes(dst->format);
    //make sure data is valid
    for (i = 0; i < planes; i++)
        if (!dst->data[i] || !src->data[i])
            return AVERROR(EINVAL);

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(dst->format);
    int planes_nb = 0;
    for (i = 0; i < desc->nb_components; i++)
        planes_nb = FFMAX(planes_nb, desc->comp[i].plane + 1);

    if(planes_nb < 3){
        av_log(ctx, AV_LOG_ERROR,"now only support 3plane yuv420p, but (%d) \n",planes_nb);
        return AVERROR(EINVAL);//now only support 3plane yuv420p
    } 

    //printf("frame_process_video planes_nb:%d \n",planes_nb);

    dec_param.data_in_luma = src->data[0];
    dec_param.data_in_chroma_u = src->data[1];
    dec_param.data_in_chroma_v = src->data[2];
#if 0
    static FILE *fp = NULL;
    if(!fp) fp = fopen("testout/check_vflbdec_src.yuv","wb");
    if(fp){ 
        fwrite(dec_param.data_in_luma, 1, src->width * src->height  , fp);
        fwrite(dec_param.data_in_chroma_u, 1, src->width * src->height / 4 , fp);
        fwrite(dec_param.data_in_chroma_v, 1, src->width * src->height / 4 , fp);
    }
    
#endif
    dec_param.data_out_luma = dst->data[0];
    dec_param.data_out_chroma_u = dst->data[1];
    dec_param.data_out_chroma_v = dst->data[2];

    sevc_layer1_int_dec_one_frame_with_param(dec_param);

    
    //if(src->opaque && (lbvdec_enhance_data_size>12)){
    if(src->opaque){
        lbvdec_enhance_data = (uint8_t *)src->opaque;
        get_roi_x = AV_RB32(lbvdec_enhance_data);
        get_roi_y = AV_RB32(lbvdec_enhance_data + 4);
        lbvdec_enhance_data_size = AV_RB32(lbvdec_enhance_data + 8);
        av_log(ctx, AV_LOG_DEBUG,"[nuhd]0x%08x vf get: roi(%d,%d) , size=%d \n",lbvdec_enhance_data,get_roi_x,get_roi_y,lbvdec_enhance_data_size);
#if 0//debug
        static int lbvdec_enhance_data_counnter = 0;
        char enhance_data_layer1_name[256];
        snprintf(enhance_data_layer1_name, sizeof(enhance_data_layer1_name), "testout/enhance_data_layer1_rx_%d.jpg", lbvdec_enhance_data_counnter);
        FILE *enhance_data_layer1 = fopen(enhance_data_layer1_name,"wb");
        if(enhance_data_layer1){
            fwrite(lbvdec_enhance_data + 12, 1, lbvdec_enhance_data_size , enhance_data_layer1);
            fclose(enhance_data_layer1);
        }

        char base_data_layer1_name[256];
        snprintf(base_data_layer1_name, sizeof(base_data_layer1_name), "testout/base_data_layer1_rx_%d.yuv", lbvdec_enhance_data_counnter); 
        FILE *base_data_layer1 = fopen(base_data_layer1_name,"wb");
        if(base_data_layer1){
            fwrite(src->data[0], 1, 1920 * 1088 , base_data_layer1);
            fwrite(src->data[1], 1, 1920 * 1088 / 2 / 2 , base_data_layer1);
            fwrite(src->data[2], 1, 1920 * 1088 / 2 / 2  , base_data_layer1);
            fclose(base_data_layer1);
        }
        lbvdec_enhance_data_counnter++;
#endif
        sevc_layer1_do_dec_one_frame(lbvdec_enhance_data + 12,lbvdec_enhance_data_size,get_roi_x,get_roi_y);
    }else{
        av_log(ctx, AV_LOG_DEBUG,"[nuhd] sei rx:0x%08x vf get no roi \n");
        sevc_layer1_do_dec_one_frame(NULL,0,0,0);
    }

    
    
    // for (i = 0; i < planes_nb; i++) {
    //     int h = dst->height;
    //     int bwidth = av_image_get_linesize(dst->format, dst->width, i);
    //     if (bwidth < 0) {
    //         av_log(NULL, AV_LOG_ERROR, "av_image_get_linesize failed\n");
    //         return AVERROR(EINVAL);
    //     }
    //     if (i == 1 || i == 2) {
    //         h = AV_CEIL_RSHIFT(dst->height, desc->log2_chroma_h);
    //     }
    //     image_copy_plane(dst->data[i], dst->linesize[i],
    //                         src->data[i], src->linesize[i],
    //                         bwidth, h);
    // }
    return 0;
}

/**************************************************************************
* you can modify this function, do what you want here. use src frame, and blend to dst frame.
* for this demo, we just copy some part of src frame to dst frame(out_w = in_w/2, out_h = in_h/2)
***************************************************************************/
static int do_conversion(AVFilterContext *ctx, void *arg, int jobnr,
                        int nb_jobs)
{
    CropContext *privCtx = ctx->priv;
    ThreadData *td = arg;
    AVFrame *dst = td->out;
    AVFrame *src = td->in;

    frame_process_video(ctx , dst, src);
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
#if 0
    int reject_flags = AV_PIX_FMT_FLAG_BITSTREAM | FF_PIX_FMT_FLAG_SW_FLAT_SUB;

    return ff_set_common_formats(ctx, ff_formats_pixdesc_filter(0, reject_flags));
#else
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
#endif
}

static av_cold void uninit(AVFilterContext *ctx)
{
    CropContext *s = ctx->priv;

    av_expr_free(s->x_pexpr);
    s->x_pexpr = NULL;
    av_expr_free(s->y_pexpr);
    s->y_pexpr = NULL;
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
        *n = lrint(d);

    return ret;
}

static int config_input(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    CropContext *s = ctx->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(link->format);
    int ret;
    const char *expr;
    double res;

    s->var_values[VAR_IN_W]  = s->var_values[VAR_IW] = ctx->inputs[0]->w;
    s->var_values[VAR_IN_H]  = s->var_values[VAR_IH] = ctx->inputs[0]->h;
    // s->var_values[VAR_A]     = (float) link->w / link->h;
    // s->var_values[VAR_SAR]   = link->sample_aspect_ratio.num ? av_q2d(link->sample_aspect_ratio) : 1;
    // s->var_values[VAR_DAR]   = s->var_values[VAR_A] * s->var_values[VAR_SAR];
    s->var_values[VAR_HSUB]  = 1<<pix_desc->log2_chroma_w;
    s->var_values[VAR_VSUB]  = 1<<pix_desc->log2_chroma_h;
    s->var_values[VAR_X]     = NAN;
    s->var_values[VAR_Y]     = NAN;
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = NAN;
    s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = NAN;
    s->var_values[VAR_N]     = 0;
    s->var_values[VAR_T]     = NAN;
#if FF_API_FRAME_PKT
    s->var_values[VAR_POS]   = NAN;
#endif

    av_image_fill_max_pixsteps(s->max_step, NULL, pix_desc);

    if (pix_desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        s->hsub = 1;
        s->vsub = 1;
    } else {
        s->hsub = pix_desc->log2_chroma_w;
        s->vsub = pix_desc->log2_chroma_h;
    }

    av_expr_parse_and_eval(&res, (expr = s->w_expr),
                           var_names, s->var_values,
                           NULL, NULL, NULL, NULL, NULL, 0, ctx);
    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->h_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail_expr;
    s->var_values[VAR_OUT_H] = s->var_values[VAR_OH] = res;
    /* evaluate again ow as it may depend on oh */
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr),
                                      var_names, s->var_values,
                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail_expr;

    s->var_values[VAR_OUT_W] = s->var_values[VAR_OW] = res;
    if (normalize_double(&s->w, s->var_values[VAR_OUT_W]) < 0 ||
        normalize_double(&s->h, s->var_values[VAR_OUT_H]) < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Too big value or invalid expression for out_w/ow or out_h/oh. "
               "Maybe the expression for out_w:'%s' or for out_h:'%s' is self-referencing.\n",
               s->w_expr, s->h_expr);
        return AVERROR(EINVAL);
    }

    av_expr_free(s->x_pexpr);
    av_expr_free(s->y_pexpr);
    s->x_pexpr = s->y_pexpr = NULL;
    if ((ret = av_expr_parse_and_eval(&res, s->x_expr, var_names,s->var_values,
                             NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0){
            return AVERROR(EINVAL);                    
    }
    s->var_values[VAR_X] = res;
    if  ((ret = av_expr_parse_and_eval(&res, s->y_expr, var_names,s->var_values,
                             NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0){
        return AVERROR(EINVAL);
    }
    s->var_values[VAR_Y] = res;
    if (normalize_double(&s->roi_x, s->var_values[VAR_X]) < 0 ||
        normalize_double(&s->roi_y, s->var_values[VAR_Y]) < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Maybe the expression for roi_x:'%s' or for roi_y:'%s' is self-referencing.\n",
               s->x_pexpr, s->y_pexpr);
        return AVERROR(EINVAL);
    }
    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d -> w:%d h:%d \n",
           link->w, link->h, s->w, s->h);

    if (s->w <= 0 || s->h <= 0 ||
        s->w > (((link->w+15)/16*16)<<2) || s->h > (((link->h+15)/16*16)<<2)) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid too big or non positive size for width '%d' or height '%d'\n",
               s->w, s->h);
        return AVERROR(EINVAL);
    }

    /* set default, required in the case the first computed value for x/y is NAN */
    if (s->roi_x <= 0 || s->roi_y <= 0 ||
        s->roi_x > s->w || s->h > s->h) {
        
        s->roi_x = 0;
        s->roi_y = 0;
    }

    av_log(ctx, AV_LOG_VERBOSE, "roi_x:%d roi_y:%d -> (parse)%s,%s \n",
           s->roi_x,s->roi_y,s->x_expr,s->y_expr);
    return 0;

fail_expr:
    av_log(ctx, AV_LOG_ERROR, "Error when evaluating the expression '%s'\n", expr);
    return ret;
}

static int config_output(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    CropContext *s = link->src->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(link->format);

    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        // Hardware frames adjust the cropping regions rather than
        // changing the frame size.
    } else {
        link->w = s->w;
        link->h = s->h;
    }
    //link->sample_aspect_ratio = ;//use src

    printf("config_output src(%dx%d) out(%dx%d) format(%d)\n",ctx->inputs[0]->w,ctx->inputs[0]->h,link->w,link->h,link->format);

    sevc_layer1_dec_init(ctx->inputs[0]->w,ctx->inputs[0]->h,link->w,link->h);

    return 0;
}


static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
#if 0
    AVFilterContext *ctx = link->dst;
    CropContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(link->format);
    int i;

    s->var_values[VAR_N] = link->frame_count_out;
    s->var_values[VAR_T] = frame->pts == AV_NOPTS_VALUE ?
        NAN : frame->pts * av_q2d(link->time_base);
#if FF_API_FRAME_PKT
FF_DISABLE_DEPRECATION_WARNINGS
    s->var_values[VAR_POS] = frame->pkt_pos == -1 ?
        NAN : frame->pkt_pos;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, NULL);
    /* It is necessary if x is expressed from y  */
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);

    normalize_double(&s->x, s->var_values[VAR_X]);
    normalize_double(&s->y, s->var_values[VAR_Y]);

    if (s->x < 0)
        s->x = 0;
    if (s->y < 0)
        s->y = 0;
    if ((unsigned)s->x + (unsigned)s->w > link->w)
        s->x = link->w - s->w;
    if ((unsigned)s->y + (unsigned)s->h > link->h)
        s->y = link->h - s->h;

    av_log(ctx, AV_LOG_TRACE, "n:%d t:%f x:%d y:%d x+w:%d y+h:%d\n",
            (int)s->var_values[VAR_N], s->var_values[VAR_T],
            s->x, s->y, s->x+s->w, s->y+s->h);

    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        frame->crop_top   += s->y;
        frame->crop_left  += s->x;
        frame->crop_bottom = frame->height - frame->crop_top - frame->crop_bottom - s->h;
        frame->crop_right  = frame->width  - frame->crop_left - frame->crop_right - s->w;
    } else {
        frame->width  = s->w;
        frame->height = s->h;

        frame->data[0] += s->y * frame->linesize[0];
        frame->data[0] += s->x * s->max_step[0];

        if (!(desc->flags & AV_PIX_FMT_FLAG_PAL)) {
            for (i = 1; i < 3; i ++) {
                if (frame->data[i]) {
                    frame->data[i] += (s->y >> s->vsub) * frame->linesize[i];
                    frame->data[i] += (s->x * s->max_step[i]) >> s->hsub;
                }
            }
        }

        /* alpha plane */
        if (frame->data[3]) {
            frame->data[3] += s->y * frame->linesize[3];
            frame->data[3] += s->x * s->max_step[3];
        }
    }

    return ff_filter_frame(link->dst->outputs[0], frame);
#else
    av_log(NULL, AV_LOG_WARNING, "### chenxf filter_frame, link %x, frame %x \n", link, frame);
    AVFilterContext *avctx = link->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    AVFrame *out;

    //allocate a new buffer, data is null
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

    //the new output frame, property is the same as input frame, only width/height is different
    av_frame_copy_props(out, frame);
    out->width  = outlink->w;
    out->height = outlink->h;

    ThreadData td;
    td.in = frame;
    td.out = out;
    int res;
    // if(res = avctx->internal->execute(avctx, do_conversion, &td, NULL, FFMIN(outlink->h, avctx->graph->nb_threads))) {
    //     return res;
    // }
    frame_process_video(avctx,out, frame);

    av_frame_free(&frame);

    return ff_filter_frame(outlink, out);
#endif
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    CropContext *s = ctx->priv;
    int ret;

    if (   !strcmp(cmd, "out_w")  || !strcmp(cmd, "w")
        || !strcmp(cmd, "out_h")  || !strcmp(cmd, "h")
        || !strcmp(cmd, "x")      || !strcmp(cmd, "y")) {

        int old_x = s->roi_x;
        int old_y = s->roi_y;
        int old_w = s->w;
        int old_h = s->h;

        AVFilterLink *outlink = ctx->outputs[0];
        AVFilterLink *inlink  = ctx->inputs[0];

        av_opt_set(s, cmd, args, 0);

        if ((ret = config_input(inlink)) < 0) {
            s->roi_x = old_x;
            s->roi_x = old_y;
            s->w = old_w;
            s->h = old_h;
            return ret;
        }

        ret = config_output(outlink);

    } else
        ret = AVERROR(ENOSYS);

    return ret;
}

#define OFFSET(x) offsetof(CropContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define TFLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption lbvdec_options[] = {
    { "out_w",       "set the width crop area expression",   OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, 0, 0, TFLAGS },
    { "w",           "set the width crop area expression",   OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, 0, 0, TFLAGS },
    { "out_h",       "set the height crop area expression",  OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, 0, 0, TFLAGS },
    { "h",           "set the height crop area expression",  OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, 0, 0, TFLAGS },
    { "x",           "set the roi area position x expression",       OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "(in_w-out_w)/2"}, 0, 0, TFLAGS },
    { "y",           "set the roi area position y expression",       OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "(in_h-out_h)/2"}, 0, 0, TFLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(lbvdec);


static const AVFilterPad avfilter_vf_crop_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
};

static const AVFilterPad avfilter_vf_crop_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

const AVFilter ff_vf_lbvdec = {
    .name            = "lbvdec",
    .description     = NULL_IF_CONFIG_SMALL("lbvdec process the input video."),
    .priv_size       = sizeof(CropContext),
    .priv_class      = &lbvdec_class,
    .uninit          = uninit,
    FILTER_INPUTS(avfilter_vf_crop_inputs),
    FILTER_OUTPUTS(avfilter_vf_crop_outputs),
    FILTER_QUERY_FUNC(query_formats),
    .process_command = process_command,
};
