#include "libavcodec/get_bits.h"
#include "libavcodec/golomb.h"
#include "libavcodec/bytestream.h"
#include "avformat.h"
#include "rawdec.h"
#include "libavutil/opt.h"
#include "libavcodec/hevc.h"
#include "libavcodec/h264.h"

#define MAX_FRAME_BLK 200
#define LBVC_UHS_RAW_PACKET_SIZE 1000000

static int lbvc_uhs_probe(const AVProbeData *p){
    GetByteContext gb;
    int32_t header = 0;
    bytestream2_init(&gb, p->buf, p->buf_size);
    int count;
    int type;

    header = bytestream2_get_be32(&gb);
    if(((header & 0xffff0000) >> 16) == 0xfffe){
        count = (header & 0x0000ffff);
    }else{
        return 0;
    }

    if((count > 0) && (count <= MAX_FRAME_BLK)){
        bytestream2_skip(&gb,2);
        bytestream2_skip(&gb,2);
        bytestream2_skip(&gb,2);
        bytestream2_skip(&gb,2);
        header = bytestream2_get_be32(&gb);
        if((((header & 0xFFFFFF00) >> 8) == 0x000001)){
            type = ((header & 0x000000FF) & 0x1F);
        }else if (header == 0x00000001){
            header = ((bytestream2_get_be16(&gb) & 0xFF00) >> 8 );
            type = (header & 0x1F);
        }

        if( type == H264_NAL_SPS ){
            return AVPROBE_SCORE_MAX;
        }else{
            return AVPROBE_SCORE_EXTENSION;
        }
    }

    return AVPROBE_SCORE_EXTENSION;
}

static int hlbvc_uhs_probe(const AVProbeData *p){
    GetByteContext gb;
    int32_t header = 0;
    bytestream2_init(&gb, p->buf, p->buf_size);
    int count;
    int type;

    header = bytestream2_get_be32(&gb);
    if(((header & 0xffff0000) >> 16) == 0xfffe){
        count = (header & 0x0000ffff);
    }else{
        return 0;
    }

    if((count > 0) && (count <= MAX_FRAME_BLK)){
        bytestream2_skip(&gb,2);
        bytestream2_skip(&gb,2);
        bytestream2_skip(&gb,2);
        bytestream2_skip(&gb,2);
        header = bytestream2_get_be32(&gb);
        if((((header & 0xFFFFFF00) >> 8) == 0x000001)){
            type = (((header & 0x000000FF) >> 1 ) & 0x3F);
        }else if (header == 0x00000001){
            header = ((bytestream2_get_be16(&gb) & 0xFF00) >> 8 );
            type = (((header) >> 1) & 0x3F);
        }

        if( type == HEVC_NAL_VPS ){
            return AVPROBE_SCORE_MAX;
        }else{
            return AVPROBE_SCORE_EXTENSION;
        }
    }

    return AVPROBE_SCORE_EXTENSION;
}


#define OFFSET(x) offsetof(FFRawVideoDemuxerContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption lbvc_uhs_rawvideo_options[] = {
    { "raw_packet_size", "", OFFSET(raw_packet_size), AV_OPT_TYPE_INT, {.i64 = LBVC_UHS_RAW_PACKET_SIZE }, 1, INT_MAX, DEC},
    { NULL },
};
#undef OFFSET

const AVClass ff_lbvc_uhs_rawvideo_demuxer_class = {
    .class_name = "lbvc uhs raw video demuxer",
    .item_name  = av_default_item_name,
    .option     = lbvc_uhs_rawvideo_options,
    .version    = LIBAVUTIL_VERSION_INT,
};
//raw_packet_size
//FF_DEF_RAWVIDEO_DEMUXER(lbvc_uhs, "Ultra High Resolution frame", lbvc_uhs_probe, "luhs,uhs", AV_CODEC_ID_LBVC_UHS)

const AVInputFormat ff_lbvc_uhs_demuxer = {
    .name           = "lbvc_uhs",
    .long_name      = NULL_IF_CONFIG_SMALL("Ultra High Resolution frame"),
    .read_probe     = lbvc_uhs_probe,
    .read_header    = ff_raw_video_read_header,
    .read_packet    = ff_raw_read_partial_packet,
    .extensions     = "luhs,uhs",
    .flags          = AVFMT_GENERIC_INDEX | AVFMT_NOTIMESTAMPS,
    .raw_codec_id   = AV_CODEC_ID_LBVC_UHS,
    .flags          = AVFMT_NOTIMESTAMPS,
    .priv_data_size = sizeof(FFRawVideoDemuxerContext),\
    .priv_class     = &ff_lbvc_uhs_rawvideo_demuxer_class,
};

const AVInputFormat ff_hlbvc_uhs_demuxer = {
    .name           = "hlbvc_uhs",
    .long_name      = NULL_IF_CONFIG_SMALL("High Effective Ultra High Resolution frame"),
    .read_probe     = hlbvc_uhs_probe,
    .read_header    = ff_raw_video_read_header,
    .read_packet    = ff_raw_read_partial_packet,
    .extensions     = "luhs,uhs",
    .flags          = AVFMT_GENERIC_INDEX | AVFMT_NOTIMESTAMPS,
    .raw_codec_id   = AV_CODEC_ID_HLBVC_UHS,
    .flags          = AVFMT_NOTIMESTAMPS,
    .priv_data_size = sizeof(FFRawVideoDemuxerContext),\
    .priv_class     = &ff_lbvc_uhs_rawvideo_demuxer_class,
};

