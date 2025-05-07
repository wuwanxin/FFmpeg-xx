/*
 * Copyright (c) 2022 NetInt
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

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#if HAVE_IO_H
#include <io.h>
#endif
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"
#include "ni_device_api.h"
#include "ni_util.h"
#include "nifilter.h"
#include "video.h"

#define NI_NUM_FRAMES_IN_QUEUE 8
#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 10
#define NMS_THRESH 0.45
#define BOX_THRESH 0.25
#define PER_MAX_DETECTIONS 1000
#define MAX_DETECTIONS 3000

#define out_roi_result 1
bool g_roi_enable = true;

typedef struct _ni_roi_network_layer {
    int32_t width;
    int32_t height;
    int32_t channel;
    int32_t classes;
    int32_t component;
    int32_t mask[3];
    float biases[12];
    int32_t output_number;
    float *output;
} ni_roi_network_layer_t;

typedef struct _ni_roi_network {
    int32_t netw;
    int32_t neth;
    ni_network_data_t raw;
    ni_roi_network_layer_t *layers;
} ni_roi_network_t;

typedef struct box {
    int x, y, w, h;
} box;


typedef struct detection {
    box bbox;
    float objectness;
    int classes;
    int color;
    float *prob;
    int prob_class;
    float max_prob;
} detection;

typedef struct detetion_cache {
    detection *dets;
    int capacity;
    int dets_num;
} detection_cache;

struct roi_box {
    int left;
    int right;
    int top;
    int bottom;
    int color;
    float objectness;
    int cls;
    float prob;
};

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} image_rect_t;

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;

typedef struct {
    int id;
    int count;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

inline static int clamp(float val, int min, int max) { return val > min ? (val < max ? val : max) : min; }

typedef struct HwScaleContext {
    ni_session_context_t api_ctx;
    ni_session_data_io_t api_dst_frame;
} HwScaleContext;

typedef struct AiContext {
    ni_session_context_t api_ctx;
    ni_session_data_io_t api_src_frame;
    ni_session_data_io_t api_dst_pkt;
} AiContext;

typedef struct NetIntRoiContext {
    const AVClass *class;
    const char *nb_file;  /* path to network binary */
    AVRational qp_offset; /* default qp offset. */
    int initialized;
    int devid;
    float obj_thresh;
    float nms_thresh;

    AiContext *ai_ctx;

    AVBufferRef *out_frames_ref;

    ni_roi_network_t network;
    detection_cache det_cache;
    struct SwsContext *img_cvt_ctx;
    AVFrame rgb_picture;

    HwScaleContext *hws_ctx;
    int keep_alive_timeout; /* keep alive timeout setting */
} NetIntRoiContext;

#define DFL_LEN 16
float exp_t[DFL_LEN];

static void compute_dfl(float* tensor, int dfl_len, float* box){
    for (int b=0; b<4; b++){
        float exp_sum=0;
        float acc_sum=0;
        for (int i=0; i< dfl_len; i++){
            exp_t[i] = exp(tensor[i+b*dfl_len]);
            exp_sum += exp_t[i];
        }
        
        for (int i=0; i< dfl_len; i++){
            acc_sum += exp_t[i]/exp_sum *i;
        }
        box[b] = acc_sum;
    }
}

static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                              float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static int quick_sort_indice_inverse(float* input, int left, int right, int* indices)
{
    float key;
    int key_index;
    int low = left;
    int high = right;

    if (left < right)
    {
        key_index = indices[left];
        key = input[left];

        while (low < high)
        {
            while (low < high && input[high] <= key)
            {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];

            while (low < high && input[low] >= key)
            {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }

        input[low] = key;
        indices[low] = key_index;

        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }

    return low;
}

static int nms(int validCount, float* outputLocations, int* classIds, int* order, int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
        {
            continue;
        }

        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
            {
                continue;
            }

            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = xmin0 + outputLocations[n * 4 + 2];
            float ymax0 = ymin0 + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = xmin1 + outputLocations[m * 4 + 2];
            float ymax1 = ymin1 + outputLocations[m * 4 + 3];

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold)
            {
                order[j] = -1;
            }
        }
    }

    return 0;
}

// 用于 qsort 的比较函数（升序）
static int compare_int(const void *a, const void *b) {
    return (*(int *)a - *(int *)b);
}

// 检查一个元素是否存在于数组中
static bool exists(int *arr, int size, int value) {
    for (int i = 0; i < size; i++) {
        if (arr[i] == value) return true;
    }
    return false;
}

// 去重函数：输入一个数组，返回去重后的数组长度，结果写入 class_set_out
static int deduplicate_classes(const int *classId, int classId_len, int *class_set_out) {
    int set_size = 0;
    for (int i = 0; i < classId_len; i++) {
        if (!exists(class_set_out, set_size, classId[i])) {
            class_set_out[set_size++] = classId[i];
        }
    }

    // 排序（可选）
    qsort(class_set_out, set_size, sizeof(int), compare_int);

    return set_size;
}

static int get_yolo_detections(float *box_tensor, float *score_tensor, float *score_sum_tensor, 
                        int grid_h, int grid_w, int strideh, int stridew, int dfl_len,
                        float *boxes, 
                        float *objProbs, 
                        int *classId, 
                        float threshold)
{

    // printf("threshold:%f\n", threshold);
    int grid2 = grid_h * grid_w;
    // printf("dfl_len:%d grid_len:%d stride:%d\n", dfl_len, grid2, stride);

    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int pad_h;//用于往外扩展一部分框 比例暂时设定为0.05W or 0.05H
    int pad_w;
    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i* grid_w + j;
            int max_class_id = -1;
            // 通过 score sum 起到快速过滤的作用
            if (score_sum_tensor != NULL){
                if (score_sum_tensor[offset] < threshold){
                    continue;
                }
            }

            float max_score = 0;
            for (int c= 0; c< OBJ_CLASS_NUM; c++){
                if ((score_tensor[offset] > threshold) && (score_tensor[offset] > max_score))
                {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }
           
            // compute box
            if (max_score> threshold && validCount < PER_MAX_DETECTIONS){
                offset = i* grid_w + j;
                float box[4];
                float before_dfl[DFL_LEN*4];
                for (int k=0; k< DFL_LEN*4; k++){
                    before_dfl[k] = box_tensor[offset];
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1,y1,x2,y2,w,h;
                x1 = (-box[0] + j + 0.5)*stridew;
                y1 = (-box[1] + i + 0.5)*strideh;
                x2 = (box[2] + j + 0.5)*stridew;
                y2 = (box[3] + i + 0.5)*strideh;
                w = x2 - x1;
                h = y2 - y1;
                pad_w = 0.05 * w;
                pad_h = 0.05 * h;
                boxes[validCount*4] = x1 - pad_w;
                boxes[validCount*4 + 1] = y1 - pad_h;
                boxes[validCount*4 + 2] = w + pad_w * 2;
                boxes[validCount*4 + 3] = h + pad_h * 2;


                objProbs[validCount] = max_score;
                classId[validCount] = max_class_id;
                validCount ++;
            }
        }
    }
    // printf("validCount: %d\n" ,validCount);
    return validCount;
}


float filterBoxes[MAX_DETECTIONS * 4];
float objProbs[MAX_DETECTIONS];
int classId[MAX_DETECTIONS];
int indexArray[MAX_DETECTIONS];


static int ni_get_detections(void *ctx, ni_roi_network_t *network,
                             detection_cache *det_cache, uint32_t img_width,
                             uint32_t img_height, float obj_thresh,
                             float nms_thresh, struct roi_box **roi_box,
                             int *roi_num)
{

    // printf("===============ni_get_detections===========\n");
    int i;
    int ret;
    int dets_num    = 0;
    detection *dets = NULL;

    *roi_box = NULL;
    *roi_num = 0;

    float x_factor = (float)img_width / network->netw;
    float y_factor = (float)img_height / network->neth; 

    // //malloc 
    // float* filterBoxes = (float*)malloc(sizeof(float) * MAX_DETECTIONS * 4);
    // float *objProbs = (float*)malloc(sizeof(float) * MAX_DETECTIONS);
    // int *classId = (int*)malloc(sizeof(int) * MAX_DETECTIONS);
    // int *indexArray = (int*)malloc(sizeof(int) * MAX_DETECTIONS);

    // memset(filterBoxes, 0, sizeof(float) * MAX_DETECTIONS * 4);
    // memset(objProbs, 0, sizeof(float) * MAX_DETECTIONS);
    // memset(classId, 0, sizeof(int) * MAX_DETECTIONS);
    // memset(indexArray, 0, sizeof(int) * MAX_DETECTIONS);

// YOU CAN SET grid_h grid_w model_in_w model_in_h grid[3]
    int validCount = 0;
    int strideh = 0;
    int stridew = 0;

    int grid_h;
    int grid_w;
    int model_in_w  = 640;
    int model_in_h = 384;
    int gridh[3] = {48, 24, 12};
    int gridw[3] = {80, 40, 20};

    object_detect_result_list od_results;
    memset(&od_results, 0, sizeof(object_detect_result_list));



    // default 3 branch

    int dfl_len = 16;//c/4
    int perCount = 0;
    int output_per_branch = 3;
    int step = 0;

    float *fileterBoxesPtr = filterBoxes;
    float *objProbsPtr = objProbs;
    int *classIdPtr = classId;
    int nc = 10;//number of classes 


    for (int i = 0; i < 3; i++)
    {
        grid_h = gridh[i];
        grid_w = gridw[i];

        // printf("grid_h: %d grid_w: %d\n",grid_h, grid_w);

        strideh = model_in_h / grid_h;
        stridew = model_in_w / grid_w;

        // printf("stride: %d\n",stride);
        int box_idx = 0;
        int score_idx = 64 * grid_h * grid_w;
        int score_sum_idx = score_idx + nc * grid_h * grid_w;
        
        float *score_sum = (float*)(network->layers[i].output + score_sum_idx);


       
     
        perCount = get_yolo_detections((float*)network->layers[i].output, (float*)(network->layers[i].output + score_idx) , (float *)score_sum,
                                       grid_h, grid_w, strideh, stridew,  dfl_len, 
                                       fileterBoxesPtr + validCount * 4, objProbsPtr + validCount, classIdPtr + validCount, obj_thresh);
    
        validCount += perCount;
        // printf("percount:%d\n", perCount);
    }


    // no object detect
    if (validCount <= 0)
    {
        return 0;
    }
    
    for (int i = 0; i < validCount; ++i)
    {
        indexArray[i] = i;
    }
    quick_sort_indice_inverse(objProbs, 0, validCount, indexArray);

    // printf("after quick_sort_indice_inverse validCount: %d\n" ,validCount);

    int class_set[64]; // 预分配一个足够大的数组存放不重复的类别
    int class_set_len = deduplicate_classes(classId, validCount, class_set);

    // printf("class_set_len:%d\n", class_set_len);

    for (int i = 0; i < class_set_len; i++) {
        int class_id = class_set[i];
        nms(validCount, filterBoxes, classId, indexArray, class_id, nms_thresh);
    }
    // printf("after nms validCount: %d\n" ,validCount);

    int last_count = 0;
    // od_results->count = 0;
    // printf("validCount: %d\n" ,validCount);


    /* box valid detect target */
    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
        {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[i];
        if(id > 1)// only detect pedestrian and people
        {
            continue;
        }

        od_results.results[last_count].box.left = (int)(clamp(x1, 0, model_in_w) * x_factor );
        od_results.results[last_count].box.top = (int)(clamp(y1, 0, model_in_h) * y_factor );
        od_results.results[last_count].box.right = (int)(clamp(x2, 0, model_in_w) * x_factor);
        od_results.results[last_count].box.bottom = (int)(clamp(y2, 0, model_in_h) * y_factor);
        od_results.results[last_count].prop = obj_conf;
        od_results.results[last_count].cls_id = id;
        last_count++;
        // printf("=========obj_conf:%f========\n", obj_conf);
    }
    od_results.count = last_count;


    // //free

    // if (filterBoxes) free(filterBoxes);
    // if (objProbs) free(objProbs);
    // if (classId) free(classId);
    // if (indexArray) free(indexArray);

    struct roi_box *rbox;
    rbox = malloc(sizeof(struct roi_box) * od_results.count);
    if (!rbox)
        return AVERROR(ENOMEM);


    int rbox_num = 0;
    int left, right, top, bot;
    for (i = 0; i < od_results.count; i++) {
        rbox[rbox_num].left       = od_results.results[i].box.left;
        rbox[rbox_num].right      = od_results.results[i].box.right;
        rbox[rbox_num].top        = od_results.results[i].box.top;
        rbox[rbox_num].bottom     = od_results.results[i].box.bottom;
        rbox[rbox_num].cls        = od_results.results[i].cls_id;
        rbox[rbox_num].color      = 0;
        rbox[rbox_num].prob       = od_results.results[i].prop;
        //printf("=====prob:%f cls:%d left:%d right:%d top:%d bottom:%d=====\n", rbox[rbox_num].prob, rbox[rbox_num].cls, rbox[rbox_num].left, rbox[rbox_num].right, rbox[rbox_num].top, rbox[rbox_num].bottom );
        rbox_num++;  
    }
    //printf("rbox_num:%d\n", rbox_num);
    if (rbox_num == 0) {
        free(rbox);
        *roi_num = rbox_num;
        *roi_box = NULL;
    } else {
        *roi_num = rbox_num;
        *roi_box = rbox;
    }
    return 0;
}


static int ni_roi_query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;

    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_NI_QUAD,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE,
    };

    formats = ff_make_format_list(pix_fmts);
    if (!formats)
        return AVERROR(ENOMEM);

    return ff_set_common_formats(ctx, formats);
}

static void cleanup_ai_context(AVFilterContext *ctx, NetIntRoiContext *s)
{
    ni_retcode_t retval;
    AiContext *ai_ctx = s->ai_ctx;

    if (ai_ctx) {
        ni_frame_buffer_free(&ai_ctx->api_src_frame.data.frame);
        ni_packet_buffer_free(&ai_ctx->api_dst_pkt.data.packet);

        retval =
            ni_device_session_close(&ai_ctx->api_ctx, 1, NI_DEVICE_TYPE_AI);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR,
                   "%s: failed to close ai session. retval %d\n", __func__,
                   retval);
        }
        ni_device_session_context_clear(&ai_ctx->api_ctx);
        av_free(ai_ctx);
        s->ai_ctx = NULL;
    }
}

static int init_ai_context(AVFilterContext *ctx, NetIntRoiContext *s,
                           AVFrame *frame)
{
    ni_retcode_t retval;
    AiContext *ai_ctx;
    ni_roi_network_t *network = &s->network;
    int ret;
    int hwframe = frame->format == AV_PIX_FMT_NI_QUAD ? 1 : 0;

#if HAVE_IO_H
    if ((s->nb_file == NULL) || (_access(s->nb_file, R_OK) != 0)) {
#else
    if ((s->nb_file == NULL) || (access(s->nb_file, R_OK) != 0)) {
#endif
        av_log(ctx, AV_LOG_ERROR, "invalid network binary path\n");
        return AVERROR(EINVAL);
    }

    ai_ctx = av_mallocz(sizeof(AiContext));
    if (!ai_ctx) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate ai context\n");
        return AVERROR(ENOMEM);
    }

    retval = ni_device_session_context_init(&ai_ctx->api_ctx);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "ai session context init failure\n");
        return AVERROR(EIO);
    }

    if (hwframe) {
        AVHWFramesContext *pAVHFWCtx;
        AVNIDeviceContext *pAVNIDevCtx;
        int cardno;

        pAVHFWCtx   = (AVHWFramesContext *)frame->hw_frames_ctx->data;
        pAVNIDevCtx = (AVNIDeviceContext *)pAVHFWCtx->device_ctx->hwctx;
        cardno      = ni_get_cardno(frame);

        ai_ctx->api_ctx.device_handle = pAVNIDevCtx->cards[cardno];
        ai_ctx->api_ctx.blk_io_handle = pAVNIDevCtx->cards[cardno];
        ai_ctx->api_ctx.hw_action     = NI_CODEC_HW_ENABLE;
        ai_ctx->api_ctx.hw_id         = cardno;
    } else
        ai_ctx->api_ctx.hw_id = s->devid;

    ai_ctx->api_ctx.device_type = NI_DEVICE_TYPE_AI;
    ai_ctx->api_ctx.keep_alive_timeout = s->keep_alive_timeout;
    retval = ni_device_session_open(&ai_ctx->api_ctx, NI_DEVICE_TYPE_AI);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "failed to open ai session. retval %d\n",
               retval);
        return AVERROR(EIO);
    }

    retval = ni_ai_config_network_binary(&ai_ctx->api_ctx, &network->raw,
                                         s->nb_file);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "failed to configure ai session. retval %d\n",
               retval);
        ret = AVERROR(EIO);
        goto failed_out;
    }

    if (!hwframe) {
        retval = ni_ai_frame_buffer_alloc(&ai_ctx->api_src_frame.data.frame,
                                          &network->raw);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "failed to allocate ni frame\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }
    }

    retval = ni_ai_packet_buffer_alloc(&ai_ctx->api_dst_pkt.data.packet,
                                       &network->raw);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate ni packet\n");
        ret = AVERROR(ENOMEM);
        goto failed_out;
    }

    s->ai_ctx = ai_ctx;
    return 0;

failed_out:
    cleanup_ai_context(ctx, s);
    return ret;
}

static void ni_destroy_network(AVFilterContext *ctx, ni_roi_network_t *network)
{
    if (network) {
        int i;

        if (network->layers) {
            for (i = 0; i < network->raw.output_num; i++) {
                if (network->layers[i].output) {
                    free(network->layers[i].output);
                    network->layers[i].output = NULL;
                }
            }

            free(network->layers);
            network->layers = NULL;
        }
    }
}

static int ni_create_network(AVFilterContext *ctx, ni_roi_network_t *network)
{
    int ret;
    int i;
    ni_network_data_t *ni_network = &network->raw;

    av_log(ctx, AV_LOG_VERBOSE, "network input number %d, output number %d\n",
           ni_network->input_num, ni_network->output_num);
    

    if (ni_network->input_num == 0 || ni_network->output_num == 0) {
        av_log(ctx, AV_LOG_ERROR, "invalid network layer\n");
        return AVERROR(EINVAL);
    }

    /* only support one input for now */
    if (ni_network->input_num != 1) {
        av_log(ctx, AV_LOG_ERROR,
               "network input layer number %d not supported\n",
               ni_network->input_num);
        return AVERROR(EINVAL);
    }

    network->layers =
        malloc(sizeof(ni_roi_network_layer_t) * ni_network->output_num);
    if (!network->layers) {
        av_log(ctx, AV_LOG_ERROR, "cannot allocate network layer memory\n");
        return AVERROR(ENOMEM);
    }
    memset(network->layers, 0,
           sizeof(ni_roi_network_layer_t) * ni_network->output_num);

    for (i = 0; i < ni_network->output_num; i++) {
        network->layers[i].width     = ni_network->linfo.out_param[i].sizes[0];
        network->layers[i].height    = ni_network->linfo.out_param[i].sizes[1];
        network->layers[i].channel   = ni_network->linfo.out_param[i].sizes[2];
        network->layers[i].output_number = ni_ai_network_layer_dims(&ni_network->linfo.out_param[i]);
        av_assert0(network->layers[i].output_number ==
                   network->layers[i].width * network->layers[i].height *
                       network->layers[i].channel);
        network->layers[i].output = malloc(network->layers[i].output_number * sizeof(float));
        if (!network->layers[i].output) {
            av_log(ctx, AV_LOG_ERROR,
                   "failed to allocate network layer %d output buffer\n", i);
            ret = AVERROR(ENOMEM);
            goto out;
        }
    }
    network->netw = ni_network->linfo.in_param[0].sizes[0];
    network->neth = ni_network->linfo.in_param[0].sizes[1];

    return 0;
out:
    ni_destroy_network(ctx, network);
    return ret;
}

static av_cold int init_hwframe_scale(AVFilterContext *ctx, NetIntRoiContext *s,
                                      enum AVPixelFormat format, AVFrame *frame)
{
    ni_retcode_t retval;
    HwScaleContext *hws_ctx;
    int ret;
    AVHWFramesContext *pAVHFWCtx;
    AVNIDeviceContext *pAVNIDevCtx;
    int cardno;

    hws_ctx = av_mallocz(sizeof(HwScaleContext));
    if (!hws_ctx) {
        av_log(ctx, AV_LOG_ERROR, "could not allocate hwframe ctx\n");
        return AVERROR(ENOMEM);
    }

    retval = ni_device_session_context_init(&hws_ctx->api_ctx);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "hw scaler session context init failure\n");
        return AVERROR(EIO);
    }

    pAVHFWCtx   = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    pAVNIDevCtx = (AVNIDeviceContext *)pAVHFWCtx->device_ctx->hwctx;
    cardno      = ni_get_cardno(frame);

    hws_ctx->api_ctx.device_handle     = pAVNIDevCtx->cards[cardno];
    hws_ctx->api_ctx.blk_io_handle     = pAVNIDevCtx->cards[cardno];
    hws_ctx->api_ctx.device_type       = NI_DEVICE_TYPE_SCALER;
    hws_ctx->api_ctx.scaler_operation  = NI_SCALER_OPCODE_SCALE;
    hws_ctx->api_ctx.hw_id             = cardno;
    hws_ctx->api_ctx.keep_alive_timeout = s->keep_alive_timeout;

    retval = ni_device_session_open(&hws_ctx->api_ctx, NI_DEVICE_TYPE_SCALER);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "could not open scaler session\n");
        ret = AVERROR(EIO);
        goto out;
    }

    /* Create scale frame pool on device */
    retval = ff_ni_build_frame_pool(&hws_ctx->api_ctx, s->network.netw,
                                    s->network.neth, format,
                                    DEFAULT_NI_FILTER_POOL_SIZE);
    if (retval < 0) {
        av_log(ctx, AV_LOG_ERROR, "could not build frame pool\n");
        ni_device_session_close(&hws_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear(&hws_ctx->api_ctx);
        ret = AVERROR(EIO);
        goto out;
    }

    s->hws_ctx = hws_ctx;
    return 0;
out:
    av_free(hws_ctx);
    return ret;
}

static void cleanup_hwframe_scale(AVFilterContext *ctx, NetIntRoiContext *s)
{
    HwScaleContext *hws_ctx = s->hws_ctx;

    if (hws_ctx) {
        ni_frame_buffer_free(&hws_ctx->api_dst_frame.data.frame);
        ni_device_session_close(&hws_ctx->api_ctx, 1, NI_DEVICE_TYPE_SCALER);
        ni_device_session_context_clear(&hws_ctx->api_ctx);

        av_free(hws_ctx);
        s->hws_ctx = NULL;
    }
}

static int ni_roi_config_input(AVFilterContext *ctx, AVFrame *frame)
{
    NetIntRoiContext *s = ctx->priv;
    int ret;

    if (s->initialized)
        return 0;

    ret = init_ai_context(ctx, s, frame);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "failed to initialize ai context\n");
        return ret;
    }

    ret = ni_create_network(ctx, &s->network);
    if (ret != 0) {
        goto fail_out;
    }

    if (frame->format != AV_PIX_FMT_NI_QUAD) {
        memset(&s->rgb_picture, 0, sizeof(s->rgb_picture));
        s->rgb_picture.width  = s->network.netw;
        s->rgb_picture.height = s->network.neth;
        s->rgb_picture.format = AV_PIX_FMT_RGB24;
        if (av_frame_get_buffer(&s->rgb_picture, 32)) {
            av_log(ctx, AV_LOG_ERROR, "Out of memory for RGB pack data!\n");
            goto fail_out;
        }

        s->img_cvt_ctx = sws_getContext(frame->width, frame->height,
                                        frame->format, s->network.netw,
                                        s->network.neth, s->rgb_picture.format,
                                        SWS_BICUBIC, NULL, NULL, NULL);
        
        if (!s->img_cvt_ctx) {
            av_log(ctx, AV_LOG_ERROR,
                   "could not create SwsContext for conversion and scaling\n");
            ret = AVERROR(ENOMEM);
            goto fail_out;
        }
    } else {
        ret = init_hwframe_scale(ctx, s, AV_PIX_FMT_BGRP, frame);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "could not initialized hwframe scale context\n");
            goto fail_out;
        }
    }

    s->initialized = 1;
    return 0;

fail_out:
    cleanup_ai_context(ctx, s);

    ni_destroy_network(ctx, &s->network);

    av_frame_unref(&s->rgb_picture);
    if (s->img_cvt_ctx) {
        sws_freeContext(s->img_cvt_ctx);
        s->img_cvt_ctx = NULL;
    }
    return ret;
}

static av_cold int ni_roi_init(AVFilterContext *ctx)
{
    NetIntRoiContext *s = ctx->priv;

    s->det_cache.dets_num = 0;
    s->det_cache.capacity = 20;
    s->det_cache.dets     = malloc(sizeof(detection) * s->det_cache.capacity);
    if (!s->det_cache.dets) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate detection cache\n");
        return AVERROR(ENOMEM);
    }

    return 0;
}

static av_cold void ni_roi_uninit(AVFilterContext *ctx)
{
    NetIntRoiContext *s       = ctx->priv;
    ni_roi_network_t *network = &s->network;

    cleanup_ai_context(ctx, s);

    ni_destroy_network(ctx, network);

    if (s->det_cache.dets) {
        free(s->det_cache.dets);
        s->det_cache.dets = NULL;
    }

    av_buffer_unref(&s->out_frames_ref);
    s->out_frames_ref = NULL;

    av_frame_unref(&s->rgb_picture);
    sws_freeContext(s->img_cvt_ctx);
    s->img_cvt_ctx = NULL;

    cleanup_hwframe_scale(ctx, s);
}

static int ni_roi_output_config_props(AVFilterLink *outlink)
{
    if(g_roi_enable == false){
        // 保持输出参数与输入一致
        AVFilterLink *inlink = outlink->src->inputs[0];
        
        outlink->time_base = inlink->time_base;
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
        outlink->w = inlink->w;
        outlink->h = inlink->h;
        
        return 0;
    }
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    AVHWFramesContext *in_frames_ctx;
    AVHWFramesContext *out_frames_ctx;
    NetIntRoiContext *s = ctx->priv;

    if ((inlink->hw_frames_ctx == NULL) && (inlink->format == AV_PIX_FMT_NI_QUAD)) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }

    if (inlink->hw_frames_ctx == NULL) {
        av_log(ctx, AV_LOG_DEBUG, "sw frame\n");
        return 0;
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;

    in_frames_ctx = (AVHWFramesContext *)ctx->inputs[0]->hw_frames_ctx->data;

    if (in_frames_ctx->sw_format == AV_PIX_FMT_BGRP) {
        av_log(ctx, AV_LOG_ERROR, "bgrp not supported\n");
        return AVERROR(EINVAL);
    }
    if (in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_8_TILE_4X4 ||
        in_frames_ctx->sw_format == AV_PIX_FMT_NI_QUAD_10_TILE_4X4) {
        av_log(ctx, AV_LOG_ERROR, "tile4x4 not supported\n");
        return AVERROR(EINVAL);
    }


    s->out_frames_ref = av_hwframe_ctx_alloc(in_frames_ctx->device_ref);
    if (!s->out_frames_ref)
        return AVERROR(ENOMEM);

    out_frames_ctx = (AVHWFramesContext *)s->out_frames_ref->data;

    ff_ni_clone_hwframe_ctx(in_frames_ctx, out_frames_ctx, &s->ai_ctx->api_ctx);

    out_frames_ctx->format            = AV_PIX_FMT_NI_QUAD;
    out_frames_ctx->width             = outlink->w;
    out_frames_ctx->height            = outlink->h;
    out_frames_ctx->sw_format         = in_frames_ctx->sw_format;
    out_frames_ctx->initial_pool_size = NI_ROI_ID;

    av_hwframe_ctx_init(s->out_frames_ref);

    av_buffer_unref(&ctx->outputs[0]->hw_frames_ctx);
    ctx->outputs[0]->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);

    if (!ctx->outputs[0]->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static int ni_read_roi(AVFilterContext *ctx, ni_session_data_io_t *p_dst_pkt,
                       AVFrame *out, int pic_width, int pic_height)
{
    // printf("==============ni_read_roi============\n");
    NetIntRoiContext *s = ctx->priv;
    ni_retcode_t retval;
    ni_roi_network_t *network = &s->network;
    AVFrameSideData *sd;
    AVFrameSideData *sd_roi_extra;
    AVRegionOfInterest *roi;
    AVRegionOfInterestNetintExtra *roi_extra;
    struct roi_box *roi_box = NULL;
    int roi_num             = 0;
    int ret;
    int i;
    int width, height;

    for (i = 0; i < network->raw.output_num; i++) {
        retval = ni_network_layer_convert_output(
            network->layers[i].output,
            network->layers[i].output_number * sizeof(float),
            &p_dst_pkt->data.packet, &network->raw, i);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR,
                   "failed to read layer %d output. retval %d\n", i, retval);
            return AVERROR(EIO);
        }
    }

    width  = pic_width;
    height = pic_height;

    s->det_cache.dets_num = 0;
    //YOU CAN SET obj_thresh nms_thresh
    s->obj_thresh = 0.25;
    s->nms_thresh = 0.45;
    ret = ni_get_detections(ctx, network, &s->det_cache, width, height,
                            s->obj_thresh, s->nms_thresh, &roi_box, &roi_num);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "failed to get roi.\n");
        return ret;
    }

    if (roi_num == 0) {
        av_log(ctx, AV_LOG_DEBUG, "no roi available\n");
        return 0;
    }

    sd = av_frame_new_side_data(out, AV_FRAME_DATA_REGIONS_OF_INTEREST,
                                (int)(roi_num * sizeof(AVRegionOfInterest)));
    sd_roi_extra = av_frame_new_side_data(
        out, AV_FRAME_DATA_NETINT_REGIONS_OF_INTEREST_EXTRA,
        (int)(roi_num * sizeof(AVRegionOfInterestNetintExtra)));
    if (!sd || !sd_roi_extra) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate roi sidedata\n");
        free(roi_box);
        return AVERROR(ENOMEM);
    }

    roi = (AVRegionOfInterest *)sd->data;
    roi_extra = (AVRegionOfInterestNetintExtra *)sd_roi_extra->data;

    //FILE* fp = fopen("roi_result.txt","a");
    //static count=0;
    for (i = 0; i < roi_num; i++) {
        roi[i].self_size = sizeof(*roi);
        roi[i].top       = roi_box[i].top;
        roi[i].bottom    = roi_box[i].bottom;
        roi[i].left      = roi_box[i].left;
        roi[i].right     = roi_box[i].right;
        roi[i].qoffset   = s->qp_offset;
        roi_extra[i].self_size = sizeof(*roi_extra);
        roi_extra[i].cls       = roi_box[i].cls;
        roi_extra[i].prob      = roi_box[i].prob;
        av_log(ctx, AV_LOG_DEBUG,
               "roi %d: top %d, bottom %d, left %d, right %d, qpo %d/%d\n", i,
               roi[i].top, roi[i].bottom, roi[i].left, roi[i].right,
               roi[i].qoffset.num, roi[i].qoffset.den);
        
        //maqg_test
        //fprintf(fp, "drawbox=enable='eq(n,%d)':x=%d:y=%d:w=%d:h=%d:color=red@0.5,\\\n",
        //    count,roi[i].left, roi[i].top, roi[i].right-roi[i].left,roi[i].bottom-roi[i].top);
        // usleep(100000);
    }
    //count++;
    //fclose(fp);

    // sleep(1);  // 睡眠 1 秒 //zjq add
    free(roi_box);
    return 0;
}

static int ni_recreate_frame(ni_frame_t *ni_frame, AVFrame *frame)
{
    uint8_t *p_data = ni_frame->p_data[0];

    av_log(NULL, AV_LOG_DEBUG,
           "linesize %d/%d/%d, data %p/%p/%p, pixel %dx%d\n",
           frame->linesize[0], frame->linesize[1], frame->linesize[2],
           frame->data[0], frame->data[1], frame->data[2], frame->width,
           frame->height);

    if (frame->format == AV_PIX_FMT_GBRP) {
        int i;
        /* GBRP -> BGRP */
        for (i = 0; i < frame->height; i++) {
            memcpy((void *)(p_data + i * frame->linesize[1]),
                   frame->data[1] + i * frame->linesize[1], frame->linesize[1]);
        }

        p_data += frame->height * frame->linesize[1];
        for (i = 0; i < frame->height; i++) {
            memcpy((void *)(p_data + i * frame->linesize[0]),
                   frame->data[0] + i * frame->linesize[0], frame->linesize[0]);
        }

        p_data += frame->height * frame->linesize[0];
        for (i = 0; i < frame->height; i++) {
            memcpy((void *)(p_data + i * frame->linesize[2]),
                   frame->data[2] + i * frame->linesize[2], frame->linesize[2]);
        }
    } else if (frame->format == AV_PIX_FMT_RGB24) {
        /* RGB24 -> BGRP */
        uint8_t *r_data = p_data + frame->width * frame->height * 2;
        uint8_t *g_data = p_data + frame->width * frame->height * 1;
        uint8_t *b_data = p_data + frame->width * frame->height * 0;
        uint8_t *fdata  = frame->data[0];
        int x, y;

        av_log(NULL, AV_LOG_DEBUG,
               "%s(): rgb24 to bgrp, pix %dx%d, linesize %d\n", __func__,
               frame->width, frame->height, frame->linesize[0]);

        for (y = 0; y < frame->height; y++) {
            for (x = 0; x < frame->width; x++) {
                int fpos  = y * frame->linesize[0];
                int ppos  = y * frame->width;
                uint8_t r = fdata[fpos + x * 3 + 0];
                uint8_t g = fdata[fpos + x * 3 + 1];
                uint8_t b = fdata[fpos + x * 3 + 2];

                r_data[ppos + x] = r;
                g_data[ppos + x] = g;
                b_data[ppos + x] = b;
            }
        }
    }
    return 0;
}

static int ni_hwframe_scale(AVFilterContext *ctx, NetIntRoiContext *s,
                            AVFrame *in, int w, int h,
                            niFrameSurface1_t **filt_frame_surface)
{
    HwScaleContext *scale_ctx = s->hws_ctx;
    int scaler_format;
    ni_retcode_t retcode;
    niFrameSurface1_t *frame_surface, *new_frame_surface;
    AVHWFramesContext *pAVHFWCtx;

    frame_surface = (niFrameSurface1_t *)in->data[3];

    av_log(ctx, AV_LOG_DEBUG, "in frame surface frameIdx %d\n",
           frame_surface->ui16FrameIdx);

    pAVHFWCtx = (AVHWFramesContext *)in->hw_frames_ctx->data;

    scaler_format = ff_ni_ffmpeg_to_gc620_pix_fmt(pAVHFWCtx->sw_format);

    retcode = ni_frame_buffer_alloc_hwenc(&scale_ctx->api_dst_frame.data.frame,
                                          w, h, 0);
    if (retcode != NI_RETCODE_SUCCESS)
        return AVERROR(ENOMEM);

    /*
     * Allocate device input frame. This call won't actually allocate a frame,
     * but sends the incoming hardware frame index to the scaler manager
     */
    retcode = ni_device_alloc_frame(
        &scale_ctx->api_ctx, FFALIGN(in->width, 2), FFALIGN(in->height, 2),
        scaler_format, 0, 0, 0, 0, 0, frame_surface->ui32nodeAddress,
        frame_surface->ui16FrameIdx, NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS) {
        av_log(NULL, AV_LOG_DEBUG, "Can't allocate device input frame %d\n",
               retcode);
        return AVERROR(ENOMEM);
    }

    /* Allocate hardware device destination frame. This acquires a frame from
     * the pool */
    retcode = ni_device_alloc_frame(
        &scale_ctx->api_ctx, FFALIGN(w, 2), FFALIGN(h, 2),
        ff_ni_ffmpeg_to_gc620_pix_fmt(AV_PIX_FMT_BGRP), NI_SCALER_FLAG_IO, 0, 0,
        0, 0, 0, -1, NI_DEVICE_TYPE_SCALER);

    if (retcode != NI_RETCODE_SUCCESS) {
        av_log(NULL, AV_LOG_DEBUG, "Can't allocate device output frame %d\n",
               retcode);
        return AVERROR(ENOMEM);
    }

    /* Set the new frame index */
    ni_device_session_read_hwdesc(
        &scale_ctx->api_ctx, &scale_ctx->api_dst_frame, NI_DEVICE_TYPE_SCALER);
    new_frame_surface =
        (niFrameSurface1_t *)scale_ctx->api_dst_frame.data.frame.p_data[3];

    *filt_frame_surface = new_frame_surface;

    return 0;
}

static int ni_roi_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    NetIntRoiContext *s  = ctx->priv;

    g_roi_enable = true;
    FILE *file = fopen("./roi_ctrl/switch.txt", "r");
    if (file) {
        float value;
        if (fscanf(file, "%f", &value) == 1) {
            fclose(file);
            if (value == 0.0) {
                g_roi_enable = false;
                return ff_filter_frame(link->dst->outputs[0], in);
            }
            else if(value >= -1.0 && value <= 1.0){
                s->qp_offset = av_d2q(value, 100);
            }
        } else {
            fclose(file);
        }
    }

    AVFrame *out         = NULL;
    ni_roi_network_t *network;
    ni_retcode_t retval;
    int ret;
    AiContext *ai_ctx;

    if (in == NULL) {
        av_log(ctx, AV_LOG_WARNING, "in frame is null\n");
        return AVERROR(EINVAL);
    }

    if (!s->initialized) {
        ret = ni_roi_config_input(ctx, in);
        if (ret) {
            av_log(ctx, AV_LOG_ERROR, "failed to config input\n");
            return ret;
        }
    }

    ai_ctx  = s->ai_ctx;
    network = &s->network;
    retval  = ni_ai_packet_buffer_alloc(&ai_ctx->api_dst_pkt.data.packet,
                                       &network->raw);
    if (retval != NI_RETCODE_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "failed to allocate packet\n");
        return AVERROR(EAGAIN);
    }

    out = av_frame_clone(in);
    if (!out)
        return AVERROR(ENOMEM);

#ifdef NI_MEASURE_LATENCY
    ff_ni_update_benchmark(NULL);
#endif

    if (in->format == AV_PIX_FMT_NI_QUAD) {
        niFrameSurface1_t *filt_frame_surface;

        ret = ni_hwframe_scale(ctx, s, in, network->netw, network->neth,
                               &filt_frame_surface);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error run hwframe scale\n");
            goto failed_out;
        }

        av_log(ctx, AV_LOG_DEBUG, "filt frame surface frameIdx %d\n",
               filt_frame_surface->ui16FrameIdx);

        /* allocate output buffer */
        retval = ni_device_alloc_frame(&ai_ctx->api_ctx, 0, 0, 0, 0, 0, 0, 0, 0,
                                       filt_frame_surface->ui32nodeAddress,
                                       filt_frame_surface->ui16FrameIdx,
                                       NI_DEVICE_TYPE_AI);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "failed to alloc hw input frame\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }

        do {
            retval = ni_device_session_read(
                &ai_ctx->api_ctx, &ai_ctx->api_dst_pkt, NI_DEVICE_TYPE_AI);
            if (retval < 0) {
                av_log(ctx, AV_LOG_ERROR, "read hwdesc retval %d\n", retval);
                ret = AVERROR(EIO);
                goto failed_out;
            } else if (retval > 0) {
                ret = ni_read_roi(ctx, &ai_ctx->api_dst_pkt, out, out->width,
                                  out->height);
                if (ret != 0) {
                    av_log(ctx, AV_LOG_ERROR,
                           "failed to read roi from packet\n");
                    goto failed_out;
                }
            }
        } while (retval == 0);

        ni_hwframe_buffer_recycle(filt_frame_surface,
                                  filt_frame_surface->device_handle);

        av_buffer_unref(&out->hw_frames_ctx);
        /* Reference the new hw frames context */
        out->hw_frames_ctx = av_buffer_ref(s->out_frames_ref);
    } else {
        // printf("=====================0========================\n");
        ret = sws_scale(s->img_cvt_ctx, (const uint8_t *const *)in->data,
                        in->linesize, 0, in->height, s->rgb_picture.data,
                        s->rgb_picture.linesize);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "failed to do sws scale\n");
            goto failed_out;
        }

        retval = ni_ai_frame_buffer_alloc(&ai_ctx->api_src_frame.data.frame,
                                          &network->raw);
        if (retval != NI_RETCODE_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "cannot allocate ai frame\n");
            ret = AVERROR(ENOMEM);
            goto failed_out;
        }

        ret = ni_recreate_frame(&ai_ctx->api_src_frame.data.frame,
                                &s->rgb_picture);//nhwc->nchw rgb->bgr
        if (ret != 0) {
            av_log(ctx, AV_LOG_ERROR, "cannot re-create ai frame\n");
            goto failed_out;
        }
        // printf("=====================1========================\n");


        /* write frame */
        do {
            retval = ni_device_session_write(
                &ai_ctx->api_ctx, &ai_ctx->api_src_frame, NI_DEVICE_TYPE_AI);
            if (retval < 0) {
                av_log(ctx, AV_LOG_ERROR,
                       "failed to write ai session: retval %d\n", retval);
                ret = AVERROR(EIO);
                goto failed_out;
            }
        } while (retval == 0);
        // printf("=====================2========================\n");
        
        /* read roi result */
        do {
            retval = ni_device_session_read(
                &ai_ctx->api_ctx, &ai_ctx->api_dst_pkt, NI_DEVICE_TYPE_AI);
                // printf("=====================3========================\n");
            if (retval < 0) {
                av_log(ctx, AV_LOG_ERROR, "read hwdesc retval %d\n", retval);
                ret = AVERROR(EIO);
                goto failed_out;
            } else if (retval > 0) {
        // printf("=====================4========================\n");
                ret = ni_read_roi(ctx, &ai_ctx->api_dst_pkt, out, out->width,
                                  out->height);
                if (ret != 0) {
                    av_log(ctx, AV_LOG_ERROR,
                           "failed to read roi from packet\n");
                    goto failed_out;
                }
            }
        } while (retval == 0);
    }

#ifdef NI_MEASURE_LATENCY
    ff_ni_update_benchmark("ni_quadra_roi");
#endif

    av_frame_free(&in);
    return ff_filter_frame(link->dst->outputs[0], out);

failed_out:
    if (out)
        av_frame_free(&out);

    av_frame_free(&in);
    return ret;
}

#define OFFSET(x) offsetof(NetIntRoiContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption ni_roi_options[] = {{"nb", "path to network binary file",
                                           OFFSET(nb_file), AV_OPT_TYPE_STRING,
                                           .flags = FLAGS},
                                          {"qpoffset",
                                           "qp offset ratio",
                                           OFFSET(qp_offset),
                                           AV_OPT_TYPE_RATIONAL,
                                           {.dbl = 0},
                                           -1.0,
                                           1.0,
                                           .flags = FLAGS,
                                           "range"},
                                          {"devid",
                                           "device to operate in swframe mode",
                                           OFFSET(devid),
                                           AV_OPT_TYPE_INT,
                                           {.i64 = 0},
                                           -1,
                                           INT_MAX,
                                           .flags = FLAGS,
                                           "range"},
                                          {"obj_thresh",
                                           "objectness thresh",
                                           OFFSET(obj_thresh),
                                           AV_OPT_TYPE_FLOAT,
                                           {.dbl = 0.25},
                                           -FLT_MAX,
                                           FLT_MAX,
                                           .flags = FLAGS,
                                           "range"},
                                          {"nms_thresh",
                                           "nms thresh",
                                           OFFSET(nms_thresh),
                                           AV_OPT_TYPE_FLOAT,
                                           {.dbl = 0.45},
                                           -FLT_MAX,
                                           FLT_MAX,
                                           .flags = FLAGS,
                                           "range"},

    {"keep_alive_timeout",
     "Specify a custom session keep alive timeout in seconds.",
     OFFSET(keep_alive_timeout),
     AV_OPT_TYPE_INT,
     {.i64 = NI_DEFAULT_KEEP_ALIVE_TIMEOUT},
     NI_MIN_KEEP_ALIVE_TIMEOUT,
     NI_MAX_KEEP_ALIVE_TIMEOUT,
     FLAGS,
     "keep_alive_timeout"},
                                          {NULL}};

static const AVClass ni_roi_class = {
    .class_name = "ni_roi",
    .item_name  = av_default_item_name,
    .option     = ni_roi_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
    //    .child_class_next = child_class_next,
};

static const AVFilterPad avfilter_vf_roi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = ni_roi_filter_frame,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    {NULL}
#endif
};

static const AVFilterPad avfilter_vf_roi_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = ni_roi_output_config_props,
    },
#if (LIBAVFILTER_VERSION_MAJOR < 8)
    {NULL}
#endif
};

AVFilter ff_vf_roi_ni_quadra = {
    .name           = "ni_quadra_roi",
    .description    = NULL_IF_CONFIG_SMALL("NetInt Quadra video roi v" NI_XCODER_REVISION),
    .init           = ni_roi_init,
    .uninit         = ni_roi_uninit,
    .priv_size      = sizeof(NetIntRoiContext),
    .priv_class     = &ni_roi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
#if (LIBAVFILTER_VERSION_MAJOR >= 8)
    FILTER_INPUTS(avfilter_vf_roi_inputs),
    FILTER_OUTPUTS(avfilter_vf_roi_outputs),
    FILTER_QUERY_FUNC(ni_roi_query_formats),
#else
    .inputs         = avfilter_vf_roi_inputs,
    .outputs        = avfilter_vf_roi_outputs,
    .query_formats  = ni_roi_query_formats,
#endif
};
