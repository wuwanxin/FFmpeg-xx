/*SRM muxer*/

#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "avformat.h"
#include "internal.h"
#include "rawenc.h"
#include "isom.h"
#include "riff.h"

#define SRM_SYNC_WORD            0x73717561  // "sqau"
#define SRM_PACKET_SYNC_WORD     0xFF72FF6F  // "ÿrÿo"
#define SRM_HEADER_SIZE          2           // header size
#define SRM_PACKET_HEADER_SIZE   20          // sync word + frame_complete_flag + program_id + stream_id + pts + dts

typedef struct SRMContext {
    const AVClass *class;
    int contain_program_count;
    uint32_t max_packet_size;
    AVRational time_base;
    int stream_count;
    uint32_t program_size;
    AVCodecParameters **codecpar;
    int64_t pts;
    int64_t dts;
    int frame_complete_flag_set;
    int program_id_set;
    int stream_id_set;
    int header_written;
} SRMContext;

static int srm_write_header(AVFormatContext *s)
{
    int i , j;
    SRMContext *srmc = s->priv_data;
    int srm_header_size = SRM_HEADER_SIZE + 1 + 4 + 8 + 2 + srmc->stream_count * 2;//srmc->stream_count=0,srm_header_size=17
    // Write SRM header
    avio_wb32(s->pb, SRM_SYNC_WORD);
    avio_wb16(s->pb, srm_header_size);//17
    // Write contain program count
    avio_w8(s->pb, srmc->contain_program_count);//1
    // Write max packet size
    avio_wb32(s->pb, srmc->max_packet_size);//4096
    // Write time base    
    srmc->time_base.num = 1;
    srmc->time_base.den = 44100 * 4;
    avio_wb64(s->pb, av_q2d(srmc->time_base));//num=1,den=44100: 1/44100 s
    // Write stream count
    srmc->stream_count = 1;
    avio_wb16(s->pb, srmc->stream_count);//0
    // Write stream headers
    for (i = 0; i < srmc->stream_count; i++) {
        const AVCodecParameters *par = srmc->codecpar[i];
        // Write codec type
        avio_w8(s->pb, 0x66);
        // Write codec ID
        avio_w8(s->pb, 0x66);
    }
    srmc->header_written = 1;
    return 0;
}

static int srm_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret = 0;
    SRMContext *srmc = s->priv_data;
    int64_t pts, dts, data_size;
    uint8_t *data;
    if (!srmc->header_written) {
        ret = srm_write_header(s);
        if (ret < 0) {
            return ret;
        }
    }
    pts = pkt->pts != AV_NOPTS_VALUE ? av_rescale_q(pkt->pts, s->streams[pkt->stream_index]->time_base, srmc->time_base) : 0;
    dts = pkt->dts != AV_NOPTS_VALUE ? av_rescale_q(pkt->dts, s->streams[pkt->stream_index]->time_base, srmc->time_base) : 0;
    data = pkt->data;
    data_size = pkt->size;
    srmc->frame_complete_flag_set = 0;
    while (data_size > 0) {
        uint32_t packet_size = srmc->max_packet_size - SRM_PACKET_HEADER_SIZE;
        // Write packet header
        avio_wb32(s->pb, SRM_PACKET_SYNC_WORD);
        if (!srmc->frame_complete_flag_set) {
            avio_w8(s->pb, 0x00);
        }else{
            avio_w8(s->pb, 0x01);
        }
        //if (!srmc->program_id_set) {
        avio_w8(s->pb, 0x66);
        //}
        //if (!srmc->stream_id_set) {
        avio_w8(s->pb, 0x66);
        //}
        if (pts != 0) {
            avio_wb64(s->pb, pts);
        } else {
            avio_wb64(s->pb, dts);
        }
        if (dts != 0) {
            avio_wb64(s->pb, dts);
        } else {
            avio_wb64(s->pb, pts);
        }
        if (data_size < packet_size) {
            packet_size = data_size;
            srmc->frame_complete_flag_set = 1;
        }
        if ( 0 < data_size - packet_size < packet_size) {
            srmc->frame_complete_flag_set = 1;
        }
        avio_write(s->pb, data, packet_size);
        data += packet_size;
        data_size -= packet_size;
        srmc->program_id_set = 1;
        srmc->stream_id_set = 1;
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
        srmc->pts = av_rescale_q(pkt->pts, s->streams[pkt->stream_index]->time_base, srmc->time_base);
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        srmc->dts = av_rescale_q(pkt->dts, s->streams[pkt->stream_index]->time_base, srmc->time_base);
    }
    return ret;
}

#define OFFSET(x) offsetof(SRMContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "contain_program_count", "The number of programs contained in the file.", OFFSET(contain_program_count), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 255, ENC },
    { "max_packet_size",       "The maximum size of a packet in bytes.",        OFFSET(max_packet_size),       AV_OPT_TYPE_INT, { .i64 = 4096 }, 0, INT_MAX, ENC },
    { NULL },
};

static const char *const srm_muxer_mime[] = {
    "application/octet-stream",
    NULL
};

static const AVCodecTag* const srm_muxer_tags[] = {
    ff_codec_bmp_tags,
    ff_codec_wav_tags,
    ff_codec_movvideo_tags,
    0
};

static const AVClass srm_muxer_class = {
    .class_name = "SRM muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_srm_muxer = {
    .name              = "srm",
    .long_name         = NULL_IF_CONFIG_SMALL("SRM (SquareRoute Media)"),
    .mime_type         = "audio/srm",
    .extensions        = "srm",
    .audio_codec       = AV_CODEC_ID_PCM_S16LE,
    .video_codec       = AV_CODEC_ID_NONE,
    .subtitle_codec    = AV_CODEC_ID_NONE,
    .write_header      = srm_write_header,
    .write_packet      = srm_write_packet,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_TS_NONSTRICT,
    .priv_data_size    = sizeof(SRMContext),
    .priv_class        = &srm_muxer_class,
    .codec_tag         = srm_muxer_tags,
};
