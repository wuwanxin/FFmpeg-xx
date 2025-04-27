/*
 * XCoder HEVC Decoder
 * Copyright (c) 2018 NetInt
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
// include codec_internal.h starting from FFmpeg n5.1.2
#if (LIBAVCODEC_VERSION_MAJOR > 59 || (LIBAVCODEC_VERSION_MAJOR >= 59 && LIBAVCODEC_VERSION_MINOR >= 37))
#include "codec_internal.h"
#endif

/**
 * @file
 * XCoder decoder.
 */

#include "nidec_logan.h"


#define OFFSETDEC(x) offsetof(XCoderLoganDecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption dec_options[] = {
  { "xcoder",    "Select which XCoder card to use.",  OFFSETDEC(dev_xcoder),
    AV_OPT_TYPE_STRING, { .str = "bestmodelload" }, CHAR_MIN, CHAR_MAX, VD, "xcoder" },

  { "bestload",      "Pick the least real loaded XCoder/decoder available.", 0, AV_OPT_TYPE_CONST,
    { .str = "bestload" }, 0, 0, VD, "xcoder" },

  { "bestmodelload",      "Pick the least model loaded XCoder/decoder available.", 0, AV_OPT_TYPE_CONST,
    { .str = "bestmodelload" }, 0, 0, VD, "xcoder" },

  { "bestinst",      "Pick the XCoder/decoder with the least number of running decoding instances.", 0, AV_OPT_TYPE_CONST,
    { .str = "bestinst" }, 0, 0, VD, "xcoder" },

  { "list",      "List the available XCoder cards.", 0, AV_OPT_TYPE_CONST,
    { .str = "list" }, 0, 0, VD, "xcoder" },

  { "dec",       "Select which decoder to use by index. First is 0, second is 1, and so on.", OFFSETDEC(dev_dec_idx),
    AV_OPT_TYPE_INT, { .i64 = NI_LOGAN_INVALID_HWID }, -1, INT_MAX, VD, "dec" },

  { "decname", "Select which decoder to use by index. First is /dev/nvme0n1, second is /dev/nvme0n2, and so on.", OFFSETDEC(dev_dec_name),
    AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, VD, "decname" },

  { "keep_alive_timeout",       "Specify a custom session keep alive timeout in seconds.", OFFSETDEC(keep_alive_timeout),
    AV_OPT_TYPE_INT, { .i64 = NI_LOGAN_DEFAULT_KEEP_ALIVE_TIMEOUT }, NI_LOGAN_MIN_KEEP_ALIVE_TIMEOUT, NI_LOGAN_MAX_KEEP_ALIVE_TIMEOUT, VD, "keep_alive_timeout" },

  { "user_data_sei_passthru",       "Enable user data unregistered SEI passthrough.", OFFSETDEC(enable_user_data_sei_passthru),
    AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VD, "user_data_sei_passthru" },

  { "check_packet",       "Enable checking source packets. Skip SEI payloads after SLICE", OFFSETDEC(enable_check_packet),
    AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VD, "check_packet" },

  { "custom_sei_passthru",       "Specify a custom SEI type to passthrough.", OFFSETDEC(custom_sei),
    AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 254, VD, "custom_sei_passthru" },

  { "low_delay",       "Specify a decode timeout value (in milliseconds, recommended value is 600) "
    "to enable low delay mode. Should be used only for streams that are in sequence.", OFFSETDEC(low_delay),
    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 10000, VD, "low_delay" },

  { "hwframes",       "Use hwframes to reduce YUV buffer traffic.", OFFSETDEC(hwFrames),
    AV_OPT_TYPE_INT,{ .i64 = HW_FRAMES_OFF }, 0, INT_MAX, VD, "hwFrames" },

  { "xcoder-params", "Set the XCoder configuration using a :-separated list of key=value parameters", OFFSETDEC(xcoder_opts),
    AV_OPT_TYPE_STRING,{ 0 }, 0, 0, VD },

  { "set_high_priority",       "Specify a custom session set high priority in 0 or 1", OFFSETDEC(set_high_priority),
    AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VD, "set_high_priority" },

  { NULL }
};

static const AVClass h265_xcoderdec_class = {
  .class_name = "h265_ni_logan_dec",
  .item_name = av_default_item_name,
  .option = dec_options,
  .version = LIBAVUTIL_VERSION_INT,
};

#if (LIBAVCODEC_VERSION_MAJOR > 59 || (LIBAVCODEC_VERSION_MAJOR >= 59 && LIBAVCODEC_VERSION_MINOR >= 37))
FFCodec
#else
AVCodec
#endif
ff_h265_ni_logan_decoder = {
#if (LIBAVCODEC_VERSION_MAJOR > 59 || (LIBAVCODEC_VERSION_MAJOR >= 59 && LIBAVCODEC_VERSION_MINOR >= 37))
  .p.name           = "h265_ni_logan_dec",
  .p.long_name      = NULL_IF_CONFIG_SMALL("H.265 NetInt Logan decoder v" NI_LOGAN_XCODER_REVISION),
  .p.type           = AVMEDIA_TYPE_VIDEO,
  .p.id             = AV_CODEC_ID_HEVC,
  .p.priv_class     = &h265_xcoderdec_class,
  .p.wrapper_name   = "libxcoder_logan",
  FF_CODEC_RECEIVE_FRAME_CB(xcoder_logan_receive_frame),
  .p.pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10BE, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NONE },
  .p.capabilities   = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_DELAY
#else
  .name           = "h265_ni_logan_dec",
  .long_name      = NULL_IF_CONFIG_SMALL("H.265 NetInt Logan decoder v" NI_LOGAN_XCODER_REVISION),
  .type           = AVMEDIA_TYPE_VIDEO,
  .id             = AV_CODEC_ID_HEVC,
  .priv_class     = &h265_xcoderdec_class,
  .receive_frame  = xcoder_logan_receive_frame,
  .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10BE, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_NONE },
  .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS,
  .capabilities   = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_DELAY
#endif
#ifndef NI_DEC_GSTREAMER_SUPPORT
  | AV_CODEC_CAP_HARDWARE
#endif
,
  .priv_data_size = sizeof(XCoderLoganDecContext),
  .init           = xcoder_logan_decode_init,
  .flush          = xcoder_logan_decode_flush_buffers,
  .close          = xcoder_logan_decode_close,

  .bsfs           = "hevc_mp4toannexb",
};
