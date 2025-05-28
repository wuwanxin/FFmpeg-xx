#define UNCHECKED_BITSTREAM_READER 1

#include <stdint.h>

#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"

#include "avcodec.h"
#include "get_bits.h"
#include "golomb.h"
typedef struct e2encParseContext {
    int header_parsed;      // 标记是否已解析头
    uint8_t *buffer;        // 累积数据的缓冲区
    size_t buffer_size;     // 累积数据的大小
} e2encParseContext;

static int e2enc_parse(AVCodecParserContext *s,
    AVCodecContext *avctx,
    const uint8_t **poutbuf, int *poutbuf_size,
    const uint8_t *buf, int buf_size)
{
    e2encParseContext *p = s->priv_data;
    int ret = 0;

    av_log(avctx, AV_LOG_DEBUG, "e2enc_parse called, buf_size=%d\n", buf_size);

    // 处理输入数据
    if (buf_size > 0) {
        // 追加数据到内部缓冲区
        uint8_t *new_buffer = av_realloc(p->buffer, p->buffer_size + buf_size);
        if (!new_buffer) {
            av_log(avctx, AV_LOG_ERROR, "Failed to allocate buffer\n");
            return AVERROR(ENOMEM);
        }
        memcpy(new_buffer + p->buffer_size, buf, buf_size);
        p->buffer = new_buffer;
        p->buffer_size += buf_size;

        // 暂时不输出数据
        *poutbuf = NULL;
        *poutbuf_size = 0;
        ret = buf_size; // 消耗所有输入数据
    } else if (buf_size == 0) {
        // 处理结束信号
        if (p->buffer_size > 0) {
            // 解析头信息（仅当有足够数据且未解析过）
            if (!p->header_parsed && p->buffer_size >= 12) {
                uint16_t e2e_scode = AV_RL16(p->buffer);
                uint8_t format = AV_RL8(p->buffer + 2);
                uint16_t height = AV_RL16(p->buffer + 3);
                uint16_t width = AV_RL16(p->buffer + 5);
                uint16_t block_height = AV_RL16(p->buffer + 7);
                uint16_t block_width = AV_RL16(p->buffer + 9);
                uint8_t quality = AV_RL8(p->buffer + 11);

                avctx->width = width;
                avctx->height = height;
                avctx->pix_fmt = AV_PIX_FMT_RGB24;
                av_log(avctx, AV_LOG_DEBUG, "Parsed header: %dx%d quality: %d \n", width, height ,quality);
                p->header_parsed = 1;
            }

            // 分配新缓冲区并复制数据
            uint8_t *out_buf = av_malloc(p->buffer_size);
            if (!out_buf) {
                av_log(avctx, AV_LOG_ERROR, "Failed to allocate output buffer\n");
                av_freep(&p->buffer);
                p->buffer_size = 0;
                *poutbuf = NULL;
                *poutbuf_size = 0;
                return AVERROR(ENOMEM);
            }
            memcpy(out_buf, p->buffer, p->buffer_size);

            // 设置输出
            *poutbuf = out_buf;
            *poutbuf_size = p->buffer_size;

            // 清理内部状态
            av_freep(&p->buffer);
            p->buffer_size = 0;
            p->header_parsed = 0;
            ret = 0;
        } else {
            *poutbuf = NULL;
            *poutbuf_size = 0;
            ret = 0;
        }
    }

    return ret;
}

static void e2enc_close(AVCodecParserContext *s)
{
    e2encParseContext *p = s->priv_data;
    av_freep(&p->buffer); // 确保释放累积的缓冲区
    p->buffer_size = 0;
}

static av_cold int init(AVCodecParserContext *s)
{
    e2encParseContext *p = s->priv_data;
    p->header_parsed = 0;
    p->buffer = NULL;
    p->buffer_size = 0;
    return 0;
}

const AVCodecParser ff_e2enc_parser = {
    .codec_ids      = { AV_CODEC_ID_E2ENC },
    .priv_data_size = sizeof(e2encParseContext),
    .parser_init    = init,
    .parser_parse   = e2enc_parse,
    .parser_close   = e2enc_close,
};