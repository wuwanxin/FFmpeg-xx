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

/**
 * @file
 * XCoder decoder.
 */

#include "nidec.h"
// Needed for hwframe on FFmpeg-n4.3+
#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 82)
#include "hwconfig.h"
#else
#include "hwaccel.h"
#endif

static const AVCodecHWConfigInternal *ff_ni_quad_hw_configs[] = {
  &(const AVCodecHWConfigInternal) {
  .public = {
    .pix_fmt = AV_PIX_FMT_NI_QUAD,
    .methods = AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX |
    AV_CODEC_HW_CONFIG_METHOD_AD_HOC | AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX,
    .device_type = AV_HWDEVICE_TYPE_NI_QUADRA,
  },
    .hwaccel = NULL,
},
NULL
};

static const AVOption dec_options[] = {
    {"xcoder",
     "Select which XCoder card to use.",
     OFFSETDEC(dev_xcoder),
     AV_OPT_TYPE_STRING,
     {.str = NI_BEST_MODEL_LOAD_STR},
     CHAR_MIN,
     CHAR_MAX,
     VD,
     "xcoder"},

    {"bestmodelload",
     "Pick the least model load XCoder/decoder available.",
     0,
     AV_OPT_TYPE_CONST,
     {.str = NI_BEST_MODEL_LOAD_STR},
     0,
     0,
     VD,
     "xcoder"},

    {"bestload",
     "Pick the least real load XCoder/decoder available.",
     0,
     AV_OPT_TYPE_CONST,
     {.str = NI_BEST_REAL_LOAD_STR},
     0,
     0,
     VD,
     "xcoder"},

    {"dec",
     "Select which decoder to use by index. First is 0, second is 1, and so "
     "on.",
     OFFSETDEC(dev_dec_idx),
     AV_OPT_TYPE_INT,
     {.i64 = BEST_DEVICE_LOAD},
     -1,
     INT_MAX,
     VD,
     "dec"},

    {"decname",
     "Select which decoder to use by NVMe block device name, e.g. "
     "/dev/nvme0n1.",
     OFFSETDEC(dev_blk_name),
     AV_OPT_TYPE_STRING,
     {0},
     0,
     0,
     VD,
     "decname"},

    {"user_data_sei_passthru",
     "Enable user data unregistered SEI passthrough.",
     OFFSETDEC(enable_user_data_sei_passthru),
     AV_OPT_TYPE_BOOL,
     {.i64 = 0},
     0,
     1,
     VD,
     "user_data_sei_passthru"},

    {"custom_sei_passthru",
     "Specify a custom SEI type to passthrough.",
     OFFSETDEC(custom_sei_type),
     AV_OPT_TYPE_INT,
     {.i64 = -1},
     -1,
     254,
     VD,
     "custom_sei_passthru"},

    {"xcoder-params",
     "Set the XCoder configuration using a :-separated list of key=value "
     "parameters.",
     OFFSETDEC(xcoder_opts),
     AV_OPT_TYPE_STRING,
     {0},
     0,
     0,
     VD},

    {"keep_alive_timeout",
     "Specify a custom session keep alive timeout in seconds.",
     OFFSETDEC(keep_alive_timeout),
     AV_OPT_TYPE_INT,
     {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
     NI_MIN_KEEP_ALIVE_TIMEOUT,
     NI_MAX_KEEP_ALIVE_TIMEOUT,
     VD,
     "keep_alive_timeout"},

    {"low_delay",
     "Enable low delay decoding mode for 1 in, 1 out decoding sequence. set 1 "
     "to enable low delay mode. Should be used only for streams that are in "
     "sequence.",
     OFFSETDEC(low_delay),
     AV_OPT_TYPE_INT,
     {.i64 = 0},
     0,
     1,
     VD,
     "low_delay"},

    {NULL}};

static const AVClass h265_xcoderdec_class = {
  .class_name = "h265_ni_quadra_dec",
  .item_name = av_default_item_name,
  .option = dec_options,
  .version = LIBAVUTIL_VERSION_INT,
};

#if (LIBAVCODEC_VERSION_MAJOR > 60 || (LIBAVCODEC_VERSION_MAJOR == 60 && LIBAVCODEC_VERSION_MINOR >= 3))
FFCodec
#else
AVCodec
#endif
ff_h265_ni_quadra_decoder = {
#if (LIBAVCODEC_VERSION_MAJOR > 60 || (LIBAVCODEC_VERSION_MAJOR == 60 && LIBAVCODEC_VERSION_MINOR >= 3))
  .p.name           = "h265_ni_quadra_dec",
  CODEC_LONG_NAME("H.265 NetInt Quadra decoder v" NI_XCODER_REVISION),
  .p.type           = AVMEDIA_TYPE_VIDEO,
  .p.id             = AV_CODEC_ID_HEVC,
  .p.priv_class     = &h265_xcoderdec_class,
  .p.capabilities   = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,
  .p.pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12,
                                                  AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE,
                                                  AV_PIX_FMT_NONE },
  FF_CODEC_RECEIVE_FRAME_CB(xcoder_receive_frame),
#else
  .name           = "h265_ni_quadra_dec",
  .long_name      = NULL_IF_CONFIG_SMALL("H.265 NetInt Quadra decoder v" NI_XCODER_REVISION),
  .type           = AVMEDIA_TYPE_VIDEO,
  .id             = AV_CODEC_ID_HEVC,
  .priv_class     = &h265_xcoderdec_class,
  .capabilities   = AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE,
  .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12,
                                                  AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_P010LE,
                                                  AV_PIX_FMT_NONE },
  .receive_frame  = xcoder_receive_frame,
  .caps_internal  = FF_CODEC_CAP_SETS_PKT_DTS,
#endif
  .priv_data_size = sizeof(XCoderH264DecContext),
  .init           = xcoder_decode_init,
  .close          = xcoder_decode_close,
  .hw_configs     = ff_ni_quad_hw_configs,
  .bsfs           = "hevc_mp4toannexb",
  .flush          = xcoder_decode_flush,
};
