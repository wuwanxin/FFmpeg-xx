/*
 * XCoder Filter Lib Wrapper
 *
 * Copyright (c) 2020 NetInt
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
 * XCoder codec lib wrapper.
 */

#ifndef AVFILTER_NIFILTER_H
#define AVFILTER_NIFILTER_H

#include "version.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_ni_quad.h"

#include <ni_device_api.h>

#define IS_FFMPEG_342_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 6) ||                                        \
     (LIBAVFILTER_VERSION_MAJOR == 6 && LIBAVFILTER_VERSION_MINOR >= 107))

#define IS_FFMPEG_421_AND_ABOVE                                                \
    ((LIBAVFILTER_VERSION_MAJOR > 7) ||                                        \
     (LIBAVFILTER_VERSION_MAJOR == 7 && LIBAVFILTER_VERSION_MINOR >= 57))

#define DEFAULT_NI_FILTER_POOL_SIZE     4

#define FRAMESYNC_OPTIONS                                                                                                                      \
    { "eof_action", "Action to take when encountering EOF from secondary input ",                                                              \
        OFFSET(opt_eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT },                                                                 \
        EOF_ACTION_REPEAT, EOF_ACTION_PASS, .flags = FLAGS, "eof_action" },                                                                    \
        { "repeat", "Repeat the previous frame.",   0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, "eof_action" },        \
        { "endall", "End both streams.",            0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, "eof_action" },        \
        { "pass",   "Pass through the main input.", 0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_PASS },   .flags = FLAGS, "eof_action" },        \
    { "shortest", "force termination when the shortest input terminates", OFFSET(opt_shortest), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, FLAGS }, \
    { "repeatlast", "extend last frame of secondary streams beyond EOF", OFFSET(opt_repeatlast), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS }

#if !IS_FFMPEG_342_AND_ABOVE
enum EOFAction {
    EOF_ACTION_REPEAT,
    EOF_ACTION_ENDALL,
    EOF_ACTION_PASS
};
#endif

void ff_ni_update_benchmark(const char *fmt, ...);
int ff_ni_ffmpeg_to_gc620_pix_fmt(enum AVPixelFormat pix_fmt);
int ff_ni_ffmpeg_to_libxcoder_pix_fmt(enum AVPixelFormat pix_fmt);
int ff_ni_copy_device_to_host_frame(AVFrame *dst, const ni_frame_t *src, int pix_fmt);
int ff_ni_copy_host_to_device_frame(ni_frame_t *dst, const AVFrame *src, int pix_fmt);
int ff_ni_build_frame_pool(ni_session_context_t *ctx,int width,int height, enum AVPixelFormat out_format,int pool_size);
void ff_ni_frame_free(void *opaque, uint8_t *data);
void ff_ni_clone_hwframe_ctx(AVHWFramesContext *in_frames_ctx,
                             AVHWFramesContext *out_frames_ctx,
                             ni_session_context_t *ctx);
void ff_ni_set_bit_depth_and_encoding_type(int8_t *p_bit_depth,
                                           int8_t *p_enc_type,
                                           enum AVPixelFormat pix_fmt);

#endif
