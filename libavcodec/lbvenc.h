#ifndef AVCODEC_LBVENC_H
#define AVCODEC_LBVENC_H
#include "bytestream.h"

typedef struct H2645SEILbvencEnhanceData {
    int present;
    uint8_t *layer1_data;
    uint32_t layer1_size;
    int layer1_roi_x;
    int layer1_roi_y;
    uint8_t *layer2_data;
    uint32_t layer2_size;
    int layer2_roi_x;
    int layer3_roi_y;

} H2645SEILbvencEnhanceData;

int lbvenc_enhance_data_decode(H2645SEILbvencEnhanceData *s,
                                               GetByteContext *gb,void *logctx);
int lbvenc_enhance_data_opaque_preprocess(H2645SEILbvencEnhanceData lbvenc_enhance_data,uint8_t** opaque);

enum AVCodecID lbvenc_common_trans_internal_base_codecid_to_codecid(int internal_id);
int lbvenc_common_trans_codecid_to_internal_base_codecid(enum AVCodecID);
#endif