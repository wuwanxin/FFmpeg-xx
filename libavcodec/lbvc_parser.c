#include "get_bits.h"
#include "parser.h"
#include "bytestream.h"

#define MAX_FRAME_BLK 200
#define ALIGN(a,b) (((a) + ((b) - 1)) / (b) * (b))
static struct __ctx {
    int g_num_blk;
};

struct __ctx g_ctx;
static int lbvc_uhs_parse_init(AVCodecParserContext *s){
    g_ctx.g_num_blk = 0;
    return 0;
}

static int lbvc_uhs_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    ParseContext *pc = s->priv_data;
    
    

    GetByteContext gb;
    int32_t header = 0;
    int16_t tmp = 0;
    bytestream2_init(&gb, buf, buf_size);
    int count;

    header = bytestream2_get_be32(&gb);
    if(((header & 0xffff0000) >> 16) == 0xfffe){
        count = (header & 0x0000ffff);
    }else{
        return -1;
    }

    if((count > 0) && (count <= MAX_FRAME_BLK)){
        if(g_ctx.g_num_blk!=0){
            if(g_ctx.g_num_blk!=count){
                return -1;
            }
        }
    }
    //printf("buf_size:%d\n",buf_size);
    tmp = bytestream2_get_be16(&gb);
    if(tmp <= 0){
        printf("header err1 %d\n",tmp);
        goto err;
    } 
    avctx->width = tmp;

    tmp = bytestream2_get_be16(&gb);
    if(tmp <= 0){
        printf("header err2 %d\n",tmp);
        goto err;
    } 
    avctx->height = tmp;

    tmp = bytestream2_get_be16(&gb);
    if(tmp <= 0){
        printf("header err3 %d\n",tmp);
        goto err;
    } 
    avctx->coded_width = ALIGN(avctx->width,tmp);//(avctx->width + (tmp - 1)) / tmp * tmp;
    
    tmp = bytestream2_get_be16(&gb);
    if(tmp <= 0){
        printf("header err4 %d\n",tmp);
        goto err;
    }
    avctx->coded_height = ALIGN(avctx->height,tmp);

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    *poutbuf = buf;
    *poutbuf_size = buf_size;

    return buf_size;
err:
    printf("header err;skip\n");
    *poutbuf = buf;
    *poutbuf_size = buf_size;

    return buf_size;   
}

const AVCodecParser ff_lbvc_uhs_parser = {
    .codec_ids      = { AV_CODEC_ID_LBVC_UHS },
    .priv_data_size = sizeof(ParseContext),
    .parser_init    = lbvc_uhs_parse_init,
    .parser_parse   = lbvc_uhs_parse,
    .parser_close   = ff_parse_close,
};
