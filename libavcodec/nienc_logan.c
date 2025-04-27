/*
 * NetInt XCoder H.264/HEVC Encoder common code
 * Copyright (c) 2018-2019 NetInt
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

#include "version.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavcodec/put_bits.h"
// starting from FFmpeg n5.1.2, new/different include files
#if (LIBAVCODEC_VERSION_MAJOR > 59 || (LIBAVCODEC_VERSION_MAJOR >= 59 && LIBAVCODEC_VERSION_MINOR >= 37))
#include "put_golomb.h"
#include "startcode.h"
#endif
#include "libavcodec/golomb.h"
#include "libavcodec/hevc.h"
#include "libavcodec/hevc_sei.h"
#include "libavcodec/h264.h"
#include "libavcodec/h264_sei.h"
#include "libavutil/hdr_dynamic_metadata.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_internal.h"
#include "libavutil/hwcontext_ni_logan.h"
#include "bytestream.h"
#include "nienc_logan.h"
#include "ni_av_codec_logan.h"

static int is_logan_input_fifo_empty(XCoderLoganEncContext *ctx)
{
  return av_fifo_size(ctx->fme_fifo) < sizeof(AVFrame);
}

static int is_logan_input_fifo_full(XCoderLoganEncContext *ctx)
{
  return av_fifo_space(ctx->fme_fifo) < sizeof(AVFrame);
}

static int enqueue_logan_frame(AVCodecContext *avctx, const AVFrame *inframe)
{
  int ret = 0;
  XCoderLoganEncContext *ctx = avctx->priv_data;

  // expand frame buffer fifo if not enough space
  if (is_logan_input_fifo_full(ctx))
  {
    if (av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame) >= LOGAN_MAX_FIFO_CAPACITY)
    {
      av_log(avctx, AV_LOG_ERROR, "Encoder frame buffer fifo capacity (%d) reaches the maxmum %d\n",
             av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame), LOGAN_MAX_FIFO_CAPACITY);
      return AVERROR_EXTERNAL;
    }
    ret = av_fifo_realloc2(ctx->fme_fifo, 2 * av_fifo_size(ctx->fme_fifo));
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Enc av_fifo_realloc2 NO MEMORY !!!\n");
      return ret;
    }
    if ((av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame) % 100) == 0)
    {
      av_log(avctx, AV_LOG_INFO, "Enc fifo being extended to: %" PRIu64 "\n",
             av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
    }
    av_assert0(0 == av_fifo_size(ctx->fme_fifo) % sizeof(AVFrame));
  }

  if (inframe == &ctx->buffered_fme)
  {
    // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
    // ff_alloc_get_frame rather than passed as function argument. So we need to
    // judge whether they are the same object. If they are the same NO need to do
    // any reference before queue operation.
    av_fifo_generic_write(ctx->fme_fifo, (void *)inframe, sizeof(*inframe), NULL);
  }
  else
  {
    AVFrame temp_frame;
    memset(&temp_frame, 0, sizeof(AVFrame));
    // In case double free for external input frame and our buffered frame.
    av_frame_ref(&temp_frame, inframe);
    av_fifo_generic_write(ctx->fme_fifo, &temp_frame, sizeof(*inframe), NULL);
  }

  av_log(avctx, AV_LOG_DEBUG, "fme queued pts:%" PRId64 ", fifo size: %" PRIu64 "\n",
         inframe->pts, av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));

  return ret;
}

static bool free_logan_frames_isempty(XCoderLoganEncContext *ctx)
{
  return  (ctx->freeHead == ctx->freeTail);
}

static bool free_logan_frames_isfull(XCoderLoganEncContext *ctx)
{
  return  (ctx->freeHead == ((ctx->freeTail == LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeTail + 1));
}

static int deq_logan_free_frames(XCoderLoganEncContext *ctx)
{
  if (free_logan_frames_isempty(ctx))
  {
    return -1;
  }
  ctx->aFree_Avframes_list[ctx->freeHead] = -1;
  ctx->freeHead = (ctx->freeHead == LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeHead + 1;
  return 0;
}

static int enq_logan_free_frames(XCoderLoganEncContext *ctx, int idx)
{
  if (free_logan_frames_isfull(ctx))
  {
    return -1;
  }
  ctx->aFree_Avframes_list[ctx->freeTail] = idx;
  ctx->freeTail = (ctx->freeTail == LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME) ? 0 : ctx->freeTail + 1;
  return 0;
}

static int recycle_logan_index_2_avframe_index(XCoderLoganEncContext *ctx, uint32_t recycleIndex)
{
  int i;
  for (i = 0; i < LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    if (ctx->sframe_pool[i]->data[3])
    {
      if (((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[i]->data[3]))->i8FrameIdx == recycleIndex)
      {
        return i;
      }
      else
      {
        //av_log(NULL, AV_LOG_TRACE, "sframe_pool[%d] ui16FrameIdx %u != recycleIndex %u\n", i, ((niFrameSurface1_t*)((uint8_t*)ctx->sframe_pool[i]->data[3]))->ui16FrameIdx, recycleIndex);
      }
    }
    else
    {
      //av_log(NULL, AV_LOG_TRACE, "sframe_pool[%d] data[3] NULL\n", i);
    }
  }
  return -1;
}

static int do_open_encoder_device(AVCodecContext *avctx,
                                  XCoderLoganEncContext *ctx,
                                  ni_logan_encoder_params_t *p_param)
{
  int ret;
  int frame_width;
  int frame_height;
  int linesize_aligned;
  int height_aligned;
  int video_full_range_flag = 0;
  AVFrame *in_frame = &ctx->buffered_fme;
  NILOGANFramesContext *nif_src_ctx;
  AVHWFramesContext *avhwf_ctx;
  enum AVColorPrimaries color_primaries;
  enum AVColorTransferCharacteristic color_trc;
  enum AVColorSpace color_space;

  if (in_frame->width > 0 && in_frame->height > 0)
  {
    frame_width = NI_LOGAN_ODD2EVEN(in_frame->width);
    frame_height = NI_LOGAN_ODD2EVEN(in_frame->height);
    color_primaries = in_frame->color_primaries;
    color_trc = in_frame->color_trc;
    color_space = in_frame->colorspace;
    // Force frame color metrics if specified in command line
    if (in_frame->color_primaries != avctx->color_primaries &&
        avctx->color_primaries != AVCOL_PRI_UNSPECIFIED)
    {
      color_primaries = avctx->color_primaries;
    }
    if (in_frame->color_trc != avctx->color_trc &&
        avctx->color_trc != AVCOL_TRC_UNSPECIFIED)
    {
      color_trc = avctx->color_trc;
    }
    if (in_frame->colorspace != avctx->colorspace &&
        avctx->colorspace != AVCOL_SPC_UNSPECIFIED)
    {
      color_space = avctx->colorspace;
    }
  }
  else
  {
    frame_width = NI_LOGAN_ODD2EVEN(avctx->width);
    frame_height = NI_LOGAN_ODD2EVEN(avctx->height);
    color_primaries = avctx->color_primaries;
    color_trc = avctx->color_trc;
    color_space = avctx->colorspace;
  }

  // if frame stride size is not as we expect it,
  // adjust using xcoder-params conf_win_right
  linesize_aligned = ((frame_width + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    linesize_aligned = ((frame_width + 15) / 16) * 16;
  }

  if (linesize_aligned < NI_LOGAN_MIN_WIDTH)
  {
    p_param->enc_input_params.conf_win_right += NI_LOGAN_MIN_WIDTH - frame_width;
    linesize_aligned = NI_LOGAN_MIN_WIDTH;
  }
  else if (linesize_aligned > frame_width)
  {
    p_param->enc_input_params.conf_win_right += linesize_aligned - frame_width;
  }
  p_param->source_width = linesize_aligned;

  height_aligned = ((frame_height + 7) / 8) * 8;
  if (avctx->codec_id == AV_CODEC_ID_H264)
  {
    height_aligned = ((frame_height + 15) / 16) * 16;
  }

  if (height_aligned < NI_LOGAN_MIN_HEIGHT)
  {
    p_param->enc_input_params.conf_win_bottom += NI_LOGAN_MIN_HEIGHT - frame_height;
    p_param->source_height = NI_LOGAN_MIN_HEIGHT;
    height_aligned = NI_LOGAN_MIN_HEIGHT;
  }
  else if (height_aligned > frame_height)
  {
    p_param->enc_input_params.conf_win_bottom += height_aligned - frame_height;
    p_param->source_height = height_aligned;
  }

  // DolbyVision support
  if (5 == p_param->dolby_vision_profile &&
      AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    color_primaries = color_trc = color_space = 2;
    video_full_range_flag = 1;
  }

  // According to the pixel format or color range from the incoming video
  if (avctx->color_range == AVCOL_RANGE_JPEG ||
      avctx->pix_fmt == AV_PIX_FMT_YUVJ420P)
  {
    av_log(avctx, AV_LOG_DEBUG, "%s set video_full_range_flag\n", __FUNCTION__);
    video_full_range_flag = 1;
  }

  // HDR HLG support
  if ((5 == p_param->dolby_vision_profile &&
       AV_CODEC_ID_HEVC == avctx->codec_id) ||
      color_primaries == AVCOL_PRI_BT2020 ||
      color_trc == AVCOL_TRC_SMPTE2084 ||
      color_trc == AVCOL_TRC_ARIB_STD_B67 ||
      color_space == AVCOL_SPC_BT2020_NCL ||
      color_space == AVCOL_SPC_BT2020_CL)
  {
    p_param->hdrEnableVUI = 1;
    ni_logan_set_vui(p_param, &ctx->api_ctx, color_primaries, color_trc, color_space,
                     video_full_range_flag, avctx->sample_aspect_ratio.num,
                     avctx->sample_aspect_ratio.den, ctx->api_ctx.codec_format);
    av_log(avctx, AV_LOG_VERBOSE, "XCoder HDR color info color_primaries: %d "
           "color_trc: %d color_space %d video_full_range_flag %d sar %d/%d\n",
           color_primaries, color_trc, color_space, video_full_range_flag,
           avctx->sample_aspect_ratio.num, avctx->sample_aspect_ratio.den);
  }
  else
  {
    p_param->hdrEnableVUI = 0;
    ni_logan_set_vui(p_param, &ctx->api_ctx, color_primaries, color_trc, color_space,
                     video_full_range_flag, avctx->sample_aspect_ratio.num,
                     avctx->sample_aspect_ratio.den, ctx->api_ctx.codec_format);
  }

  ctx->api_ctx.hw_id = ctx->dev_enc_idx;
  ctx->api_ctx.hw_name = ctx->dev_enc_name;
  strcpy(ctx->api_ctx.dev_xcoder, ctx->dev_xcoder);

  if (in_frame->width > 0 && in_frame->height > 0)
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder buffered_fme.linesize: %d/%d/%d "
           "width/height %dx%d conf_win_right %d  conf_win_bottom %d , "
           "color primaries %u trc %u space %u\n",
           in_frame->linesize[0], in_frame->linesize[1], in_frame->linesize[2],
           in_frame->width, in_frame->height,
           p_param->enc_input_params.conf_win_right,
           p_param->enc_input_params.conf_win_bottom,
           color_primaries, color_trc, color_space);

    if (in_frame->format == AV_PIX_FMT_NI_LOGAN)
    {
      ni_logan_hwframe_surface_t *surface = (ni_logan_hwframe_surface_t *)in_frame->data[3];
#ifdef _WIN32
      int64_t handle = (((int64_t) surface->device_handle_ext) << 32) | surface->device_handle;
      ctx->api_ctx.sender_handle = (ni_device_handle_t) handle;
#else
      ctx->api_ctx.sender_handle = (ni_device_handle_t) surface->device_handle;
#endif
      ctx->api_ctx.hw_action = NI_LOGAN_CODEC_HW_ENABLE;
      av_log(avctx, AV_LOG_VERBOSE, "XCoder frame sender_handle:%p, hw_id:%d\n",
             (void *) ctx->api_ctx.sender_handle, ctx->api_ctx.hw_id);
    }

    if (in_frame->hw_frames_ctx && ctx->api_ctx.hw_id == -1)
    {
      avhwf_ctx = (AVHWFramesContext*) in_frame->hw_frames_ctx->data;
      nif_src_ctx = avhwf_ctx->internal->priv;
      ctx->api_ctx.hw_id = nif_src_ctx->api_ctx.hw_id;
      av_log(avctx, AV_LOG_VERBOSE, "%s: hw_id -1 collocated to %d \n",
             __FUNCTION__, ctx->api_ctx.hw_id);
    }
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder frame width/height %dx%d conf_win_right"
           " %d  conf_win_bottom %d color primaries %u trc %u space %u\n",
           avctx->width, avctx->height, p_param->enc_input_params.conf_win_right,
           p_param->enc_input_params.conf_win_bottom, avctx->color_primaries,
           avctx->color_trc, avctx->colorspace);
  }
  ctx->api_ctx.hw_name=ctx->dev_enc_name;
  ret = ni_logan_device_session_open(&ctx->api_ctx, NI_LOGAN_DEVICE_TYPE_ENCODER);
  // As the file handle may change we need to assign back
  ctx->dev_xcoder_name = ctx->api_ctx.dev_xcoder_name;
  ctx->blk_xcoder_name = ctx->api_ctx.blk_xcoder_name;
  ctx->dev_enc_idx = ctx->api_ctx.hw_id;

  if (ret == NI_LOGAN_RETCODE_INVALID_PARAM)
  {
    av_log(avctx, AV_LOG_ERROR, "%s\n", ctx->api_ctx.param_err_msg);
  }
  if (ret != 0)
  {
    av_log(avctx, AV_LOG_ERROR, "Failed to open encoder (status = %d), "
           "critical error or resource unavailable\n", ret);
    ret = AVERROR_EXTERNAL;
    // ff_xcoder_logan_encode_close(avctx); will be called at codec close
    return ret;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder %s Index %d (inst: %d) opened "
           "successfully\n", ctx->dev_xcoder_name, ctx->dev_enc_idx,
           ctx->api_ctx.session_id);
  }

  return ret;
}

static void do_close_encoder_device(XCoderLoganEncContext *ctx)
{
  ni_logan_device_session_close(&ctx->api_ctx, ctx->encoder_eof,
                          NI_LOGAN_DEVICE_TYPE_ENCODER);
#ifdef _WIN32
  ni_logan_device_close(ctx->api_ctx.device_handle);
#elif __linux__
  ni_logan_device_close(ctx->api_ctx.device_handle);
  ni_logan_device_close(ctx->api_ctx.blk_io_handle);
#endif
  ctx->api_ctx.device_handle = NI_INVALID_DEVICE_HANDLE;
  ctx->api_ctx.blk_io_handle = NI_INVALID_DEVICE_HANDLE;
  ctx->api_ctx.auto_dl_handle = NI_INVALID_DEVICE_HANDLE;
  ctx->api_ctx.sender_handle = NI_INVALID_DEVICE_HANDLE;
}

static int xcoder_logan_encoder_headers(AVCodecContext *avctx)
{
  // use a copy of encoder context, take care to restore original config
  // cropping setting
  int ret, recv, orig_conf_win_right, orig_conf_win_bottom;
  ni_logan_packet_t *xpkt;
  XCoderLoganEncContext *ctx = NULL;
  XCoderLoganEncContext *s = avctx->priv_data;
  ni_logan_encoder_params_t *p_param = &s->api_param;

  ctx = malloc(sizeof(XCoderLoganEncContext));
  if(!ctx)
  {
    return AVERROR(ENOMEM);
  }

  memcpy(ctx, avctx->priv_data, sizeof(XCoderLoganEncContext));

  orig_conf_win_right = p_param->enc_input_params.conf_win_right;
  orig_conf_win_bottom = p_param->enc_input_params.conf_win_bottom;

  ret = do_open_encoder_device(avctx, ctx, p_param);
  if (ret < 0)
  {
    free(ctx);
    return ret;
  }

  xpkt = &(ctx->api_pkt.data.packet);
  ni_logan_packet_buffer_alloc(xpkt, NI_LOGAN_MAX_TX_SZ);

  for (; ;)
  {
    recv = ni_logan_device_session_read(&(ctx->api_ctx), &(ctx->api_pkt),
                                  NI_LOGAN_DEVICE_TYPE_ENCODER);

    if (recv > 0)
    {
      free(avctx->extradata);
      avctx->extradata_size = recv - NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE;
      avctx->extradata = av_mallocz(avctx->extradata_size +
                                    AV_INPUT_BUFFER_PADDING_SIZE);
      memcpy(avctx->extradata,
             (uint8_t*)xpkt->p_data + NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE,
             avctx->extradata_size);

      av_log(avctx, AV_LOG_VERBOSE, "%s len: %d\n",
             __FUNCTION__, avctx->extradata_size);
      break;
    }
    else if (recv == NI_LOGAN_RETCODE_SUCCESS)
    {
      continue;
    }
    else
    {
      av_log(avctx, AV_LOG_ERROR, "%s error: %d", __FUNCTION__, recv);
      break;
    }
  }

  do_close_encoder_device(ctx);

  ni_logan_packet_buffer_free(&(ctx->api_pkt.data.packet));
  ni_logan_rsrc_free_device_context(ctx->rsrc_ctx);
  ctx->rsrc_ctx = NULL;

  p_param->enc_input_params.conf_win_right = orig_conf_win_right;
  p_param->enc_input_params.conf_win_bottom = orig_conf_win_bottom;

  free(ctx);
  ctx = NULL;

  return (recv < 0 ? recv : ret);
}

static int xcoder_logan_setup_encoder(AVCodecContext *avctx)
{
  XCoderLoganEncContext *s = avctx->priv_data;
  int i, ret = 0;
  ni_logan_encoder_params_t *p_param = &s->api_param;
  ni_logan_encoder_params_t *pparams = NULL;
  ni_logan_session_run_state_t prev_state = s->api_ctx.session_run_state;

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);
  //s->api_ctx.session_id = NI_LOGAN_INVALID_SESSION_ID;
  ni_logan_device_session_context_init(&(s->api_ctx));
  s->api_ctx.session_run_state = prev_state;

  s->api_ctx.codec_format = NI_LOGAN_CODEC_FORMAT_H264;
  if (avctx->codec_id == AV_CODEC_ID_HEVC)
  {
    s->api_ctx.codec_format = NI_LOGAN_CODEC_FORMAT_H265;
  }

  s->firstPktArrived = 0;
  s->spsPpsArrived = 0;
  s->spsPpsHdrLen = 0;
  s->p_spsPpsHdr = NULL;
  s->xcode_load_pixel = 0;
  s->reconfigCount = 0;
  s->gotPacket = 0;
  s->sentFrame = 0;
  s->latest_dts = 0;

  if (! s->vpu_reset &&
      LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != s->api_ctx.session_run_state)
  {
    av_log(avctx, AV_LOG_INFO, "Session state: %d allocate frame fifo.\n",
           s->api_ctx.session_run_state);
    // FIFO 4 * FPS length of frames
    s->fme_fifo_capacity = 4 * (avctx->time_base.den / avctx->time_base.num / avctx->ticks_per_frame);
    if(s->fme_fifo_capacity < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "Invalid time_base: too small\n");
      return AVERROR_EXTERNAL;
    }

    s->fme_fifo = av_fifo_alloc(s->fme_fifo_capacity * sizeof(AVFrame));
    av_log(avctx, AV_LOG_DEBUG, "Allocate frame fifo size: %d.\n", s->fme_fifo_capacity);
  }
  else
  {
    if(s->fme_fifo)
    {
      av_log(avctx, AV_LOG_INFO, "Session seq change, fifo size: %" PRIu64 "\n",
             av_fifo_size(s->fme_fifo) / sizeof(AVFrame));
    }
  }

  if (! s->fme_fifo)
  {
    return AVERROR(ENOMEM);
  }
  s->eos_fme_received = 0;

  //Xcoder User Configuration
  ret = ni_logan_encoder_init_default_params(p_param, avctx->framerate.num, avctx->framerate.den,
                                       avctx->bit_rate, NI_LOGAN_ODD2EVEN(avctx->width),
                                       NI_LOGAN_ODD2EVEN(avctx->height));
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_WIDTH_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too big\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_HEIGHT_TOO_SMALL)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Height: too small\n");
    return AVERROR_EXTERNAL;
  }
  if (ret == NI_LOGAN_RETCODE_PARAM_ERROR_AREA_TOO_BIG)
  {
    av_log(avctx, AV_LOG_ERROR, "Invalid Picture Width x Height: exceeds %d\n",
           NI_LOGAN_MAX_RESOLUTION_AREA);
    return AVERROR_EXTERNAL;
  }
  if (ret < 0)
  {
    int i;
    av_log(avctx, AV_LOG_ERROR, "Error setting preset or log.\n");
    av_log(avctx, AV_LOG_INFO, "Possible presets:");
    for (i = 0; g_logan_xcoder_preset_names[i]; i++)
      av_log(avctx, AV_LOG_INFO, " %s", g_logan_xcoder_preset_names[i]);
    av_log(avctx, AV_LOG_INFO, "\n");

    av_log(avctx, AV_LOG_INFO, "Possible log:");
    for (i = 0; g_logan_xcoder_log_names[i]; i++)
      av_log(avctx, AV_LOG_INFO, " %s", g_logan_xcoder_log_names[i]);
    av_log(avctx, AV_LOG_INFO, "\n");

    return AVERROR(EINVAL);
  }

  av_log(avctx, AV_LOG_DEBUG, "pix_fmt is %d, sw_pix_fmt is %d\n",
         avctx->pix_fmt, avctx->sw_pix_fmt);
  if (avctx->pix_fmt != AV_PIX_FMT_NI_LOGAN)
  {
    av_log(avctx, AV_LOG_DEBUG, "sw_pix_fmt assigned to pix_fmt was %d, "
           "is now %d\n", avctx->pix_fmt, avctx->sw_pix_fmt);
    avctx->sw_pix_fmt = avctx->pix_fmt;
  }
  else
  {
    p_param->hwframes = 1;
    av_log(avctx, AV_LOG_DEBUG, "p_param->hwframes = %d\n", p_param->hwframes);
  }

  switch (avctx->sw_pix_fmt)
  {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10BE:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUVJ420P:
      break;
    default:
      av_log(avctx, AV_LOG_ERROR, "Error: pixel format %d not supported.\n",
             avctx->sw_pix_fmt);
      return AVERROR_INVALIDDATA;
  }

  if (s->xcoder_opts)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (!av_dict_parse_string(&dict, s->xcoder_opts, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_logan_encoder_params_set_value(p_param, en->key, en->value, &s->api_ctx);
        switch (parse_ret)
        {
          case NI_LOGAN_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid %s: out of range\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_ZERO:
            av_log(avctx, AV_LOG_ERROR, "Error setting option %s to value 0\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_GOP_INTRA_INCOMPATIBLE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for %s: %s incompatible with GOP structure.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);

      if (ni_logan_encoder_params_check(p_param, s->api_ctx.codec_format) !=
          NI_LOGAN_RETCODE_SUCCESS)
      {
        av_log(avctx, AV_LOG_ERROR, "Validate encode parameters failed\n");
        return AVERROR_EXTERNAL;
      }
    }
  }

  if (p_param->enable_vfr)
  {
    //in the vfr mode, Customer WangSu may reset time base to a very large value, such as 1000.
    //At this time, the calculated framerate depends on timebase and ticket_per_frame is incorrect.
    //So we choose to set the default framerate 30.
    //If the calucluated framerate is correct, we will keep the original calculated framerate value
    //Assume the frame between 5-120 is correct.
    //using the time_base to initial timing info
    if (p_param->enc_input_params.frame_rate < 5 || p_param->enc_input_params.frame_rate > 120)
    {
      p_param->enc_input_params.frame_rate = 30;
    }
    s->api_ctx.ui32timing_scale = avctx->time_base.den;
    s->api_ctx.ui32num_unit_in_tick = avctx->time_base.num;
    s->api_ctx.prev_bitrate = p_param->bitrate;
    s->api_ctx.init_bitrate = p_param->bitrate;
    s->api_ctx.last_change_framenum = 0;
    s->api_ctx.fps_change_detect_count = 0;
  }

  if (s->xcoder_gop)
  {
    AVDictionary *dict = NULL;
    AVDictionaryEntry *en = NULL;

    if (!av_dict_parse_string(&dict, s->xcoder_gop, "=", ":", 0))
    {
      while ((en = av_dict_get(dict, "", en, AV_DICT_IGNORE_SUFFIX)))
      {
        int parse_ret = ni_logan_encoder_gop_params_set_value(p_param, en->key, en->value);
        switch (parse_ret)
        {
          case NI_LOGAN_RETCODE_PARAM_INVALID_NAME:
            av_log(avctx, AV_LOG_ERROR, "Unknown option: %s.\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_BIG:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too big\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_TOO_SMALL:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s too small\n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_OOR:
            av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP parameters: %s out of range \n", en->key);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_ERROR_ZERO:
             av_log(avctx, AV_LOG_ERROR, "Invalid custom GOP paramaters: Error setting option %s to value 0\n", en->key);
             return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_PARAM_INVALID_VALUE:
            av_log(avctx, AV_LOG_ERROR, "Invalid value for GOP param %s: %s.\n", en->key, en->value);
            return AVERROR_EXTERNAL;
          case NI_LOGAN_RETCODE_FAILURE:
            av_log(avctx, AV_LOG_ERROR, "Generic failure during xcoder-params setting for %s\n", en->key);
            return AVERROR_EXTERNAL;
          default:
            break;
        }
      }
      av_dict_free(&dict);
    }
  }

  s->api_ctx.p_session_config = &s->api_param;
  pparams = &s->api_param;
  switch (pparams->enc_input_params.gop_preset_index)
  {
    /* dts_offset is the max number of non-reference frames in a GOP
     * (derived from x264/5 algo) In case of IBBBP the first dts of the I frame should be input_pts-(3*ticks_per_frame)
     * In case of IBP the first dts of the I frame should be input_pts-(1*ticks_per_frame)
     * thus we ensure pts>dts in all cases
     * */
    case 1 /*PRESET_IDX_ALL_I*/:
    case 2 /*PRESET_IDX_IPP*/:
    case 6 /*PRESET_IDX_IPPPP*/:
    case 9 /*PRESET_IDX_SP*/:
      s->dts_offset = 0;
      break;
    /* ts requires dts/pts of I frame not same when there are B frames in streams */
    case 3 /*PRESET_IDX_IBBB*/:
    case 4 /*PRESET_IDX_IBPBP*/:
    case 7 /*PRESET_IDX_IBBBB*/:
      s->dts_offset = 1;
      break;
    case 5 /*PRESET_IDX_IBBBP*/:
      s->dts_offset = 2;
      break;
    case 8 /*PRESET_IDX_RA_IB*/:
      s->dts_offset = 3;
      break;
    default:
      // TBD need user to specify offset
      s->dts_offset = 7;
      av_log(avctx, AV_LOG_VERBOSE, "dts offset default to 7, TBD\n");
      break;
  }
  if (1 == pparams->force_frame_type)
  {
    s->dts_offset = 7;
  }

  av_log(avctx, AV_LOG_INFO, "dts offset: %d\n", s->dts_offset);

  if (0 == strcmp(s->dev_xcoder, LIST_DEVICES_STR))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder: printing out all xcoder devices and "
           "their load, and exit ...\n");
    ni_logan_rsrc_print_all_devices_capability();
    return AVERROR_EXIT;
  }

  //overwrite keep alive timeout value here with a custom value if it was provided
  // provided
  // if xcoder option is set then overwrite the (legacy) decoder option
  uint32_t xcoder_timeout = s->api_param.enc_input_params.keep_alive_timeout;
  if (xcoder_timeout != NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT)
  {
    s->api_ctx.keep_alive_timeout = xcoder_timeout;
  }
  else
  {
    s->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
  }
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVMe Keep Alive Timeout set to = %d\n",
         s->api_ctx.keep_alive_timeout);
  //overwrite set_high_priority value here with a custom value if it was provided
  uint32_t xcoder_high_priority = s->api_param.enc_input_params.set_high_priority;
  if(xcoder_high_priority != 0)
  {
    s->api_ctx.set_high_priority = xcoder_high_priority;
  }
  else
  {
    s->api_ctx.set_high_priority = s->set_high_priority;
  }
  av_log(avctx, AV_LOG_VERBOSE, "Custom NVMe set_high_priority set to = %d\n",
         s->api_ctx.set_high_priority);
  avctx->bit_rate = pparams->bitrate;
  s->total_frames_received = 0;
  s->encoder_eof = 0;
  s->roi_side_data_size = s->nb_rois = 0;
  s->av_rois = NULL;
  s->avc_roi_map = NULL;
  s->hevc_sub_ctu_roi_buf = NULL;
  s->hevc_roi_map = NULL;
  s->api_ctx.src_bit_depth = 8;
  s->api_ctx.src_endian = NI_LOGAN_FRAME_LITTLE_ENDIAN;
  s->api_ctx.roi_len = 0;
  s->api_ctx.roi_avg_qp = 0;
  s->api_ctx.bit_depth_factor = 1;
  if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt ||
      AV_PIX_FMT_YUV420P10LE == avctx->sw_pix_fmt)
  {
    s->api_ctx.bit_depth_factor = 2;
    s->api_ctx.src_bit_depth = 10;
    if (AV_PIX_FMT_YUV420P10BE == avctx->sw_pix_fmt)
    {
      s->api_ctx.src_endian = NI_LOGAN_FRAME_BIG_ENDIAN;
    }
  }

  // DolbyVision, HRD and AUD settings
  if (AV_CODEC_ID_HEVC == avctx->codec_id)
  {
    if (5 == pparams->dolby_vision_profile)
    {
      pparams->hrd_enable = pparams->enable_aud = 1;
      pparams->enc_input_params.forced_header_enable = NI_LOGAN_ENC_REPEAT_HEADERS_ALL_KEY_FRAMES;
      pparams->enc_input_params.decoding_refresh_type = 2;
    }
    if (pparams->hrd_enable)
    {
      pparams->enc_input_params.rc.enable_rate_control = 1;
    }
  }

  // init HW AVFRAME pool
  s->freeHead = 0;
  s->freeTail = 0;
  for (i = 0; i < LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    s->sframe_pool[i] = av_frame_alloc();
    if (!s->sframe_pool[i])
    {
      return AVERROR(ENOMEM);
    }
    s->aFree_Avframes_list[i] = i;
    s->freeTail++;
  }
  s->aFree_Avframes_list[i] = -1;

  // init HDR SEI stuff
  s->api_ctx.sei_hdr_content_light_level_info_len =
  s->api_ctx.light_level_data_len =
  s->api_ctx.sei_hdr_mastering_display_color_vol_len =
  s->api_ctx.mdcv_max_min_lum_data_len = 0;
  s->api_ctx.p_master_display_meta_data = NULL;

  // init HRD SEI stuff (TBD: value after recovery ?)
  s->api_ctx.hrd_params.au_cpb_removal_delay_minus1 = 0;

  memset( &(s->api_fme), 0, sizeof(ni_logan_session_data_io_t) );
  memset( &(s->api_pkt), 0, sizeof(ni_logan_session_data_io_t) );

  if (avctx->width > 0 && avctx->height > 0)
  {
    ni_logan_frame_buffer_alloc(&(s->api_fme.data.frame),
                          NI_LOGAN_ODD2EVEN(avctx->width),
                          NI_LOGAN_ODD2EVEN(avctx->height),
                          0,
                          0,
                          s->api_ctx.bit_depth_factor,
                          (s->buffered_fme.format == AV_PIX_FMT_NI_LOGAN));
  }

  // generate encoded bitstream headers in advance if configured to do so
  if (pparams->generate_enc_hdrs)
  {
    ret = xcoder_logan_encoder_headers(avctx);
  }

  return ret;
}

av_cold int ff_xcoder_logan_encode_init(AVCodecContext *avctx)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  int ret;

  ni_log_set_level(ff_to_ni_log_level(av_log_get_level()));

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);

  if (ctx->dev_xcoder == NULL)
  {
    av_log(avctx, AV_LOG_ERROR, "Error: XCoder option dev_xcoder is null\n");
    return AVERROR_INVALIDDATA;
  }
  else
  {
    av_log(avctx, AV_LOG_VERBOSE, "XCoder options: dev_xcoder: %s dev_enc_idx "
           "%d\n", ctx->dev_xcoder, ctx->dev_enc_idx);
  }

  if (ctx->api_ctx.session_run_state == LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    ctx->dev_enc_idx = ctx->orig_dev_enc_idx;
  }
  else
  {
    ctx->orig_dev_enc_idx = ctx->dev_enc_idx;
  }

  if ((ret = xcoder_logan_setup_encoder(avctx)) < 0)
  {
    ff_xcoder_logan_encode_close(avctx);
    return ret;
  }

#ifdef _WIN32
  // For windows opening the encoder when init will take less time.
  // If HW frame detected then open in xcoder_send_frame function.
  if (avctx->pix_fmt != AV_PIX_FMT_NI_LOGAN)
  {
    // NETINT_INTERNAL - currently only for internal testing
    ni_logan_encoder_params_t *p_param = &ctx->api_param;
    ret = do_open_encoder_device(avctx, ctx, p_param);
    if (ret < 0)
    {
      ff_xcoder_logan_encode_close(avctx);
      return ret;
    }
  }
#endif
  ctx->vpu_reset = 0;

  return 0;
}

static int xcoder_logan_encode_reset(AVCodecContext *avctx)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  av_log(avctx, AV_LOG_WARNING, "%s\n", __FUNCTION__);
  ctx->vpu_reset = 1;
  ff_xcoder_logan_encode_close(avctx);
  return ff_xcoder_logan_encode_init(avctx);
}

int ff_xcoder_logan_encode_close(AVCodecContext *avctx)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  ni_logan_retcode_t ret = NI_LOGAN_RETCODE_FAILURE;
  int i;

  for (i = 0; i < LOGAN_MAX_NUM_FRAMEPOOL_HWAVFRAME; i++)
  {
    //any remaining stored AVframes that have not been unref will die here
    av_frame_free(&(ctx->sframe_pool[i]));
    ctx->sframe_pool[i] = NULL;
  }

  do_close_encoder_device(ctx);

  if (ctx->api_ctx.p_master_display_meta_data)
  {
    free(ctx->api_ctx.p_master_display_meta_data);
    ctx->api_ctx.p_master_display_meta_data = NULL;
  }

  av_log(avctx, AV_LOG_DEBUG, "%s (status = %d)\n", __FUNCTION__, ret);
  ni_logan_frame_buffer_free( &(ctx->api_fme.data.frame) );
  ni_logan_packet_buffer_free( &(ctx->api_pkt.data.packet) );

  if(ctx->fme_fifo)
  {
    av_log(avctx, AV_LOG_DEBUG, "fifo size: %" PRIu64 "\n",
           av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "frame fifo is NULL\n");
  }

  if (! ctx->vpu_reset &&
      ctx->api_ctx.session_run_state != LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING)
  {
    av_fifo_free(ctx->fme_fifo);
    av_log(avctx, AV_LOG_DEBUG, "frame fifo is freed.\n");
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "frame fifo is kept.\n");
  }

  ni_logan_device_session_context_clear(&ctx->api_ctx);
  ni_logan_rsrc_free_device_context(ctx->rsrc_ctx);
  ctx->rsrc_ctx = NULL;

  free(ctx->p_spsPpsHdr);
  ctx->p_spsPpsHdr = NULL;

  free(ctx->av_rois);
  free(ctx->avc_roi_map);
  free(ctx->hevc_sub_ctu_roi_buf);
  free(ctx->hevc_roi_map);
  ctx->av_rois = NULL;
  ctx->avc_roi_map = NULL;
  ctx->hevc_sub_ctu_roi_buf = NULL;
  ctx->hevc_roi_map = NULL;
  ctx->roi_side_data_size = ctx->nb_rois = 0;
  ctx->started = 0;

  return 0;
}

static int xcoder_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  int ret = 0;
  int sent;
  int orig_avctx_width = avctx->width, orig_avctx_height = avctx->height;
  AVFrameSideData *side_data;
  AVHWFramesContext *avhwf_ctx;
  NILOGANFramesContext *nif_src_ctx;
  int is_hwframe;
  int format_in_use;
  int frame_width, frame_height;

  ni_logan_encoder_params_t *p_param = &ctx->api_param; // NETINT_INTERNAL - currently only for internal testing

  av_log(avctx, AV_LOG_DEBUG, "%s pkt_size %d %dx%d  avctx: %dx%d\n", __FUNCTION__,
         frame ? frame->pkt_size : -1, frame ? frame->width : -1,
         frame ? frame->height : -1, avctx->width, avctx->height);

  if (ctx->encoder_flushing)
  {
    if (! frame && is_logan_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_DEBUG, "XCoder EOF: null frame && fifo empty\n");
      return AVERROR_EOF;
    }
  }

  if (! frame)
  {
    if (LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
    {
      av_log(avctx, AV_LOG_DEBUG, "null frame, send queued frame\n");
    }
    else
    {
      ctx->eos_fme_received = 1;
      av_log(avctx, AV_LOG_DEBUG, "null frame, ctx->eos_fme_received = 1\n");
    }
  }
  else
  {
    av_log(avctx, AV_LOG_DEBUG, "%s #%"PRIu64"\n", __FUNCTION__,
           ctx->api_ctx.frame_num);

    // queue up the frame if fifo is NOT empty, or sequence change ongoing !
    if (!is_logan_input_fifo_empty(ctx) ||
        LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      ret = enqueue_logan_frame(avctx, frame);
      if (ret < 0)
      {
        return ret;
      }

      if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        av_log(avctx, AV_LOG_TRACE, "XCoder doing sequence change, frame #"
               "%"PRIu64" queued and return 0 !\n", ctx->api_ctx.frame_num);
        return 0;
      }
    }
    else if (frame != &ctx->buffered_fme)
    {
      // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
      // ff_alloc_get_frame rather than passed as function argument. So we need to
      // judge whether they are the same object. If they are the same NO need to do
      // any reference.
      ret = av_frame_ref(&ctx->buffered_fme, frame);
    }
  }

  if (is_logan_input_fifo_empty(ctx))
  {
    av_log(avctx, AV_LOG_DEBUG, "no frame in fifo to send, just send/receive ..\n");
    if (ctx->eos_fme_received)
    {
      av_log(avctx, AV_LOG_DEBUG, "no frame in fifo to send, send eos ..\n");
      // if received eos but not sent any frame, there is no need to continue the following process
      if (ctx->started == 0)
      {
        av_log(avctx, AV_LOG_DEBUG, "session is not open, send eos, return EOF\n");
        return AVERROR_EOF;
      }
    }
  }
  else
  {
    av_fifo_generic_peek(ctx->fme_fifo, &ctx->buffered_fme,
                         sizeof(AVFrame), NULL);
    // Since the address of frame->data pointer array has changed due to fifo
    // copy, the frame->extended_data pointer needs to be updated.
    ctx->buffered_fme.extended_data = ctx->buffered_fme.data;
  }

  frame_width = NI_LOGAN_ODD2EVEN(ctx->buffered_fme.width);
  frame_height = NI_LOGAN_ODD2EVEN(ctx->buffered_fme.height);
  is_hwframe = (ctx->buffered_fme.format == AV_PIX_FMT_NI_LOGAN);

  // leave encoder instance open to when the first frame buffer arrives so that
  // its stride size is known and handled accordingly.
  if (ctx->started == 0)
  {
#ifdef _WIN32
    if (ctx->buffered_fme.width != avctx->width ||
       ctx->buffered_fme.height != avctx->height ||
       ctx->buffered_fme.color_primaries != avctx->color_primaries ||
       ctx->buffered_fme.color_trc != avctx->color_trc ||
       ctx->buffered_fme.colorspace != avctx->colorspace)
    {
      av_log(avctx, AV_LOG_INFO, "WARNING reopen device Width: %d-%d, "
             "Height: %d-%d, color_primaries: %d-%d, color_trc: %d-%d, "
             "color_space: %d-%d\n",
             ctx->buffered_fme.width, avctx->width,
             ctx->buffered_fme.height, avctx->height,
             ctx->buffered_fme.color_primaries, avctx->color_primaries,
             ctx->buffered_fme.color_trc, avctx->color_trc,
             ctx->buffered_fme.colorspace, avctx->colorspace);
      do_close_encoder_device(ctx);
      // Errror when set this parameters in ni_logan_encoder_params_set_value !!!!!!
      p_param->enc_input_params.conf_win_right = 0;
      p_param->enc_input_params.conf_win_bottom = 0;

      if ((ret = do_open_encoder_device(avctx, ctx, p_param)) < 0)
      {
        return ret;
      }
    }
    else if (is_hwframe) // if hw-frame detected for windows then open here.
#endif
    {
      if ((ret = do_open_encoder_device(avctx, ctx, p_param)) < 0)
      {
        return ret;
      }
    }
    ctx->api_fme.data.frame.start_of_stream = 1;
    ctx->started = 1;
  }
  else
  {
    ctx->api_fme.data.frame.start_of_stream = 0;
  }

  if ((ctx->buffered_fme.height && ctx->buffered_fme.width) &&
      (ctx->buffered_fme.height != avctx->height ||
       ctx->buffered_fme.width != avctx->width))
  {
    av_log(avctx, AV_LOG_INFO, "%s resolution change %dx%d -> %dx%d\n",
           __FUNCTION__, avctx->width, avctx->height, ctx->buffered_fme.width,
           ctx->buffered_fme.height);
    ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING;
    ctx->eos_fme_received = 1;

    // have to queue this frame if not done so: an empty queue
    if (is_logan_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_TRACE, "%s resolution change when fifo empty, frame "
             "#%"PRIu64" being queued\n", __FUNCTION__, ctx->api_ctx.frame_num);
      if (frame != &ctx->buffered_fme)
      {
        // For FFmpeg-n4.4+ receive_packet interface the buffered_fme is fetched from
        // ff_alloc_get_frame rather than passed as function argument. So we need to
        // judge whether they are the same object. If they are the same do NOT
        // unreference any of them because we need to enqueue it later.
        av_frame_unref(&ctx->buffered_fme);
      }
      ret = enqueue_logan_frame(avctx, frame);
      if (ret < 0)
      {
        return ret;
      }
    }
  }

  // init aux params in ni_logan_frame_t
  ni_logan_enc_init_aux_params(&ctx->api_fme);

  // employ a ni_logan_frame_t to represent decode frame when using new libxcoder
  // API to prepare side data
  ni_logan_frame_t dec_frame = {0};
  ni_aux_data_t *aux_data = NULL;

  if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state ||
      (ctx->eos_fme_received && is_logan_input_fifo_empty(ctx)))
  {
    av_log(avctx, AV_LOG_DEBUG, "XCoder start flushing\n");
    ctx->api_fme.data.frame.end_of_stream = 1;
    ctx->encoder_flushing = 1;
  }
  else
  {
    // Set the initial value of extra_data_len
    ctx->api_fme.data.frame.extra_data_len = NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE;

    // NETINT_INTERNAL - currently only for internal testing
    if (p_param->reconf_demo_mode)
    {
      ret = ni_logan_enc_fill_reconfig_params(p_param, &ctx->api_ctx,
                          &ctx->api_fme, ctx->reconfigCount);
      if (ret < 0)
      {
        return ret;
      }
      else
      {
        ctx->reconfigCount = ret;
      }
    }

    // NetInt long term reference frame support
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_LONG_TERM_REF);
    if (side_data && (side_data->size == sizeof(AVNetintLongTermRef)))
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_LONG_TERM_REF,
                                       sizeof(ni_long_term_ref_t));
      if (aux_data)
      {
        memcpy(aux_data->data, side_data->data, side_data->size);
      }

      if (ctx->api_fme.data.frame.reconf_len == 0)
      {
        ctx->reconfigCount++;
      }
    }

    // NetInt target bitrate reconfiguration support
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_BITRATE);
    if (side_data && (side_data->size == sizeof(int32_t)))
    {
      if (ctx->api_param.enable_vfr)
      {
        //ctx->api_params.enc_input_params.frame_rate is the default framerate when vfr enabled
        int32_t bitrate = *((int32_t *)side_data->data);
        ctx->api_ctx.init_bitrate = bitrate;
        bitrate = bitrate * ctx->api_param.enc_input_params.frame_rate / ctx->api_ctx.prev_fps;
        *(int32_t *)side_data->data = bitrate;
        ctx->api_ctx.prev_bitrate = bitrate;
      }

      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_BITRATE,
                                       sizeof(int32_t));
      if (aux_data)
      {
        memcpy(aux_data->data, side_data->data, side_data->size);
      }

      if (ctx->api_fme.data.frame.reconf_len == 0)
      {
        ctx->reconfigCount++;
      }
    }

    // NetInt support VFR by reconfig bitrate and vui
    if (ctx->api_param.enable_vfr)
    {
      int32_t cur_fps = 0, bit_rate = 0;

      if (ctx->buffered_fme.pts > ctx->api_ctx.prev_pts)
      {
        ctx->api_ctx.passed_time_in_timebase_unit += ctx->buffered_fme.pts - ctx->api_ctx.prev_pts;
        ctx->api_ctx.count_frame_num_in_sec++;
        //change the bitrate for VFR
        //1. Only when the fps change, setting the new bitrate
        //2. The interval between two bitrate change settings shall be greater than 1 seconds(hardware limiation)
        //   or at the start the transcoding, we should detect the init frame rate(30) and the actual framerate
        if (ctx->api_ctx.passed_time_in_timebase_unit >= (avctx->time_base.den / avctx->time_base.num))
        {
          cur_fps = ctx->api_ctx.count_frame_num_in_sec;
          bit_rate = (int)(ctx->api_param.enc_input_params.frame_rate * (ctx->api_ctx.init_bitrate / cur_fps));
          if ((ctx->api_ctx.frame_num != 0) && (bit_rate != ctx->api_ctx.prev_bitrate) &&
              ((ctx->api_ctx.frame_num < ctx->api_param.enc_input_params.frame_rate) || 
               ((uint32_t)(ctx->api_ctx.frame_num - ctx->api_ctx.last_change_framenum) >= ctx->api_param.enc_input_params.frame_rate)))
          {
            //adjust the upper and lower limits of bitrate each time
            bit_rate = av_clip(bit_rate, ctx->api_ctx.prev_bitrate / 2, ctx->api_ctx.prev_bitrate * 3 / 2);

            aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_BITRATE,
                                         sizeof(int32_t));
            if (aux_data)
            {
              memcpy(aux_data->data, &bit_rate, sizeof(int32_t));
            }

            ctx->api_ctx.prev_bitrate = bit_rate;
            ctx->api_ctx.last_change_framenum = ctx->api_ctx.frame_num;
            ctx->api_ctx.prev_fps     = cur_fps;
          }
          ctx->api_ctx.count_frame_num_in_sec = 0;
          ctx->api_ctx.passed_time_in_timebase_unit = 0;
        }
        ctx->api_ctx.prev_pts = ctx->buffered_fme.pts;
      }
      else if (ctx->buffered_fme.pts < ctx->api_ctx.prev_pts)
      {
        //error handle for the case that pts jump back
        //this may cause a little error in the bitrate setting, This little error is acceptable.
        //As long as the subsequent, PTS is normal, it will be repaired quickly.
        ctx->api_ctx.prev_pts = ctx->buffered_fme.pts;
      }
      else
      {
        //do nothing, when the pts of two adjacent frames are the same
        //this may cause a little error in the bitrate setting, This little error is acceptable.
        //As long as the subsequent, PTS is normal, it will be repaired quickly.
      }
    }

    // force pic qp demo mode: initial QP (200 frames) -> QP value specified by
    // ForcePicQpDemoMode (100 frames) -> initial QP (remaining frames)
    if (p_param->force_pic_qp_demo_mode)
    {
      if (ctx->api_ctx.frame_num >= 300)
      {
        ctx->api_fme.data.frame.force_pic_qp =
        p_param->enc_input_params.rc.intra_qp;
      }
      else if (ctx->api_ctx.frame_num >= 200)
      {
        ctx->api_fme.data.frame.force_pic_qp = p_param->force_pic_qp_demo_mode;
      }
    }
    // END NETINT_INTERNAL - currently only for internal testing

    // qp reconf parameters
    AVFrameSideData *reconf_side_data;
    reconf_side_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_NETINT_MIN_MAX_QP);
    if (reconf_side_data && reconf_side_data->size == sizeof(AVNetintMinMaxQP))
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame,
                                       NI_FRAME_AUX_DATA_MIN_MAX_QP,
                                       sizeof(ni_logan_rc_min_max_qp));
      if (aux_data)
      {
        memcpy(aux_data->data, reconf_side_data->data, reconf_side_data->size);
      }
    }

    // SEI (HDR)
    // content light level info
    AVFrameSideData *hdr_side_data;

    hdr_side_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (hdr_side_data && hdr_side_data->size == sizeof(AVContentLightMetadata))
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame,
                                       NI_FRAME_AUX_DATA_CONTENT_LIGHT_LEVEL,
                                       sizeof(ni_content_light_level_t));
      if (aux_data)
      {
        memcpy(aux_data->data, hdr_side_data->data, hdr_side_data->size);
      }
    }

    // mastering display color volume
    hdr_side_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (hdr_side_data &&
        hdr_side_data->size == sizeof(AVMasteringDisplayMetadata))
    {
      aux_data = ni_logan_frame_new_aux_data(
        &dec_frame, NI_FRAME_AUX_DATA_MASTERING_DISPLAY_METADATA,
        sizeof(ni_mastering_display_metadata_t));
      if (aux_data)
      {
        memcpy(aux_data->data, hdr_side_data->data, hdr_side_data->size);
      }
    }

    // SEI (HDR10+)
    AVFrameSideData *s_data = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (s_data && s_data->size == sizeof(AVDynamicHDRPlus))
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_HDR_PLUS,
                                       sizeof(ni_dynamic_hdr_plus_t));
      if (aux_data)
      {
        memcpy(aux_data->data, s_data->data, s_data->size);
      }
    } // hdr10+

    // SEI (close caption)
    side_data = av_frame_get_side_data(&ctx->buffered_fme,AV_FRAME_DATA_A53_CC);
    if (side_data && side_data->size > 0)
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_A53_CC,
                                       side_data->size);
      if (aux_data)
      {
        memcpy(aux_data->data, side_data->data, side_data->size);
      }
    }

    // supply QP map if ROI enabled and if ROIs passed in
    const AVFrameSideData *p_sd = av_frame_get_side_data(
      &ctx->buffered_fme, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    if (p_param->enc_input_params.roi_enable && p_sd)
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame,
                                       NI_FRAME_AUX_DATA_REGIONS_OF_INTEREST,
                                       p_sd->size);
      if (aux_data)
      {
        memcpy(aux_data->data, p_sd->data, p_sd->size);
      }
    }

    // User data unregistered SEI
    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_UDU_SEI);
    if (side_data && side_data->size > 0)
    {
      aux_data = ni_logan_frame_new_aux_data(&dec_frame, NI_FRAME_AUX_DATA_UDU_SEI,
                                       side_data->size);
      if (aux_data)
      {
        memcpy(aux_data->data, (uint8_t *)side_data->data, side_data->size);
      }
    }

    ctx->api_fme.data.frame.pts = ctx->buffered_fme.pts;
    ctx->api_fme.data.frame.dts = ctx->buffered_fme.pkt_dts;
    ctx->api_fme.data.frame.video_width = NI_LOGAN_ODD2EVEN(avctx->width);
    ctx->api_fme.data.frame.video_height = NI_LOGAN_ODD2EVEN(avctx->height);

    ctx->api_fme.data.frame.ni_logan_pict_type = 0;

    if (ctx->api_ctx.force_frame_type)
    {
      switch (ctx->buffered_fme.pict_type)
      {
        case AV_PICTURE_TYPE_I:
          ctx->api_fme.data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_FORCE_IDR;
          break;
        case AV_PICTURE_TYPE_P:
          ctx->api_fme.data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_P;
          break;
        default:
          ;
      }
    }
    else if (AV_PICTURE_TYPE_I == ctx->buffered_fme.pict_type)
    {
      ctx->api_fme.data.frame.force_key_frame = 1;
      ctx->api_fme.data.frame.ni_logan_pict_type = LOGAN_PIC_TYPE_FORCE_IDR;
    }

    // whether should send SEI with this frame
    int send_sei_with_idr = ni_logan_should_send_sei_with_frame(
      &ctx->api_ctx, ctx->api_fme.data.frame.ni_logan_pict_type, p_param);

    av_log(avctx, AV_LOG_TRACE, "%s: #%"PRIu64" ni_logan_pict_type %d "
           "forced_header_enable %d intraPeriod %d send_sei_with_idr: %s\n",
           __FUNCTION__,
           ctx->api_ctx.frame_num, ctx->api_fme.data.frame.ni_logan_pict_type,
           p_param->enc_input_params.forced_header_enable,
           p_param->enc_input_params.intra_period,
           send_sei_with_idr ? "Yes" : "No");

    // data buffer for various SEI: HDR mastering display color volume, HDR
    // content light level, close caption, User data unregistered, HDR10+ etc.
    uint8_t mdcv_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t cll_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t cc_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t udu_data[NI_LOGAN_MAX_SEI_DATA];
    uint8_t hdrp_data[NI_LOGAN_MAX_SEI_DATA];

    // prep for auxiliary data (various SEI, ROI) in encode frame, based on the
    // data returned in decoded frame
    ni_logan_enc_prep_aux_data(&ctx->api_ctx, &ctx->api_fme.data.frame, &dec_frame,
                         ctx->api_ctx.codec_format, send_sei_with_idr,
                         mdcv_data, cll_data, cc_data, udu_data, hdrp_data);

    // DolbyVision (HRD SEI), HEVC only for now
    uint8_t hrd_buf[NI_LOGAN_MAX_SEI_DATA];
    uint32_t hrd_sei_len = 0; // HRD SEI size in bytes
    if (AV_CODEC_ID_HEVC == avctx->codec_id && p_param->hrd_enable)
    {
      if (send_sei_with_idr)
      {
        hrd_sei_len += ni_logan_enc_buffering_period_sei(p_param, &ctx->api_ctx,
                                                         ctx->api_ctx.frame_num + 1,
                                                         NI_LOGAN_MAX_SEI_DATA,
                                                         hrd_buf);
      }
      // pic_timing SEI will inserted after encoding
      ctx->api_fme.data.frame.sei_total_len += hrd_sei_len;
    }

    side_data = av_frame_get_side_data(&ctx->buffered_fme,
                                       AV_FRAME_DATA_NETINT_CUSTOM_SEI);
    if (side_data && side_data->size > 0)
    {
      ret = ni_logan_enc_buffering_custom_sei(side_data->data, &ctx->api_ctx, ctx->buffered_fme.pts);
      if (ret < 0)
      {
        av_log(avctx, AV_LOG_ERROR, "ni_logan_enc_buffering_custom_sei failed, ret = %d\n",
               ret);
        return ret;
      }
    }

    if (ctx->api_fme.data.frame.sei_total_len > NI_LOGAN_ENC_MAX_SEI_BUF_SIZE)
    {
      av_log(avctx, AV_LOG_ERROR, "%s: sei total length %u exceeds maximum sei "
             "size %u.\n", __FUNCTION__, ctx->api_fme.data.frame.sei_total_len,
             NI_LOGAN_ENC_MAX_SEI_BUF_SIZE);
      ret = AVERROR(EINVAL);
      return ret;
    }

    ctx->api_fme.data.frame.extra_data_len += ctx->api_fme.data.frame.sei_total_len;
    // FW layout requirement: leave space for reconfig data if SEI and/or ROI
    // is present
    if ((ctx->api_fme.data.frame.sei_total_len ||
         ctx->api_fme.data.frame.roi_len)
        && !ctx->api_fme.data.frame.reconf_len)
    {
      ctx->api_fme.data.frame.extra_data_len += sizeof(ni_logan_encoder_change_params_t);
    }

    if (ctx->api_ctx.auto_dl_handle != NI_INVALID_DEVICE_HANDLE)
    {
      is_hwframe = 0;
      format_in_use = avctx->sw_pix_fmt;
      av_log(avctx, AV_LOG_TRACE, "%s: Autodownload mode, disable hw frame\n",
             __FUNCTION__);
    }
    else
    {
      format_in_use = ctx->buffered_fme.format;
    }

    if (is_hwframe)
    {
      ret = sizeof(ni_logan_hwframe_surface_t);
    }
    else
    {
      ret = av_image_get_buffer_size(format_in_use,
                                     ctx->buffered_fme.width,
                                     ctx->buffered_fme.height, 1);
    }
#if FF_API_PKT_PTS
    av_log(avctx, AV_LOG_TRACE, "%s: pts=%" PRId64 ", pkt_dts=%" PRId64 ", "
           "pkt_pts=%" PRId64 "\n", __FUNCTION__, ctx->buffered_fme.pts,
           ctx->buffered_fme.pkt_dts, ctx->buffered_fme.pkt_pts);
#endif
    av_log(avctx, AV_LOG_TRACE, "%s: buffered_fme.format=%d, width=%d, "
           "height=%d, pict_type=%d, key_frame=%d, size=%d\n", __FUNCTION__,
           format_in_use, ctx->buffered_fme.width, ctx->buffered_fme.height,
           ctx->buffered_fme.pict_type, ctx->buffered_fme.key_frame, ret);

    if (ret < 0)
    {
      return ret;
    }

    if (is_hwframe)
    {
      uint8_t *dsthw;
      const uint8_t *srchw;
      ni_logan_frame_buffer_alloc_hwenc(&(ctx->api_fme.data.frame),
                                  NI_LOGAN_ODD2EVEN(ctx->buffered_fme.width),
                                  NI_LOGAN_ODD2EVEN(ctx->buffered_fme.height),
                                  ctx->api_fme.data.frame.extra_data_len);
      if (!ctx->api_fme.data.frame.p_data[3])
      {
        return AVERROR(ENOMEM);
      }

      dsthw = ctx->api_fme.data.frame.p_data[3];
      srchw = (const uint8_t *) ctx->buffered_fme.data[3];
      av_log(avctx, AV_LOG_TRACE, "dst=%p src=%p, len =%d\n", dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
      memcpy(dsthw, srchw, ctx->api_fme.data.frame.data_len[3]);
      av_log(avctx, AV_LOG_TRACE, "session_id:%u, FrameIdx:%d, %d, W-%u, H-%u, bit_depth:%d, encoding_type:%d\n",
             ((ni_logan_hwframe_surface_t *)dsthw)->ui16SessionID,
             ((ni_logan_hwframe_surface_t *)dsthw)->i8FrameIdx,
             ((ni_logan_hwframe_surface_t *)dsthw)->i8InstID,
             ((ni_logan_hwframe_surface_t *)dsthw)->ui16width,
             ((ni_logan_hwframe_surface_t *)dsthw)->ui16height,
             ((ni_logan_hwframe_surface_t *)dsthw)->bit_depth,
             ((ni_logan_hwframe_surface_t *)dsthw)->encoding_type);
    }
    else
    {
      int dst_stride[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
      int dst_height_aligned[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
      int src_height[NI_LOGAN_MAX_NUM_DATA_POINTERS] = {0};
      int need_to_copy = 1;

      src_height[0] = ctx->buffered_fme.height;
      src_height[1] = src_height[2] = ctx->buffered_fme.height / 2;

      ni_logan_get_hw_yuv420p_dim(frame_width, frame_height,
                            ctx->api_ctx.bit_depth_factor,
                            avctx->codec_id == AV_CODEC_ID_H264,
                            dst_stride, dst_height_aligned);

      av_log(avctx, AV_LOG_DEBUG, "[0] %p stride[0] %u height %u data[1] %p data[3] %p extra_len %d\n",
            ctx->buffered_fme.data[0], dst_stride[0], ctx->buffered_fme.height, ctx->buffered_fme.data[1],
            ctx->buffered_fme.data[3], ctx->api_fme.data.frame.extra_data_len);
      if (NI_LOGAN_RETCODE_SUCCESS == is_logan_fw_rev_higher(&ctx->api_ctx, 20, 16) &&
          NI_LOGAN_RETCODE_SUCCESS == ni_logan_frame_zerocopy_check(ctx->buffered_fme.width,
                                        ctx->buffered_fme.height,
                                        ctx->buffered_fme.linesize,
                                        dst_stride,
                                        src_height,
                                        dst_height_aligned,
                                        ctx->api_ctx.bit_depth_factor,
                                        ctx->buffered_fme.data))
      {
        need_to_copy = 0;
        // Need to modify ni_logan_encoder_frame_buffer_alloc
        ni_logan_frame_zerocopy_buffer_alloc(&(ctx->api_fme.data.frame),
                                      NI_LOGAN_ODD2EVEN(ctx->buffered_fme.width),
                                      NI_LOGAN_ODD2EVEN(ctx->buffered_fme.height),
                                      dst_stride,
                                      ctx->api_fme.data.frame.extra_data_len,
                                      ctx->api_ctx.bit_depth_factor,
                                      ctx->buffered_fme.data);
      }
      else
      {
        // alignment(16) extra padding for H.264 encoding
        ni_logan_encoder_frame_buffer_alloc(&(ctx->api_fme.data.frame),
                                      NI_LOGAN_ODD2EVEN(ctx->buffered_fme.width),
                                      NI_LOGAN_ODD2EVEN(ctx->buffered_fme.height),
                                      dst_stride,
                                      (avctx->codec_id == AV_CODEC_ID_H264),
                                      ctx->api_fme.data.frame.extra_data_len,
                                      ctx->api_ctx.bit_depth_factor);
      }

      if (!ctx->api_fme.data.frame.p_data[0])
      {
        return AVERROR(ENOMEM);
      }

      if (ctx->api_ctx.auto_dl_handle == NI_INVALID_DEVICE_HANDLE)
      {
        av_log(avctx, AV_LOG_TRACE, "%s: api_fme.data_len[0]=%d,"
               "buffered_fme.linesize=%d/%d/%d, dst alloc linesize = %d/%d/%d, "
               "src height = %d/%d%d, dst height aligned = %d/%d/%d, "
               "ctx->api_fme.force_key_frame=%d, extra_data_len=%d sei_size=%u "
               "(hdr_content_light_level %u hdr_mastering_display_color_vol %u "
               "hdr10+ %u cc %u udu %u prefC %u hrd %u) "
               "reconf_size=%u roi_size=%u force_pic_qp=%u "
               "use_cur_src_as_long_term_pic %u use_long_term_ref %u\n",
               __FUNCTION__, ctx->api_fme.data.frame.data_len[0],
               ctx->buffered_fme.linesize[0],
               ctx->buffered_fme.linesize[1],
               ctx->buffered_fme.linesize[2],
               dst_stride[0], dst_stride[1], dst_stride[2],
               src_height[0], src_height[1], src_height[2],
               dst_height_aligned[0], dst_height_aligned[1], dst_height_aligned[2],
               ctx->api_fme.data.frame.force_key_frame,
               ctx->api_fme.data.frame.extra_data_len,
               ctx->api_fme.data.frame.sei_total_len,
               ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len,
               ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len,
               ctx->api_fme.data.frame.sei_hdr_plus_len,
               ctx->api_fme.data.frame.sei_cc_len,
               ctx->api_fme.data.frame.sei_user_data_unreg_len,
               ctx->api_fme.data.frame.preferred_characteristics_data_len,
               hrd_sei_len,
               ctx->api_fme.data.frame.reconf_len,
               ctx->api_fme.data.frame.roi_len,
               ctx->api_fme.data.frame.force_pic_qp,
               ctx->api_fme.data.frame.use_cur_src_as_long_term_pic,
               ctx->api_fme.data.frame.use_long_term_ref);

        if(need_to_copy)
        {
          // YUV part of the encoder input data layout
          ni_logan_copy_hw_yuv420p((uint8_t **) ctx->api_fme.data.frame.p_data,
                            ctx->buffered_fme.data, ctx->buffered_fme.width,
                            ctx->buffered_fme.height,
                            ctx->api_ctx.bit_depth_factor,
                            dst_stride, dst_height_aligned,
                            ctx->buffered_fme.linesize, src_height);
          ctx->api_fme.data.frame.separate_metadata = 0;
        }
        else
        {
          ctx->api_fme.data.frame.separate_metadata = 1;
        }

        av_log(avctx, AV_LOG_TRACE, "After memcpy p_data 0:0x%p, 1:0x%p, 2:0x%p"
               " len:0:%d 1:%d 2:%d\n",
               ctx->api_fme.data.frame.p_data[0],
               ctx->api_fme.data.frame.p_data[1],
               ctx->api_fme.data.frame.p_data[2],
               ctx->api_fme.data.frame.data_len[0],
               ctx->api_fme.data.frame.data_len[1],
               ctx->api_fme.data.frame.data_len[2]);
      }
      else
      {
        ni_logan_hwframe_surface_t *src_surf;
        ni_logan_session_data_io_t *p_session_data;
        av_log(avctx, AV_LOG_TRACE, "%s: Autodownload to be run\n", __FUNCTION__);
        avhwf_ctx = (AVHWFramesContext*)ctx->buffered_fme.hw_frames_ctx->data;
        nif_src_ctx = avhwf_ctx->internal->priv;
        src_surf = (ni_logan_hwframe_surface_t*)ctx->buffered_fme.data[3];
        p_session_data = &ctx->api_fme;

        ret = ni_logan_device_session_hwdl(&nif_src_ctx->api_ctx, p_session_data, src_surf);
        if (ret <= 0)
        {
          av_log(avctx, AV_LOG_ERROR, "nienc.c:ni_logan_hwdl_frame() failed to retrieve frame\n");
          return AVERROR_EXTERNAL;
        }
      }
    }

    // auxiliary data part of the encoder input data layout
    ni_logan_enc_copy_aux_data(&ctx->api_ctx, &ctx->api_fme.data.frame, &dec_frame,
                         ctx->api_ctx.codec_format, mdcv_data, cll_data,
                         cc_data, udu_data, hdrp_data);
    ni_logan_frame_buffer_free(&dec_frame);

    // fill in HRD SEI if available
    if (hrd_sei_len)
    {
      uint8_t *dst = (uint8_t *)ctx->api_fme.data.frame.p_data[3] +
                     (ctx->api_fme.data.frame.separate_metadata ? 0 : ctx->api_fme.data.frame.data_len[3]) +
                     NI_LOGAN_APP_ENC_FRAME_META_DATA_SIZE;

      // skip data portions already filled in until the HRD SEI part;
      // reserve reconfig size if any of sei, roi or reconfig is present
      if (ctx->api_fme.data.frame.reconf_len ||
          ctx->api_fme.data.frame.roi_len ||
          ctx->api_fme.data.frame.sei_total_len)
      {
        dst += sizeof(ni_logan_encoder_change_params_t);
      }

      // skip any of the following data types enabled, to get to HRD location:
      // - ROI map
      // - HDR mastering display color volume
      // - HDR content light level info
      // - HLG preferred characteristics SEI
      // - close caption
      // - HDR10+
      // - User data unregistered SEI
      dst += ctx->api_fme.data.frame.roi_len +
      ctx->api_fme.data.frame.sei_hdr_mastering_display_color_vol_len +
      ctx->api_fme.data.frame.sei_hdr_content_light_level_info_len +
      ctx->api_fme.data.frame.preferred_characteristics_data_len +
      ctx->api_fme.data.frame.sei_cc_len +
      ctx->api_fme.data.frame.sei_hdr_plus_len +
      ctx->api_fme.data.frame.sei_user_data_unreg_len;

      memcpy(dst, hrd_buf, hrd_sei_len);
    }

    ctx->sentFrame = 1;
  }

  sent = ni_logan_device_session_write(&ctx->api_ctx, &ctx->api_fme, NI_LOGAN_DEVICE_TYPE_ENCODER);
  av_log(avctx, AV_LOG_DEBUG, "%s: pts %lld dts %lld size %d sent to xcoder\n",
         __FUNCTION__, ctx->api_fme.data.frame.pts, ctx->api_fme.data.frame.dts, sent);

  // return EIO at error
  if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == sent)
  {
    ret = xcoder_logan_encode_reset(avctx);
    if (ret < 0)
    {
      av_log(avctx, AV_LOG_ERROR, "%s(): VPU recovery failed:%d, return EIO\n",
             __FUNCTION__, sent);
      ret = AVERROR(EIO);
    }
    return ret;
  }
  else if (sent < 0)
  {
    av_log(avctx, AV_LOG_ERROR, "%s(): failure sent (%d) , return EIO\n",
           __FUNCTION__, sent);
    ret = AVERROR(EIO);

    // if rejected due to sequence change in progress, revert resolution
    // setting and will do it again next time.
    if (ctx->api_fme.data.frame.start_of_stream &&
        (avctx->width != orig_avctx_width ||
         avctx->height != orig_avctx_height))
    {
      avctx->width = orig_avctx_width;
      avctx->height = orig_avctx_height;
    }
    return ret;
  }
  else if (sent == 0)
  {
    // case of sequence change in progress
    if (ctx->api_fme.data.frame.start_of_stream &&
        (avctx->width != orig_avctx_width ||
         avctx->height != orig_avctx_height))
    {
      avctx->width = orig_avctx_width;
      avctx->height = orig_avctx_height;
    }

    // when buffer_full, drop the frame and return EAGAIN if in strict timeout
    // mode, otherwise buffer the frame and it is to be sent out using encode2
    // API: queue the frame only if not done so yet, i.e. queue is empty
    // *and* it's a valid frame. ToWatch: what are other rc cases ?
    if (ctx->api_ctx.status == NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL)
    {
      if (ctx->api_param.strict_timeout_mode)
      {
        av_log(avctx, AV_LOG_ERROR, "%s: Error Strict timeout period exceeded, "
               "return EAGAIN\n", __FUNCTION__);
        ret = AVERROR(EAGAIN);
      }
      else
      {
        av_log(avctx, AV_LOG_DEBUG, "%s: Write buffer full, returning 1\n",
               __FUNCTION__);
        ret = 1;
        if (frame && is_logan_input_fifo_empty(ctx))
        {
          ret = enqueue_logan_frame(avctx, frame);
          if (ret < 0)
          {
            return ret;
          }
        }
      }
    }
  }
  else
  {
    if (!ctx->eos_fme_received && is_hwframe)
    {
      av_frame_ref(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]], &ctx->buffered_fme);
      av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d popped from free head %d\n", ctx->aFree_Avframes_list[ctx->freeHead], ctx->freeHead);
      av_log(avctx, AV_LOG_TRACE, "ctx->buffered_fme.data[3] %p sframe_pool[%d]->data[3] %p\n",
             ctx->buffered_fme.data[3], ctx->aFree_Avframes_list[ctx->freeHead],
             ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
      if (ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3])
      {
        av_log(avctx, AV_LOG_DEBUG, "sframe_pool[%d] ui16FrameIdx %u, device_handle %" PRId64 ".\n",
               ctx->aFree_Avframes_list[ctx->freeHead],
               ((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->i8FrameIdx,
               ((ni_logan_hwframe_surface_t*)((uint8_t*)ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]))->device_handle);
        av_log(avctx, AV_LOG_TRACE, "%s: after ref sframe_pool, hw frame av_buffer_get_ref_count=%d, data[3]=%p\n",
               __FUNCTION__, av_buffer_get_ref_count(ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->buf[0]),
               ctx->sframe_pool[ctx->aFree_Avframes_list[ctx->freeHead]]->data[3]);
      }
      if (deq_logan_free_frames(ctx) != 0)
      {
        av_log(avctx, AV_LOG_ERROR, "free frames is empty\n");
        ret = AVERROR_EXTERNAL;
        return ret;
      }
    }

    // only if it's NOT sequence change flushing (in which case only the eos
    // was sent and not the first sc pkt) AND
    // only after successful sending will it be removed from fifo
    if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING != ctx->api_ctx.session_run_state)
    {
      if (! is_logan_input_fifo_empty(ctx))
      {
        av_fifo_drain(ctx->fme_fifo, sizeof(AVFrame));
        av_log(avctx, AV_LOG_DEBUG, "fme popped pts:%" PRId64 ", "
               "fifo size: %" PRIu64 "\n",  ctx->buffered_fme.pts,
               av_fifo_size(ctx->fme_fifo) / sizeof(AVFrame));
      }
      av_frame_unref(&ctx->buffered_fme);
    }
    else
    {
      av_log(avctx, AV_LOG_TRACE, "XCoder frame(eos) sent, sequence changing! NO fifo pop !\n");
    }

    //pushing input pts in circular FIFO
    ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_w_idx % NI_LOGAN_FIFO_SZ] = ctx->api_fme.data.frame.pts;
    ctx->api_ctx.enc_pts_w_idx++;
    ret = 0;

    // have another check before return: if no more frames in fifo to send and
    // we've got eos (NULL) frame from upper stream, flag for flushing
    if (ctx->eos_fme_received && is_logan_input_fifo_empty(ctx))
    {
      av_log(avctx, AV_LOG_DEBUG, "Upper stream EOS frame received, fifo empty, start flushing ..\n");
      ctx->encoder_flushing = 1;
    }
  }

  if (ctx->encoder_flushing)
  {
    av_log(avctx, AV_LOG_DEBUG, "%s flushing ..\n", __FUNCTION__);
    ret = ni_logan_device_session_flush(&ctx->api_ctx, NI_LOGAN_DEVICE_TYPE_ENCODER);
  }

  return ret;
}

static int xcoder_logan_encode_reinit(AVCodecContext *avctx)
{
  int ret = 0;
  AVFrame tmp_fme;
  XCoderLoganEncContext *ctx = avctx->priv_data;
  ni_logan_session_run_state_t prev_state = ctx->api_ctx.session_run_state;

  ctx->eos_fme_received = 0;
  ctx->encoder_eof = 0;
  ctx->encoder_flushing = 0;

  if (ctx->api_ctx.pts_table && ctx->api_ctx.dts_queue)
  {
    ff_xcoder_logan_encode_close(avctx);
    ctx->api_ctx.session_run_state = prev_state;
  }
  ctx->started = 0;
  ctx->firstPktArrived = 0;
  ctx->spsPpsArrived = 0;
  ctx->spsPpsHdrLen = 0;
  ctx->p_spsPpsHdr = NULL;

  // and re-init avctx's resolution to the changed one that is
  // stored in the first frame of the fifo
  av_fifo_generic_peek(ctx->fme_fifo, &tmp_fme, sizeof(AVFrame), NULL);
  av_log(avctx, AV_LOG_INFO, "%s resolution changing %dx%d -> %dx%d\n",
         __FUNCTION__, avctx->width, avctx->height, tmp_fme.width, tmp_fme.height);
  avctx->width = tmp_fme.width;
  avctx->height = tmp_fme.height;

  ret = ff_xcoder_logan_encode_init(avctx);
  ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;

  while ((ret >= 0) && !is_logan_input_fifo_empty(ctx))
  {
    ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
    ret = xcoder_send_frame(avctx, NULL);

    // new resolution changes or buffer full should break flush.
    // if needed, add new cases here
    if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING == ctx->api_ctx.session_run_state)
    {
      av_log(avctx, AV_LOG_DEBUG, "%s(): break flush queued frames, "
             "resolution changes again, session_run_state=%d, status=%d\n",
             __FUNCTION__, ctx->api_ctx.session_run_state, ctx->api_ctx.status);
      break;
    }
    else if (NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL == ctx->api_ctx.status)
    {
      ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
      av_log(avctx, AV_LOG_DEBUG, "%s(): break flush queued frames, "
            "because of buffer full, session_run_state=%d, status=%d\n",
            __FUNCTION__, ctx->api_ctx.session_run_state, ctx->api_ctx.status);
      break;
    }
    else
    {
      ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
      av_log(avctx, AV_LOG_DEBUG, "%s(): continue to flush queued frames, "
             "ret=%d\n", __FUNCTION__, ret);
    }
  }

  return ret;
}

static int xcoder_logan_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  ni_logan_encoder_params_t *p_param = &ctx->api_param;
  int i, ret = 0;
  int recv;
  ni_logan_packet_t *xpkt = &ctx->api_pkt.data.packet;

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);

  if (ctx->encoder_eof)
  {
    av_log(avctx, AV_LOG_TRACE, "%s: EOS\n", __FUNCTION__);
    return AVERROR_EOF;
  }

  ni_logan_packet_buffer_alloc(xpkt, NI_LOGAN_MAX_TX_SZ);
  while (1)
  {
    xpkt->recycle_index = -1;
    recv = ni_logan_device_session_read(&ctx->api_ctx, &ctx->api_pkt, NI_LOGAN_DEVICE_TYPE_ENCODER);

    av_log(avctx, AV_LOG_TRACE, "%s: xpkt.end_of_stream=%d, xpkt.data_len=%d, "
           "recv=%d, encoder_flushing=%d, encoder_eof=%d\n", __FUNCTION__,
           xpkt->end_of_stream, xpkt->data_len, recv, ctx->encoder_flushing,
           ctx->encoder_eof);

    if (recv <= 0)
    {
      ctx->encoder_eof = xpkt->end_of_stream;
      /* not ready ?? */
      if (ctx->encoder_eof || xpkt->end_of_stream)
      {
        if (LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
            ctx->api_ctx.session_run_state)
        {
          // after sequence change completes, reset codec state
          av_log(avctx, AV_LOG_INFO, "%s 1: sequence change completed, return "
                 "EAGAIN and will reopen " "codec!\n", __FUNCTION__);

          ret = xcoder_logan_encode_reinit(avctx);
          if (ret >= 0)
          {
            ret = AVERROR(EAGAIN);
          }
          break;
        }

        ret = AVERROR_EOF;
        av_log(avctx, AV_LOG_TRACE, "%s: got encoder_eof, "
               "return AVERROR_EOF\n", __FUNCTION__);
        break;
      }
      else
      {
        if (NI_LOGAN_RETCODE_ERROR_VPU_RECOVERY == recv)
        {
          ret = xcoder_logan_encode_reset(avctx);
          if (ret < 0)
          {
            av_log(avctx, AV_LOG_ERROR, "%s(): VPU recovery failed:%d, "
                   "returning EIO\n", __FUNCTION__, recv);
            ret = AVERROR(EIO);
          }
          return ret;
        }

        if (recv < 0)
        {
          if ((NI_LOGAN_RETCODE_ERROR_INVALID_SESSION == recv) && !ctx->started)  // session may be in recovery state, return EAGAIN
          {
            av_log(avctx, AV_LOG_ERROR, "%s: VPU might be reset, "
                   "invalid session id\n", __FUNCTION__);
            ret = AVERROR(EAGAIN);
          }
          else
          {
            av_log(avctx, AV_LOG_ERROR, "%s: Persistent failure, "
                   "returning EIO,ret=%d\n", __FUNCTION__, recv);
            ret = AVERROR(EIO);
          }
          ctx->gotPacket = 0;
          ctx->sentFrame = 0;
          break;
        }

        if (ctx->api_param.low_delay_mode == 1&& ctx->sentFrame && !ctx->gotPacket)
        {
          av_log(avctx, AV_LOG_TRACE, "%s: low delay mode, keep reading until "
                 "pkt arrives\n", __FUNCTION__);
          continue;
        } else if (ctx->api_param.low_delay_mode == 2 &&
                   ctx->api_ctx.frame_num - ctx->api_ctx.pkt_num >
                   ( ctx->api_param.enc_input_params.gop_preset_index == 0 ?
                     ctx->api_param.enc_input_params.custom_gop_params.custom_gop_size:
                     g_map_preset_to_gopsize[ctx->api_param.enc_input_params.gop_preset_index]))
        {
          av_log(avctx, AV_LOG_TRACE, "%s: low delay mode 2, keep reading send frame %d receive pkt %d gop %d\n",
                __FUNCTION__, ctx->api_ctx.frame_num, ctx->api_ctx.pkt_num, ctx->api_param.enc_input_params.gop_preset_index);
          continue;
        }

        ctx->gotPacket = 0;
        ctx->sentFrame = 0;
        if (!is_logan_input_fifo_empty(ctx) &&
            (LOGAN_SESSION_RUN_STATE_NORMAL == ctx->api_ctx.session_run_state) &&
            (NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL != ctx->api_ctx.status))
        {
          ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
          ret = xcoder_send_frame(avctx, NULL);

          // if session_run_state is changed in xcoder_send_frame, keep it
          if (LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
          {
            ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
          }
          if (ret < 0)
          {
            av_log(avctx, AV_LOG_ERROR, "%s(): xcoder_send_frame 1 error, "
                   "ret=%d\n", __FUNCTION__, ret);
            return ret;
          }
          continue;
        }
        ret = AVERROR(EAGAIN);
        if (! ctx->encoder_flushing && ! ctx->eos_fme_received)
        {
          av_log(avctx, AV_LOG_TRACE, "%s: NOT encoder_flushing, NOT "
                 "eos_fme_received, return AVERROR(EAGAIN)\n", __FUNCTION__);
          break;
        }
      }
    }
    else
    {
      /* got encoded data back */
      int meta_size = NI_LOGAN_FW_ENC_BITSTREAM_META_DATA_SIZE;
      if (avctx->pix_fmt == AV_PIX_FMT_NI_LOGAN && xpkt->recycle_index >= 0 && xpkt->recycle_index < 1056)
      {
        int avframe_index;
        av_log(avctx, AV_LOG_VERBOSE, "UNREF index %d.\n", xpkt->recycle_index);
        avframe_index = recycle_logan_index_2_avframe_index(ctx, xpkt->recycle_index);
        if (avframe_index >=0 && ctx->sframe_pool[avframe_index])
        {
          AVFrame *frame = ctx->sframe_pool[avframe_index];
          void *opaque = av_buffer_get_opaque(frame->buf[0]);
          // This opaque would carry the valid event handle to help release the
          // hwframe surface descriptor for windows target.
          opaque = (void *) ctx->api_ctx.event_handle;
          av_log(avctx, AV_LOG_TRACE, "%s: after ref sframe_pool, hw frame "
                 "av_buffer_get_ref_count=%d, data[3]=%p event handle:%p\n",
                 __FUNCTION__, av_buffer_get_ref_count(frame->buf[0]),
                 frame->data[3], opaque);
          av_frame_unref(frame);
          av_log(avctx, AV_LOG_DEBUG, "AVframe_index = %d pushed to free tail "
                 "%d\n", avframe_index, ctx->freeTail);
          if (enq_logan_free_frames(ctx, avframe_index) != 0)
          {
            av_log(avctx, AV_LOG_ERROR, "free frames is full\n");
          }
          av_log(avctx, AV_LOG_DEBUG, "enq head %d, tail %d\n",ctx->freeHead, ctx->freeTail);
          //enqueue the index back to free
          xpkt->recycle_index = -1;
        }
        else
        {
          av_log(avctx, AV_LOG_DEBUG, "can't push to tail - avframe_index %d sframe_pool %p\n",
                 avframe_index, ctx->sframe_pool[avframe_index]);
        }
      }

      if (! ctx->spsPpsArrived)
      {
        ret = AVERROR(EAGAIN);
        ctx->spsPpsArrived = 1;
        ctx->spsPpsHdrLen = recv - meta_size;
        ctx->p_spsPpsHdr = malloc(ctx->spsPpsHdrLen);
        if (! ctx->p_spsPpsHdr)
        {
          ret = AVERROR(ENOMEM);
          break;
        }

        memcpy(ctx->p_spsPpsHdr, (uint8_t*)xpkt->p_data + meta_size, xpkt->data_len - meta_size);

        // start pkt_num counter from 1 to get the real first frame
        ctx->api_ctx.pkt_num = 1;
        // for low-latency mode, keep reading until the first frame is back
        if (ctx->api_param.low_delay_mode == 1)
        {
          av_log(avctx, AV_LOG_TRACE, "%s: low delay mode, keep reading until "
                 "1st pkt arrives\n", __FUNCTION__);
          continue;
        }
        break;
      }
      ctx->gotPacket = 1;
      ctx->sentFrame = 0;

      uint8_t pic_timing_buf[NI_LOGAN_MAX_SEI_DATA];
      uint32_t pic_timing_sei_len = 0;
      int nalu_type = 0;
      const uint8_t *p_start_code;
      uint32_t stc = -1;
      uint32_t copy_len = 0;
      uint8_t *p_src = (uint8_t*)xpkt->p_data + meta_size;
      uint8_t *p_end = p_src + (xpkt->data_len - meta_size);
      int is_idr = 0;
      int64_t local_pts = xpkt->pts;
      int custom_sei_cnt = 0;
      int total_custom_sei_len = 0;
      int sei_idx = 0;
      ni_logan_all_custom_sei_t *ni_logan_all_custom_sei;
      ni_logan_custom_sei_t *ni_custom_sei;
      if (ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ])
      {
        ni_logan_all_custom_sei = ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ];
        custom_sei_cnt = ni_logan_all_custom_sei->custom_sei_cnt;
        for (sei_idx = 0; sei_idx < custom_sei_cnt; sei_idx++)
        {
          total_custom_sei_len += ni_logan_all_custom_sei->ni_custom_sei[sei_idx].custom_sei_size;
        }
      }

      if (p_param->hrd_enable || custom_sei_cnt)
      {
        // if HRD or custom sei enabled, search for pic_timing or custom SEI insertion point by
        // skipping non-VCL until video data is found.
        p_start_code = p_src;
        if(AV_CODEC_ID_HEVC == avctx->codec_id)
        {
          do
          {
            stc = -1;
            p_start_code = avpriv_find_start_code(p_start_code, p_end, &stc);
            nalu_type = (stc >> 1) & 0x3F;
          } while (nalu_type > HEVC_NAL_RSV_VCL31);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        }
        else if(AV_CODEC_ID_H264 == avctx->codec_id)
        {
          do
          {
            stc = -1;
            p_start_code = avpriv_find_start_code(p_start_code, p_end, &stc);
            nalu_type = stc & 0x1F;
          } while (nalu_type > H264_NAL_IDR_SLICE);

          // calc. length to copy
          copy_len = p_start_code - 5 - p_src;
        }
        else
        {
          av_log(avctx, AV_LOG_ERROR, "%s: codec %d not supported for SEI !\n",
                 __FUNCTION__, avctx->codec_id);
        }

        if (p_param->hrd_enable)
        {
          int is_i_or_idr;
          if (HEVC_NAL_IDR_W_RADL == nalu_type || HEVC_NAL_IDR_N_LP == nalu_type)
          {
            is_idr = 1;
          }
          is_i_or_idr = (LOGAN_PIC_TYPE_I   == xpkt->frame_type ||
                         LOGAN_PIC_TYPE_IDR == xpkt->frame_type ||
                         LOGAN_PIC_TYPE_CRA == xpkt->frame_type);
          pic_timing_sei_len = ni_logan_enc_pic_timing_sei2(p_param, &ctx->api_ctx,
                                                            is_i_or_idr, is_idr, xpkt->pts,
                                                            NI_LOGAN_MAX_SEI_DATA, pic_timing_buf);
          // returned pts is display number
        }
      }

      if (! ctx->firstPktArrived)
      {
        int sizeof_spspps_attached_to_idr = ctx->spsPpsHdrLen;

        // if not enable forced repeat header, check AV_CODEC_FLAG_GLOBAL_HEADER flag
        // to determine whether to add a SPS/PPS header in the first packat
        if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
            (p_param->enc_input_params.forced_header_enable != NI_LOGAN_ENC_REPEAT_HEADERS_ALL_KEY_FRAMES) &&
             (p_param->enc_input_params.forced_header_enable != NI_LOGAN_ENC_REPEAT_HEADERS_ALL_I_FRAMES))
        {
          sizeof_spspps_attached_to_idr = 0;
        }
        ctx->firstPktArrived = 1;
        ctx->first_frame_pts = xpkt->pts;

        ret = ff_get_encode_buffer(avctx, pkt, xpkt->data_len - meta_size + sizeof_spspps_attached_to_idr + total_custom_sei_len + pic_timing_sei_len, 0);
        if (! ret)
        {
          uint8_t *p_side_data, *p_dst;
          // fill in AVC/HEVC sidedata
          if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
              (avctx->extradata_size != ctx->spsPpsHdrLen ||
               memcmp(avctx->extradata, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen)))
          {
            avctx->extradata_size = ctx->spsPpsHdrLen;
            free(avctx->extradata);
            avctx->extradata = av_mallocz(avctx->extradata_size +
                                          AV_INPUT_BUFFER_PADDING_SIZE);
            if (! avctx->extradata)
            {
              av_log(avctx, AV_LOG_ERROR, "Cannot allocate AVC/HEVC header of "
                     "size %d.\n", avctx->extradata_size);
              return AVERROR(ENOMEM);
            }
            memcpy(avctx->extradata, ctx->p_spsPpsHdr, avctx->extradata_size);
          }

          p_side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA,
                                                ctx->spsPpsHdrLen);
          if (p_side_data)
          {
            memcpy(p_side_data, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen);
          }

          p_dst = pkt->data;
          if (sizeof_spspps_attached_to_idr)
          {
            memcpy(p_dst, ctx->p_spsPpsHdr, ctx->spsPpsHdrLen);
            p_dst += ctx->spsPpsHdrLen;
          }

          // 1st pkt, skip buffering_period SEI and insert pic_timing SEI
          if (pic_timing_sei_len || custom_sei_cnt)
          {
            // copy buf_period
            memcpy(p_dst, p_src, copy_len);
            p_dst += copy_len;

            // copy custom sei before slice
            sei_idx = 0;
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              if (ni_custom_sei->custom_sei_loc == NI_LOGAN_CUSTOM_SEI_LOC_AFTER_VCL)
              {
                break;
              }
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }

            // copy pic_timing
            if (pic_timing_sei_len)
            {
              memcpy(p_dst, pic_timing_buf, pic_timing_sei_len);
              p_dst += pic_timing_sei_len;
            }

            // copy the IDR data
            memcpy(p_dst, p_src + copy_len,
                   xpkt->data_len - meta_size - copy_len);
            p_dst += (xpkt->data_len - meta_size - copy_len);

            // copy custom sei after slice
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }
          }
          else
          {
            memcpy(p_dst, (uint8_t*)xpkt->p_data + meta_size,
                   xpkt->data_len - meta_size);
          }
        }

        // free buffer
        if (custom_sei_cnt)
        {
          free(ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ]);
          ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ] = NULL;
        }
      }
      else
      {
        // insert header when intraRefresh is enabled and forced header mode is 1 (all key frames)
        // for every intraRefreshMinPeriod key frames, pkt counting starts from 1, e.g. for
        // cycle of 100, the header is forced on frame 102, 202, ...;
        // note that api_ctx.pkt_num returned is the actual index + 1
        int intra_refresh_hdr_sz = 0;
        if (ctx->p_spsPpsHdr && ctx->spsPpsHdrLen &&
            (p_param->enc_input_params.forced_header_enable == NI_LOGAN_ENC_REPEAT_HEADERS_ALL_KEY_FRAMES) &&
            (1 == p_param->enc_input_params.intra_mb_refresh_mode ||
             2 == p_param->enc_input_params.intra_mb_refresh_mode ||
             3 == p_param->enc_input_params.intra_mb_refresh_mode))
        {
          if (p_param->intra_refresh_reset)
          {
            if (ctx->api_ctx.pkt_num - ctx->api_ctx.force_frame_pkt_num == p_param->ui32minIntraRefreshCycle)
            {
              intra_refresh_hdr_sz = ctx->spsPpsHdrLen;
              ctx->api_ctx.force_frame_pkt_num = ctx->api_ctx.pkt_num;
            }
            else
            {
              for (i = 0; i < NI_LOGAN_MAX_FORCE_FRAME_TABLE_SIZE; i++)
              {
                if (xpkt->pts == ctx->api_ctx.force_frame_pts_table[i])
                {
                  intra_refresh_hdr_sz = ctx->spsPpsHdrLen;
                  ctx->api_ctx.force_frame_pkt_num = ctx->api_ctx.pkt_num;
                  break;
                }
              }
            }
          }
          else if (p_param->ui32minIntraRefreshCycle > 0 && ctx->api_ctx.pkt_num > 3 && 0 == ((ctx->api_ctx.pkt_num - 3) % p_param->ui32minIntraRefreshCycle))
          {
            intra_refresh_hdr_sz = ctx->spsPpsHdrLen;
            av_log(avctx, AV_LOG_TRACE, "%s pkt %" PRId64 " force header on "
                   "intraRefreshMinPeriod %u\n", __FUNCTION__,
                   ctx->api_ctx.pkt_num - 1, p_param->ui32minIntraRefreshCycle);
          }
        }

        ret = ff_get_encode_buffer(avctx, pkt, xpkt->data_len - meta_size + total_custom_sei_len + pic_timing_sei_len + intra_refresh_hdr_sz, 0);
        if (! ret)
        {
          uint8_t *p_dst = pkt->data;
          if (intra_refresh_hdr_sz)
          {
            memcpy(p_dst, ctx->p_spsPpsHdr, intra_refresh_hdr_sz);
            p_dst += intra_refresh_hdr_sz;
          }
          // insert pic_timing if required
          if (pic_timing_sei_len || custom_sei_cnt)
          {
            // for non-IDR, skip AUD and insert
            // for IDR, skip AUD VPS SPS PPS buf_period and insert
            memcpy(p_dst, p_src, copy_len);
            p_dst += copy_len;

            // copy custom sei before slice
            sei_idx = 0;
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              if (ni_custom_sei->custom_sei_loc == NI_LOGAN_CUSTOM_SEI_LOC_AFTER_VCL)
              {
                break;
              }
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }

            // copy pic_timing
            if (pic_timing_sei_len)
            {
              memcpy(p_dst, pic_timing_buf, pic_timing_sei_len);
              p_dst += pic_timing_sei_len;
            }

            // copy the video data
            memcpy(p_dst, p_src + copy_len,
                   xpkt->data_len - meta_size - copy_len);
            p_dst += (xpkt->data_len - meta_size - copy_len);

            // copy custom sei after slice
            while (sei_idx < custom_sei_cnt)
            {
              ni_custom_sei = &ni_logan_all_custom_sei->ni_custom_sei[sei_idx];
              memcpy(p_dst, ni_custom_sei->custom_sei_data, ni_custom_sei->custom_sei_size);
              p_dst += ni_custom_sei->custom_sei_size;
              sei_idx++;
            }
          }
          else
          {
            memcpy(p_dst, (uint8_t*)xpkt->p_data + meta_size,
                   xpkt->data_len - meta_size);
          }
        }

        // free buffer
        if (custom_sei_cnt)
        {
          free(ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ]);
          ctx->api_ctx.pkt_custom_sei[local_pts % NI_LOGAN_FIFO_SZ] = NULL;
        }
      }
      if (!ret)
      {
        if (LOGAN_PIC_TYPE_IDR == xpkt->frame_type ||
            LOGAN_PIC_TYPE_CRA == xpkt->frame_type)
        {
          pkt->flags |= AV_PKT_FLAG_KEY;
        }
        pkt->pts = xpkt->pts;
        /* to ensure pts>dts for all frames, we assign a guess pts for the first 'dts_offset' frames and then the pts from input stream
         * is extracted from input pts FIFO.
         * if GOP = IBBBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -3 -2 -1 0 1 ... and -3 -2 -1 are the guessed values
         * if GOP = IBPBP and PTSs = 0 1 2 3 4 5 .. then out DTSs = -1 0 1 2 3 ... and -1 is the guessed value
         * the number of guessed values is equal to dts_offset
         */
        if (ctx->total_frames_received < ctx->dts_offset)
        {
          // guess dts
          pkt->dts = ctx->first_frame_pts + (ctx->total_frames_received - ctx->dts_offset) * avctx->ticks_per_frame;
        }
        else
        {
          // get dts from pts FIFO
          pkt->dts = ctx->api_ctx.enc_pts_list[ctx->api_ctx.enc_pts_r_idx % NI_LOGAN_FIFO_SZ];
          ctx->api_ctx.enc_pts_r_idx++;
        }

        if (ctx->total_frames_received >= 1)
        {
          if (pkt->dts < ctx->latest_dts)
          {
            av_log(avctx, AV_LOG_WARNING, "dts: %" PRId64 ". < latest_dts: "
                   "%" PRId64 ".\n", pkt->dts, ctx->latest_dts);
          }
        }

        if (pkt->dts > pkt->pts)
        {
          av_log(avctx, AV_LOG_WARNING, "dts: %" PRId64 ", pts: %" PRId64 ". "
                 "Forcing dts = pts\n", pkt->dts, pkt->pts);
          pkt->dts = pkt->pts;
        }
        ctx->total_frames_received++;
        ctx->latest_dts = pkt->dts;
        av_log(avctx, AV_LOG_DEBUG, "%s pkt %" PRId64 " pts %" PRId64 " "
               "dts %" PRId64 "  size %d  st_index %d \n", __FUNCTION__,
               ctx->api_ctx.pkt_num - 1, pkt->pts, pkt->dts, pkt->size,
               pkt->stream_index);
      }
      ctx->encoder_eof = xpkt->end_of_stream;

      if (ctx->encoder_eof &&
          LOGAN_SESSION_RUN_STATE_SEQ_CHANGE_DRAINING ==
          ctx->api_ctx.session_run_state)
      {
        // after sequence change completes, reset codec state
        av_log(avctx, AV_LOG_TRACE, "%s 2: sequence change completed, "
               "return 0 and will reopen codec !\n", __FUNCTION__);
        ret = xcoder_logan_encode_reinit(avctx);
      }
      else if(!is_logan_input_fifo_empty(ctx) &&
              (LOGAN_SESSION_RUN_STATE_NORMAL == ctx->api_ctx.session_run_state) &&
              (NI_LOGAN_RETCODE_NVME_SC_WRITE_BUFFER_FULL != ctx->api_ctx.status))
      {
        ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING;
        ret = xcoder_send_frame(avctx, NULL);

        // if session_run_state is changed in xcoder_send_frame, keep it
        if (LOGAN_SESSION_RUN_STATE_QUEUED_FRAME_DRAINING == ctx->api_ctx.session_run_state)
        {
          ctx->api_ctx.session_run_state = LOGAN_SESSION_RUN_STATE_NORMAL;
        }
        if (ret < 0)
        {
          av_log(avctx, AV_LOG_ERROR, "%s: xcoder_send_frame 2 error, ret=%d\n",
                 __FUNCTION__, ret);
          return ret;
        }
      }
      break;
    }
  }

  return ret;
}

int ff_xcoder_logan_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                           const AVFrame *frame, int *got_packet)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  int ret;

  av_log(avctx, AV_LOG_DEBUG, "%s\n", __FUNCTION__);

  ret = xcoder_send_frame(avctx, frame);
  // return immediately for critical errors
  if (AVERROR(ENOMEM) == ret || AVERROR_EXTERNAL == ret ||
      (ret < 0 && ctx->encoder_eof))
  {
    return ret;
  }

  ret = xcoder_logan_receive_packet(avctx, pkt);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
  {
    *got_packet = 0;
  }
  else if (ret < 0)
  {
    return ret;
  }
  else
  {
    *got_packet = 1;
  }

  return 0;
}

int ff_xcoder_logan_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
  XCoderLoganEncContext *ctx = avctx->priv_data;
  AVFrame *frame = &ctx->buffered_fme;
  int ret;

  ret = ff_encode_get_frame(avctx, frame);
  if (!ctx->encoder_flushing && ret >= 0 || ret == AVERROR_EOF)
  {
    ret = xcoder_send_frame(avctx, (ret == AVERROR_EOF ? NULL : frame));
    if (ret < 0 && ret != AVERROR_EOF)
    {
      return ret;
    }
  }
  // Once send_frame returns EOF go on receiving packets until EOS is met.
  return xcoder_logan_receive_packet(avctx, pkt);
}

// Needed for yuvbypass on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || (LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82))
const AVCodecHWConfigInternal *ff_ni_logan_enc_hw_configs[] = {
  HW_CONFIG_ENCODER_FRAMES(NI_LOGAN,  NI_LOGAN),
  HW_CONFIG_ENCODER_DEVICE(YUV420P, NI_LOGAN),
  HW_CONFIG_ENCODER_DEVICE(YUV420P10, NI_LOGAN),
  NULL,
};
#endif
