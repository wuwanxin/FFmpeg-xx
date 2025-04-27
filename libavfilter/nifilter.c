/*
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
 * video common filter routines
 */

#include <stdio.h>

#include <ni_device_api.h>

#include "avfilter.h"
#include "nifilter.h"
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
#include "libavutil/time.h"

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif


typedef struct BenchmarkTimeStamps {
    int64_t real_usec;
    int64_t user_usec;
    int64_t sys_usec;
} BenchmarkTimeStamps;

typedef struct gc620_pixel_fmts {
    enum AVPixelFormat pix_fmt_ffmpeg;
    int                pix_fmt_gc620;
    ni_pix_fmt_t       pix_fmt_libxcoder;
} gc620_pixel_fmts_t;

static struct gc620_pixel_fmts gc620_pixel_fmt_list[] = {
    {AV_PIX_FMT_NV12, GC620_NV12, NI_PIX_FMT_NV12},
    {AV_PIX_FMT_NV21, GC620_NV21, NI_PIX_FMT_NONE},
    {AV_PIX_FMT_YUV420P, GC620_I420, NI_PIX_FMT_YUV420P},
    {AV_PIX_FMT_P010LE, GC620_P010_MSB, NI_PIX_FMT_P010LE},
    {AV_PIX_FMT_YUV420P10LE, GC620_I010, NI_PIX_FMT_YUV420P10LE},
    {AV_PIX_FMT_YUYV422, GC620_YUYV, NI_PIX_FMT_YUYV422},
    {AV_PIX_FMT_UYVY422, GC620_UYVY, NI_PIX_FMT_UYVY422},
    {AV_PIX_FMT_NV16, GC620_NV16, NI_PIX_FMT_NONE},
    {AV_PIX_FMT_RGBA, GC620_RGBA8888, NI_PIX_FMT_RGBA},
    {AV_PIX_FMT_BGR0, GC620_BGRX8888, NI_PIX_FMT_BGR0},
    {AV_PIX_FMT_BGRA, GC620_BGRA8888, NI_PIX_FMT_BGRA},
    {AV_PIX_FMT_ABGR, GC620_ABGR8888, NI_PIX_FMT_ABGR},
    {AV_PIX_FMT_ARGB, GC620_ARGB8888, NI_PIX_FMT_ARGB},
    {AV_PIX_FMT_BGR565LE, GC620_RGB565, NI_PIX_FMT_NONE},
    {AV_PIX_FMT_RGB565LE, GC620_BGR565, NI_PIX_FMT_NONE},
    {AV_PIX_FMT_RGB555LE, GC620_B5G5R5X1, NI_PIX_FMT_NONE},
    {AV_PIX_FMT_BGRP, GC620_RGB888_PLANAR, NI_PIX_FMT_BGRP},
    {AV_PIX_FMT_NI_QUAD_8_TILE_4X4, GC620_NV12, NI_PIX_FMT_NV12}
};

static BenchmarkTimeStamps get_benchmark_time_stamps(void)
{
    BenchmarkTimeStamps time_stamps = { av_gettime_relative() };
#if HAVE_GETRUSAGE
    struct rusage rusage;

    getrusage(RUSAGE_SELF, &rusage);
    time_stamps.user_usec =
        (rusage.ru_utime.tv_sec * 1000000LL) + rusage.ru_utime.tv_usec;
    time_stamps.sys_usec =
        (rusage.ru_stime.tv_sec * 1000000LL) + rusage.ru_stime.tv_usec;
#elif HAVE_GETPROCESSTIMES
    HANDLE proc;
    FILETIME c, e, k, u;
    proc = GetCurrentProcess();
    GetProcessTimes(proc, &c, &e, &k, &u);
    time_stamps.user_usec =
        ((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
    time_stamps.sys_usec =
        ((int64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
#else
    time_stamps.user_usec = time_stamps.sys_usec = 0;
#endif
    return time_stamps;
}

void ff_ni_update_benchmark(const char *fmt, ...)
{
    static BenchmarkTimeStamps current_time;
    va_list va;
    char buf[1024];
    BenchmarkTimeStamps t = get_benchmark_time_stamps();

    if (fmt) {
        va_start(va, fmt);
        vsnprintf(buf, sizeof(buf), fmt, va);
        va_end(va);
        av_log(NULL, AV_LOG_INFO, "bench: %8" PRIu64 " user %8" PRIu64 " "
               "sys %8" PRIu64 " real %s \n",
               t.user_usec - current_time.user_usec,
               t.sys_usec - current_time.sys_usec,
               t.real_usec - current_time.real_usec, buf);
    }
    current_time = t;
}

int ff_ni_ffmpeg_to_gc620_pix_fmt(enum AVPixelFormat pix_fmt)
{
    int i, tablesz;

    tablesz = sizeof(gc620_pixel_fmt_list)/sizeof(struct gc620_pixel_fmts);

    /* linear search through table to find if the pixel format is supported */
    for (i = 0; i < tablesz; i++) {
        if (gc620_pixel_fmt_list[i].pix_fmt_ffmpeg == pix_fmt) {
            return gc620_pixel_fmt_list[i].pix_fmt_gc620;
        }
    }
    return -1;
}

int ff_ni_ffmpeg_to_libxcoder_pix_fmt(enum AVPixelFormat pix_fmt)
{
    int i, tablesz;

    tablesz = sizeof(gc620_pixel_fmt_list)/sizeof(struct gc620_pixel_fmts);

    /* linear search through table to find if the pixel format is supported */
    for (i = 0; i < tablesz; i++) {
        if (gc620_pixel_fmt_list[i].pix_fmt_ffmpeg == pix_fmt) {
            return gc620_pixel_fmt_list[i].pix_fmt_libxcoder;
        }
    }

    return -1;
}


int ff_ni_copy_device_to_host_frame(AVFrame *dst, const ni_frame_t *src, int pix_fmt)
{
  switch (pix_fmt)
  {
    /* packed */
    case GC620_RGBA8888:
    case GC620_BGRA8888:
    case GC620_ABGR8888:
    case GC620_ARGB8888:
    case GC620_RGB565:
    case GC620_BGR565:
    case GC620_B5G5R5X1:
    case GC620_YUYV:
      memcpy(dst->data[0],src->p_data[0],src->data_len[0]);
      break;

    /* semi-planar */
    case GC620_NV12:
    case GC620_NV21:
    case GC620_P010_MSB:
    case GC620_NV16:
      memcpy(dst->data[0], src->p_data[0], src->data_len[0]);
      memcpy(dst->data[1], src->p_data[1], src->data_len[1]);
      break;

    /* planar */
    case GC620_I420:
    case GC620_I010:
      memcpy(dst->data[0], src->p_data[0], src->data_len[0]);
      memcpy(dst->data[1], src->p_data[1], src->data_len[1]);
      memcpy(dst->data[2], src->p_data[2], src->data_len[2]);
      break;

    default:
      return -1;
  }

  return 0;
}

int ff_ni_copy_host_to_device_frame(ni_frame_t *dst, const AVFrame *src, int pix_fmt)
{
  switch (pix_fmt)
  {
    /* packed */
    case GC620_RGBA8888:
    case GC620_BGRA8888:
    case GC620_ABGR8888:
    case GC620_ARGB8888:
    case GC620_RGB565:
    case GC620_BGR565:
    case GC620_B5G5R5X1:
    case GC620_YUYV:
      memcpy(dst->p_data[0], src->data[0], dst->data_len[0]);
      dst->pixel_format = pix_fmt;
      break;

    /* planar */
    case GC620_I420:
    case GC620_I010:
      memcpy(dst->p_data[0], src->data[0], dst->data_len[0]);
      memcpy(dst->p_data[1], src->data[1], dst->data_len[1]);
      memcpy(dst->p_data[2], src->data[2], dst->data_len[2]);
      dst->pixel_format = pix_fmt;
      break;

    /* semi-planar */
    case GC620_NV12:
    case GC620_NV21:
    case GC620_P010_MSB:
    case GC620_NV16:
      memcpy(dst->p_data[0], src->data[0], dst->data_len[0]);
      memcpy(dst->p_data[0], src->data[0], dst->data_len[0]);
      dst->pixel_format = pix_fmt;
      break;

    default:
      dst->pixel_format = -1;
      return -1;
  }

  return 0;
}

void ff_ni_frame_free(void *opaque, uint8_t *data)
{
  int ret;

  if (data)
  {
    niFrameSurface1_t* p_data3 = (niFrameSurface1_t*)((uint8_t*)data);
    if (p_data3->ui16FrameIdx != 0)
    {
      av_log(NULL, AV_LOG_DEBUG, "Recycle trace ui16FrameIdx = [%d] DevHandle %d\n", p_data3->ui16FrameIdx, p_data3->device_handle);
      ret = ni_hwframe_buffer_recycle(p_data3, p_data3->device_handle);
      if (ret != NI_RETCODE_SUCCESS)
      {
        av_log(NULL, AV_LOG_ERROR, "ERROR Failed to recycle trace ui16FrameIdx = [%d] DevHandle %d\n", p_data3->ui16FrameIdx, p_data3->device_handle);
      }
    }
    // buffer is created by av_malloc, so use av_free to release.
    av_free(data);
  }
};


int ff_ni_build_frame_pool(ni_session_context_t *ctx,
                           int width, int height,
                           enum AVPixelFormat out_format,
                           int pool_size)
{
  int rc;
  int scaler_format;
  int options;

  scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(out_format);
  options = NI_SCALER_FLAG_IO |  NI_SCALER_FLAG_PC;
  if (ctx->isP2P) {
      options |= NI_SCALER_FLAG_P2;
  }

  /* Allocate a pool of frames by the scaler */
  rc = ni_device_alloc_frame(ctx,
                             FFALIGN(width,2),
                             FFALIGN(height,2),
                             scaler_format,
                             options,
                             0, // rec width
                             0, // rec height
                             0, // rec X pos
                             0, // rec Y pos
                             pool_size, // rgba color/pool size
                             0, // frame index
                             NI_DEVICE_TYPE_SCALER);

    return rc;
}

void ff_ni_clone_hwframe_ctx(AVHWFramesContext *in_frames_ctx,
                             AVHWFramesContext *out_frames_ctx,
                             ni_session_context_t *ctx)
{
  AVNIFramesContext *in_frames_hwctx;
  AVNIFramesContext *out_frames_hwctx;
  NIFramesContext *out_ni_frames_ctx;
  NIFramesContext *in_ni_frames_ctx;

  /*
   * Warning: ugly hacks lie ahead...
   *
   * We clone the incoming hardware frame context to the output
   * frame context including its internal data. This should really
   * be set up by the ni_frames_init() hwcontext driver.
   */

  out_frames_hwctx = out_frames_ctx->hwctx;
  in_frames_hwctx = in_frames_ctx->hwctx;

  memcpy(out_frames_ctx->internal->priv,
         in_frames_ctx->internal->priv, sizeof(NIFramesContext));

  memcpy(out_frames_hwctx,in_frames_hwctx, sizeof(AVNIFramesContext));

  in_ni_frames_ctx = in_frames_ctx->internal->priv;
  out_ni_frames_ctx = out_frames_ctx->internal->priv;

  if (ctx)
  {
      ni_device_session_copy(ctx, &out_ni_frames_ctx->api_ctx);
  }
  else
  {
      ni_device_session_copy(&in_ni_frames_ctx->api_ctx,
                             &out_ni_frames_ctx->api_ctx);
  }

}

void ff_ni_set_bit_depth_and_encoding_type(int8_t *p_bit_depth,
                                           int8_t *p_enc_type,
                                           enum AVPixelFormat pix_fmt)
{

    // bit depth is 1 for 8-bit format, 2 for 10-bit format
    // encoding type should be 1 for planar or packed, 0 for semi-planar

    switch (pix_fmt)
    {
        case AV_PIX_FMT_YUV420P:
            *p_bit_depth = 1;   // 8-bits per component
            *p_enc_type  = 1;   // planar
            break;

        case AV_PIX_FMT_YUV420P10LE:
            *p_bit_depth = 2;   // 10-bits per component
            *p_enc_type  = 1;   // planar
            break;

        case AV_PIX_FMT_NV12:
            *p_bit_depth = 1;   // 8-bits per component
            *p_enc_type  = 0;   // semi-planar
            break;
        case AV_PIX_FMT_NI_QUAD_8_TILE_4X4:
            *p_bit_depth = 1;   // 8-bits per component
            *p_enc_type = NI_PIXEL_PLANAR_FORMAT_TILED4X4;   // semi-planar
            break;
        case AV_PIX_FMT_P010LE:
            *p_bit_depth = 2;   // 10-bits per component
            *p_enc_type  = 0;   // semi-planar
            break;

        case AV_PIX_FMT_YUYV422:
        case AV_PIX_FMT_UYVY422:
            *p_bit_depth = 1;   // 8-bits per component
            *p_enc_type  = 1;   // packed
            break;

        case AV_PIX_FMT_NV16:
            *p_bit_depth = 1;   // 8-bits per component
            *p_enc_type  = 0;   // semi-planar
            break;

        case AV_PIX_FMT_BGRP:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_BGRA:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_BGR0:
            *p_bit_depth = 1;   // 8-bits per component
            *p_enc_type  = 1;   // packed or planar
            break;

        default:
            av_log(NULL, AV_LOG_WARNING, "WARNING: unexpected pix format %s\n",
                   av_get_pix_fmt_name(pix_fmt));

            // use default values if we've supported a new pixel format
            *p_bit_depth = 1;   // 8-bits per component
            *p_enc_type  = 1;   // planar or packed
            break;
    }
}
