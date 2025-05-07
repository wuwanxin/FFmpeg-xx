#include "get_bits.h"
#include "parser.h"
#include "bytestream.h"
#include "libavutil/opt.h"

#define MAX_FRAME_BLK 200
#define ALIGN(a,b) (((a) + ((b) - 1)) / (b) * (b))


typedef struct {
    uint8_t *buffer;      // Buffer to store incomplete data
    int buffer_size;      // Current buffer size
	int max_buffer_size;  // Maximum buffer size
	
    int last_processed_position;
    int header_position;

    int num_blk;
    int error_pkt;
} LBVCUHSParser;

static int lbvc_uhs_parse_init(AVCodecParserContext *s){
	LBVCUHSParser *lbvc_parser = (LBVCUHSParser *)s->priv_data;
	lbvc_parser->max_buffer_size = 1024*1024*40; // Set an appropriate max buffer size
    lbvc_parser->buffer = av_malloc(lbvc_parser->max_buffer_size);
    lbvc_parser->buffer_size = 0;
    lbvc_parser->num_blk = 0;
    lbvc_parser->header_position = -1;
    lbvc_parser->error_pkt = 0;
    return 0;
}

static int find_sync_code(LBVCUHSParser *lbvc_parser,GetByteContext *gb) {
    int32_t header = 0;
    int count;
    int left = bytestream2_get_bytes_left(gb) ;
    int pos = -1;
    while(left >= 4 ){
        count = -1;
        header = bytestream2_get_be32(gb);
#if 0
        static FILE *tfp = NULL;
        if(!tfp) tfp = fopen("testout/tfp_read_bytes.bin","wb");
        if(tfp){
            fputc((char)((header & 0xff000000) >> 24)  ,tfp);
            // fputc((char)((header & 0x00ff0000) >> 16)  ,tfp);
            // fputc((char)((header & 0x0000ff00) >> 8)  ,tfp);
            // fputc((char)(header & 0x000000ff)  ,tfp);
            fflush(tfp);
        }
#endif
        if(((header & 0xffff0000) >> 16) == 0xfffe){
            count = (header & 0x0000ffff);
        }else{
            left -= 1;
            bytestream2_seek(gb,-3,SEEK_CUR);
            continue;
        }
    
        if((count > 0) && (count <= MAX_FRAME_BLK)){
            if(lbvc_parser->num_blk!=0){
                if(lbvc_parser->num_blk!=count){
                    return -1;
                }
            }else{
                lbvc_parser->num_blk = count;
            }
            pos = (bytestream2_tell(gb)-4);
            break;
        }
        left -= 1;
        bytestream2_seek(gb,-3,SEEK_CUR);
        
    }
    
    return pos;
}

static int lbvc_uhs_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                           const uint8_t **poutbuf, int *poutbuf_size,
                           const uint8_t *buf, int buf_size) {
    LBVCUHSParser *lbvc_parser = (LBVCUHSParser *)s->priv_data;
    int packet_size = 0;
    int resume_data = 0;
    // Ensure the buffer has enough space
    if (lbvc_parser->buffer_size + buf_size > lbvc_parser->max_buffer_size) {
        lbvc_parser->max_buffer_size = (lbvc_parser->buffer_size + buf_size) * 2;
        lbvc_parser->buffer = realloc(lbvc_parser->buffer, lbvc_parser->max_buffer_size);
    }

    // Copy new data into the buffer
    memcpy(lbvc_parser->buffer + lbvc_parser->buffer_size, buf, buf_size);
    lbvc_parser->buffer_size += buf_size;
    resume_data = buf_size;

    // Create GetByteContext
    GetByteContext gbc;
    bytestream2_init(&gbc, lbvc_parser->buffer, lbvc_parser->buffer_size);

    // Track the current position in the buffer
    int last_processed_position = lbvc_parser->last_processed_position;
    int header_position = lbvc_parser->header_position;

    // Start searching from the last processed position
    bytestream2_skip(&gbc,last_processed_position);

    if(header_position < 0){
        // Look for the packet header from the current position
        int header_position = find_sync_code(lbvc_parser, &gbc);
        
        if (header_position < 0) {
            // No header found, update last_processed_position and return
            lbvc_parser->last_processed_position = lbvc_parser->buffer_size; // Move to the end of the buffer
            goto end; // Not enough data to complete a packet
        }
        lbvc_parser->header_position = header_position;
        
        //Check param
        int16_t tmp = 0;
        tmp = bytestream2_get_be16(&gbc);
        if(tmp <= 0){
            printf("header err1 %d\n",tmp);
            lbvc_parser->error_pkt = 1;
        }else{
            if(avctx->width && (avctx->width!=tmp)) lbvc_parser->error_pkt = 1;
            avctx->width = tmp;
        }
        
        tmp = bytestream2_get_be16(&gbc);
        if(tmp <= 0){
            printf("header err2 %d\n",tmp);
            lbvc_parser->error_pkt = 1;
        }else{
            if(avctx->height && (avctx->height!=tmp)) lbvc_parser->error_pkt = 1;
            avctx->height = tmp;
        }
        
        tmp = bytestream2_get_be16(&gbc);
        if(tmp <= 0){
            printf("header err3 %d\n",tmp);
            lbvc_parser->error_pkt = 1;
        } else{
            if(avctx->coded_width && (avctx->coded_width!=ALIGN(avctx->width,tmp))) lbvc_parser->error_pkt = 1;
            avctx->coded_width = ALIGN(avctx->width,tmp);//(avctx->width + (tmp - 1)) / tmp * tmp;
			char option_value[32];
			snprintf(option_value, sizeof(option_value), "%d", tmp);
            av_opt_set(avctx->priv_data,"blk_w",option_value,0);
        }
        
        tmp = bytestream2_get_be16(&gbc);
        if(tmp <= 0){
            printf("header err4 %d\n",tmp);
            lbvc_parser->error_pkt = 1;
        }else{
            if(avctx->coded_height && (avctx->coded_height!=ALIGN(avctx->height,tmp))) lbvc_parser->error_pkt = 1;
            avctx->coded_height = ALIGN(avctx->height,tmp);
			char option_value[32];
			snprintf(option_value, sizeof(option_value), "%d", tmp);
            av_opt_set(avctx->priv_data,"blk_h",option_value,0);
        }
        
        avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    // Update header_position value
    header_position = lbvc_parser->header_position;
    int next_header_position = -1;
    if(header_position >= 0){
        // Look for the next packet header
        next_header_position = find_sync_code(lbvc_parser, &gbc);
        
        if (next_header_position < 0) {
            // No next header found, cache the current buffer for the next call
            lbvc_parser->last_processed_position = lbvc_parser->buffer_size; // Update to current header position
            goto end; // Not enough data to complete a packet
        }

        // Calculate the size of the packet
        packet_size = next_header_position - header_position; // Size from the current header to the next header
    }else{
        // Header not found, Error
        goto err;
    }

    // Set the output buffer and size
    if(!lbvc_parser->error_pkt){
        *poutbuf = av_malloc(packet_size);
        memcpy((uint8_t *)*poutbuf, lbvc_parser->buffer + header_position, packet_size);
        *poutbuf_size = packet_size;
    
        // Update the buffer to remove processed data
        lbvc_parser->buffer_size -= next_header_position; // Remove the contents of the current and next packet
        memmove(lbvc_parser->buffer, lbvc_parser->buffer + next_header_position, lbvc_parser->buffer_size);
    }else{
        *poutbuf_size = 0;
        *poutbuf = NULL;
    }
    

    // Reset last_processed_position for the next call
    lbvc_parser->last_processed_position = 0;
    lbvc_parser->header_position = -1; 
    lbvc_parser->error_pkt = 0;


end:
    return resume_data; // Return success, one packet extracted
err:
    lbvc_parser->error_pkt = 1;
    return resume_data; // Now not process error !!!!
}


static void lbvc_uhs_parser_free(AVCodecParserContext *s) {
    LBVCUHSParser *lbvc_parser = (LBVCUHSParser *)s->priv_data;
    free(lbvc_parser->buffer);
	ff_parse_close(s);
}

const AVCodecParser ff_lbvc_uhs_parser = {
    .codec_ids      = { AV_CODEC_ID_LBVC_UHS },
    .priv_data_size = sizeof(LBVCUHSParser),
    .parser_init    = lbvc_uhs_parse_init,
    .parser_parse   = lbvc_uhs_parse,
    .parser_close   = ff_parse_close,
};
