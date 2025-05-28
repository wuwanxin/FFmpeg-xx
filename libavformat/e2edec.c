/*
 * RAW e2evideo demuxer
 * Copyright (c) 2008 Michael Niedermayer <michaelni@gmx.at>
 *
*/

#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "avformat.h"
#include "rawdec.h"



static int e2e_probe(const AVProbeData *p)
{
    int valid_frames = 0;  // 统计有效的帧数
    int i;

    // 检查数据长度是否足够（至少2字节的帧头 + 部分数据）
    if (p->buf_size < 4)
        return 0;

    // 遍历数据，查找帧头 0xFF 0xFF
    for (i = 0; i + 1 < p->buf_size; i++) {
        // 检测帧头 0xFF 0xFF 确认格式0-rgb888 1-yuv420 
        if (p->buf[i] == 0xFF && p->buf[i + 1] == 0xFF && (p->buf[i + 2] == 0x00 || p->buf[i + 2] == 0x01)) {
            // 确保剩余数据足够（假设帧头后至少2字节的有效数据）
            if (i + 3 >= p->buf_size)
                break;

            valid_frames++;  // 统计有效帧
            i += 1;         // 跳过已检测的 0xFF
        }
    }

    // 根据有效帧数量返回探测分数
    if (valid_frames >= 3) {
        // 至少有3个有效帧，认为匹配
        return AVPROBE_SCORE_EXTENSION + 1;  // 比普通格式高1分
    } else if (valid_frames >= 1) {
        // 至少有1个有效帧，可能是该格式
        return AVPROBE_SCORE_EXTENSION / 2;  // 较低分数
    }

    // 无有效帧，不匹配
    return 0;
}

FF_DEF_RAWVIDEO_DEMUXER(e2e, "raw e2e video", e2e_probe, "e2e", AV_CODEC_ID_E2ENC)
