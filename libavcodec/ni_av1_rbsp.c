/*
 * NetInt AV1 RBSP parser common source code
 * Copyright (c) 2018-2023 NetInt
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
 *
 * An RBSP parser according to the AV1 syntax template. Using FFmpeg put_bits
 * module for the bit write operations.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// commented out since it's not used
//#include <ni_rsrc_api.h>

#include <libavcodec/avcodec.h>
#include <libavutil/internal.h>
#include <libavutil/avassert.h>

#include "ni_av1_rbsp.h"
#include "cbs_internal.h"

#include "libavutil/pixfmt.h"
#include "cbs.h"
#include "cbs_av1.h"
#include "internal.h"

static int ni_av1_read_uvlc(CodedBitstreamContext *ctx, GetBitContext *gbc,
                             const char *name, uint32_t *write_to,
                             uint32_t range_min, uint32_t range_max)
{
    uint32_t zeroes, bits_value, value;
    int position;

    if (ctx->trace_enable)
        position = get_bits_count(gbc);

    zeroes = 0;
    while (1) {
        if (get_bits_left(gbc) < 1) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Invalid uvlc code at "
                   "%s: bitstream ended.\n", name);
            return AVERROR_INVALIDDATA;
        }

        if (get_bits1(gbc))
            break;
        ++zeroes;
    }

    if (zeroes >= 32) {
        value = MAX_UINT_BITS(32);
    } else {
        if (get_bits_left(gbc) < zeroes) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Invalid uvlc code at "
                   "%s: bitstream ended.\n", name);
            return AVERROR_INVALIDDATA;
        }

        bits_value = get_bits_long(gbc, zeroes);
        value = bits_value + (UINT32_C(1) << zeroes) - 1;
    }

    if (ctx->trace_enable) {
        char bits[65];
        int i, j, k;

        if (zeroes >= 32) {
            while (zeroes > 32) {
                k = FFMIN(zeroes - 32, 32);
                for (i = 0; i < k; i++)
                    bits[i] = '0';
                bits[i] = 0;
                ff_cbs_trace_syntax_element(ctx, position, name,
                                            NULL, bits, 0);
                zeroes -= k;
                position += k;
            }
        }

        for (i = 0; i < zeroes; i++)
            bits[i] = '0';
        bits[i++] = '1';

        if (zeroes < 32) {
            for (j = 0; j < zeroes; j++)
                bits[i++] = (bits_value >> (zeroes - j - 1) & 1) ? '1' : '0';
        }

        bits[i] = 0;
        ff_cbs_trace_syntax_element(ctx, position, name,
                                    NULL, bits, value);
    }

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [%"PRIu32",%"PRIu32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }

    *write_to = value;
    return 0;
}

int ni_av1_write_uvlc(CodedBitstreamContext *ctx, PutBitContext *pbc,
                              const char *name, uint32_t value,
                              uint32_t range_min, uint32_t range_max)
{
    uint32_t v;
    int position, zeroes;

    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [%"PRIu32",%"PRIu32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }

    if (ctx->trace_enable)
        position = put_bits_count(pbc);

    if (value == 0) {
        zeroes = 0;
        put_bits(pbc, 1, 1);
    } else {
        zeroes = av_log2(value + 1);
        v = value - (1U << zeroes) + 1;
        put_bits(pbc, zeroes, 0);
        put_bits(pbc, 1, 1);
        put_bits(pbc, zeroes, v);
    }

    if (ctx->trace_enable) {
        char bits[65];
        int i, j;
        i = 0;
        for (j = 0; j < zeroes; j++)
            bits[i++] = '0';
        bits[i++] = '1';
        for (j = 0; j < zeroes; j++)
            bits[i++] = (v >> (zeroes - j - 1) & 1) ? '1' : '0';
        bits[i++] = 0;
        ff_cbs_trace_syntax_element(ctx, position, name, NULL,
                                    bits, value);
    }

    return 0;
}

static int ni_av1_read_leb128(CodedBitstreamContext *ctx, GetBitContext *gbc,
                               const char *name, uint64_t *write_to)
{
    uint64_t value;
    int position, err, i;

    if (ctx->trace_enable)
        position = get_bits_count(gbc);

    value = 0;
    for (i = 0; i < 8; i++) {
        int subscript[2] = { 1, i };
        uint32_t byte;
        err = ff_cbs_read_unsigned(ctx, gbc, 8, "leb128_byte[i]", subscript,
                                   &byte, 0x00, 0xff);
        if (err < 0)
            return err;

        value |= (uint64_t)(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80))
            break;
    }

    if (value > UINT32_MAX)
        return AVERROR_INVALIDDATA;

    if (ctx->trace_enable)
        ff_cbs_trace_syntax_element(ctx, position, name, NULL, "", value);

    *write_to = value;
    return 0;
}

static int ni_av1_write_leb128(CodedBitstreamContext *ctx, PutBitContext *pbc,
                                const char *name, uint64_t value)
{
    int position, err, len, i;
    uint8_t byte;

    len = (av_log2(value) + 7) / 7;

    if (ctx->trace_enable)
        position = put_bits_count(pbc);

    for (i = 0; i < len; i++) {
        int subscript[2] = { 1, i };

        byte = value >> (7 * i) & 0x7f;
        if (i < len - 1)
            byte |= 0x80;

        err = ff_cbs_write_unsigned(ctx, pbc, 8, "leb128_byte[i]", subscript,
                                    byte, 0x00, 0xff);
        if (err < 0)
            return err;
    }

    if (ctx->trace_enable)
        ff_cbs_trace_syntax_element(ctx, position, name, NULL, "", value);

    return 0;
}

static int ni_av1_read_ns(CodedBitstreamContext *ctx, GetBitContext *gbc,
                           uint32_t n, const char *name,
                           const int *subscripts, uint32_t *write_to)
{
    uint32_t m, v, extra_bit, value;
    int position, w;

    av_assert0(n > 0);

    if (ctx->trace_enable)
        position = get_bits_count(gbc);

    w = av_log2(n) + 1;
    m = (1 << w) - n;

    if (get_bits_left(gbc) < w) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "Invalid non-symmetric value at "
               "%s: bitstream ended.\n", name);
        return AVERROR_INVALIDDATA;
    }

    if (w - 1 > 0)
        v = get_bits(gbc, w - 1);
    else
        v = 0;

    if (v < m) {
        value = v;
    } else {
        extra_bit = get_bits1(gbc);
        value = (v << 1) - m + extra_bit;
    }

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (i = 0; i < w - 1; i++)
            bits[i] = (v >> i & 1) ? '1' : '0';
        if (v >= m)
            bits[i++] = extra_bit ? '1' : '0';
        bits[i] = 0;

        ff_cbs_trace_syntax_element(ctx, position,
                                    name, subscripts, bits, value);
    }

    *write_to = value;
    return 0;
}

static int ni_av1_write_ns(CodedBitstreamContext *ctx, PutBitContext *pbc,
                            uint32_t n, const char *name,
                            const int *subscripts, uint32_t value)
{
    uint32_t w, m, v, extra_bit;
    int position;

    if (value > n) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [0,%"PRIu32"].\n",
               name, value, n);
        return AVERROR_INVALIDDATA;
    }

    if (ctx->trace_enable)
        position = put_bits_count(pbc);

    w = av_log2(n) + 1;
    m = (1 << w) - n;

    if (put_bits_left(pbc) < w)
        return AVERROR(ENOSPC);

    if (value < m) {
        v = value;
        put_bits(pbc, w - 1, v);
    } else {
        v = m + ((value - m) >> 1);
        extra_bit = (value - m) & 1;
        put_bits(pbc, w - 1, v);
        put_bits(pbc, 1, extra_bit);
    }

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (i = 0; i < w - 1; i++)
            bits[i] = (v >> i & 1) ? '1' : '0';
        if (value >= m)
            bits[i++] = extra_bit ? '1' : '0';
        bits[i] = 0;

        ff_cbs_trace_syntax_element(ctx, position,
                                    name, subscripts, bits, value);
    }

    return 0;
}

static int ni_av1_read_increment(CodedBitstreamContext *ctx, GetBitContext *gbc,
                                  uint32_t range_min, uint32_t range_max,
                                  const char *name, uint32_t *write_to)
{
    uint32_t value;
    int position, i;
    char bits[33];

    av_assert0(range_min <= range_max && range_max - range_min < sizeof(bits) - 1);
    if (ctx->trace_enable)
        position = get_bits_count(gbc);

    for (i = 0, value = range_min; value < range_max;) {
        if (get_bits_left(gbc) < 1) {
            av_log(ctx->log_ctx, AV_LOG_ERROR, "Invalid increment value at "
                   "%s: bitstream ended.\n", name);
            return AVERROR_INVALIDDATA;
        }
        if (get_bits1(gbc)) {
            bits[i++] = '1';
            ++value;
        } else {
            bits[i++] = '0';
            break;
        }
    }

    if (ctx->trace_enable) {
        bits[i] = 0;
        ff_cbs_trace_syntax_element(ctx, position,
                                    name, NULL, bits, value);
    }

    *write_to = value;
    return 0;
}

static int ni_av1_write_increment(CodedBitstreamContext *ctx, PutBitContext *pbc,
                                   uint32_t range_min, uint32_t range_max,
                                   const char *name, uint32_t value)
{
    int len;

    av_assert0(range_min <= range_max && range_max - range_min < 32);
    if (value < range_min || value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [%"PRIu32",%"PRIu32"].\n",
               name, value, range_min, range_max);
        return AVERROR_INVALIDDATA;
    }

    if (value == range_max)
        len = range_max - range_min;
    else
        len = value - range_min + 1;
    if (put_bits_left(pbc) < len)
        return AVERROR(ENOSPC);

    if (ctx->trace_enable) {
        char bits[33];
        int i;
        for (i = 0; i < len; i++) {
            if (range_min + i == value)
                bits[i] = '0';
            else
                bits[i] = '1';
        }
        bits[i] = 0;
        ff_cbs_trace_syntax_element(ctx, put_bits_count(pbc),
                                    name, NULL, bits, value);
    }

    if (len > 0)
        put_bits(pbc, len, (1 << len) - 1 - (value != range_max));

    return 0;
}

static int ni_av1_read_subexp(CodedBitstreamContext *ctx, GetBitContext *gbc,
                               uint32_t range_max, const char *name,
                               const int *subscripts, uint32_t *write_to)
{
    uint32_t value;
    int position, err;
    uint32_t max_len, len, range_offset, range_bits;

    if (ctx->trace_enable)
        position = get_bits_count(gbc);

    av_assert0(range_max > 0);
    max_len = av_log2(range_max - 1) - 3;

    err = ni_av1_read_increment(ctx, gbc, 0, max_len,
                                 "subexp_more_bits", &len);
    if (err < 0)
        return err;

    if (len) {
        range_bits   = 2 + len;
        range_offset = 1 << range_bits;
    } else {
        range_bits   = 3;
        range_offset = 0;
    }

    if (len < max_len) {
        err = ff_cbs_read_unsigned(ctx, gbc, range_bits,
                                   "subexp_bits", NULL, &value,
                                   0, MAX_UINT_BITS(range_bits));
        if (err < 0)
            return err;

    } else {
        err = ni_av1_read_ns(ctx, gbc, range_max - range_offset,
                              "subexp_final_bits", NULL, &value);
        if (err < 0)
            return err;
    }
    value += range_offset;

    if (ctx->trace_enable)
        ff_cbs_trace_syntax_element(ctx, position,
                                    name, subscripts, "", value);

    *write_to = value;
    return err;
}

static int ni_av1_write_subexp(CodedBitstreamContext *ctx, PutBitContext *pbc,
                                uint32_t range_max, const char *name,
                                const int *subscripts, uint32_t value)
{
    int position, err;
    uint32_t max_len, len, range_offset, range_bits;

    if (value > range_max) {
        av_log(ctx->log_ctx, AV_LOG_ERROR, "%s out of range: "
               "%"PRIu32", but must be in [0,%"PRIu32"].\n",
               name, value, range_max);
        return AVERROR_INVALIDDATA;
    }

    if (ctx->trace_enable)
        position = put_bits_count(pbc);

    av_assert0(range_max > 0);
    max_len = av_log2(range_max - 1) - 3;

    if (value < 8) {
        range_bits   = 3;
        range_offset = 0;
        len = 0;
    } else {
        range_bits = av_log2(value);
        len = range_bits - 2;
        if (len > max_len) {
            // The top bin is combined with the one below it.
            av_assert0(len == max_len + 1);
            --range_bits;
            len = max_len;
        }
        range_offset = 1 << range_bits;
    }

    err = ni_av1_write_increment(ctx, pbc, 0, max_len,
                                  "subexp_more_bits", len);
    if (err < 0)
        return err;

    if (len < max_len) {
        err = ff_cbs_write_unsigned(ctx, pbc, range_bits,
                                    "subexp_bits", NULL,
                                    value - range_offset,
                                    0, MAX_UINT_BITS(range_bits));
        if (err < 0)
            return err;

    } else {
        err = ni_av1_write_ns(ctx, pbc, range_max - range_offset,
                               "subexp_final_bits", NULL,
                               value - range_offset);
        if (err < 0)
            return err;
    }

    if (ctx->trace_enable)
        ff_cbs_trace_syntax_element(ctx, position,
                                    name, subscripts, "", value);

    return err;
}


static int ni_av1_tile_log2(int blksize, int target)
{
    int k;
    for (k = 0; (blksize << k) < target; k++);
    return k;
}

static int ni_av1_get_relative_dist(const AV1RawSequenceHeader *seq,
                                     unsigned int a, unsigned int b)
{
    unsigned int diff, m;
    if (!seq->enable_order_hint)
        return 0;
    diff = a - b;
    m = 1 << seq->order_hint_bits_minus_1;
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

static size_t ni_av1_get_payload_bytes_left(GetBitContext *gbc)
{
    GetBitContext tmp = *gbc;
    size_t size = 0;
    for (int i = 0; get_bits_left(&tmp) >= 8; i++) {
        if (get_bits(&tmp, 8))
            size = i;
    }
    return size;
}

#if (LIBAVCODEC_VERSION_MAJOR >= 59 || LIBAVCODEC_VERSION_MAJOR >= 58 && LIBAVCODEC_VERSION_MINOR >= 134)
#define film_grain(x) film_grain.x
#else
#define film_grain(x) x
#endif

#define HEADER(name) do { \
        ff_cbs_trace_header(ctx, name); \
    } while (0)

#define CHECK(call) do { \
        err = (call); \
        if (err < 0) \
            return err; \
    } while (0)

#define FUNC_NAME(rw, codec, name) ni_ ## codec ## _ ## rw ## _ ## name
#define FUNC_AV1(rw, name) FUNC_NAME(rw, av1, name)
#define FUNC(name) FUNC_AV1(READWRITE, name)

#define SUBSCRIPTS(subs, ...) (subs > 0 ? ((int[subs + 1]){ subs, __VA_ARGS__ }) : NULL)

#define fb(width, name) \
        xf(width, name, current->name, 0, MAX_UINT_BITS(width), 0, )
#define fc(width, name, range_min, range_max) \
        xf(width, name, current->name, range_min, range_max, 0, )
#define flag(name) fb(1, name)
#define su(width, name) \
        xsu(width, name, current->name, 0, )

#define fbs(width, name, subs, ...) \
        xf(width, name, current->name, 0, MAX_UINT_BITS(width), subs, __VA_ARGS__)
#define fcs(width, name, range_min, range_max, subs, ...) \
        xf(width, name, current->name, range_min, range_max, subs, __VA_ARGS__)
#define flags(name, subs, ...) \
        xf(1, name, current->name, 0, 1, subs, __VA_ARGS__)
#define sus(width, name, subs, ...) \
        xsu(width, name, current->name, subs, __VA_ARGS__)

#define fixed(width, name, value) do { \
        av_unused uint32_t fixed_value = value; \
        xf(width, name, fixed_value, value, value, 0, ); \
    } while (0)

#undef READ
#undef READWRITE
#undef RWContext
#undef xf
#undef xsu
#undef uvlc
#undef ns
#undef increment
#undef subexp
#undef delta_q
#undef leb128
#undef infer
#undef byte_alignment


#define WRITE
#define READWRITE write
#define RWContext PutBitContext

#define xf(width, name, var, range_min, range_max, subs, ...) do { \
        CHECK(ff_cbs_write_unsigned(ctx, rw, width, #name, \
                                    SUBSCRIPTS(subs, __VA_ARGS__), \
                                    var, range_min, range_max)); \
    } while (0)

#define xsu(width, name, var, subs, ...) do { \
        CHECK(ff_cbs_write_signed(ctx, rw, width, #name, \
                                  SUBSCRIPTS(subs, __VA_ARGS__), var, \
                                  MIN_INT_BITS(width), \
                                  MAX_INT_BITS(width))); \
    } while (0)

#define uvlc(name, range_min, range_max) do { \
        CHECK(ni_av1_write_uvlc(ctx, rw, #name, current->name, \
                                 range_min, range_max)); \
    } while (0)

#define ns(max_value, name, subs, ...) do { \
        CHECK(ni_av1_write_ns(ctx, rw, max_value, #name, \
                               SUBSCRIPTS(subs, __VA_ARGS__), \
                               current->name)); \
    } while (0)

#define increment(name, min, max) do { \
        CHECK(ni_av1_write_increment(ctx, rw, min, max, #name, \
                                      current->name)); \
    } while (0)

#define subexp(name, max, subs, ...) do { \
        CHECK(ni_av1_write_subexp(ctx, rw, max, #name, \
                                   SUBSCRIPTS(subs, __VA_ARGS__), \
                                   current->name)); \
    } while (0)

#define delta_q(name) do { \
        xf(1, name.delta_coded, current->name != 0, 0, 1, 0, ); \
        if (current->name) \
            xsu(1 + 6, name.delta_q, current->name, 0, ); \
    } while (0)

#define leb128(name) do { \
        CHECK(ni_av1_write_leb128(ctx, rw, #name, current->name)); \
    } while (0)

#define infer(name, value) do { \
        if (current->name != (value)) { \
            av_log(ctx->log_ctx, AV_LOG_ERROR, \
                   "%s does not match inferred value: " \
                   "%"PRId64", but should be %"PRId64".\n", \
                   #name, (int64_t)current->name, (int64_t)(value)); \
            return AVERROR_INVALIDDATA; \
        } \
    } while (0)

#define byte_alignment(rw) (put_bits_count(rw) % 8)

#include "ni_av1_syntax_template.c"

#undef WRITE
#undef READWRITE
#undef RWContext
#undef xf
#undef xsu
#undef uvlc
#undef ns
#undef increment
#undef subexp
#undef delta_q
#undef leb128
#undef infer
#undef byte_alignment

void av1_bitstream_fetch(const PutBitContext *stream, AVPacket *pkt,
                        size_t size) {
    memcpy(pkt->data, stream->buf, size);
    pkt->size = size;
}

void av1_bitstream_reset(PutBitContext *stream) {
    init_put_bits(stream, stream->buf, MAX_PUT_BUF_SIZE);
}

int av1_write_leb128(CodedBitstreamContext *ctx, PutBitContext *s, const char *name, uint64_t value)
{
    ni_av1_write_leb128(ctx, s, name, value);

    return 0;
}

int av1_write_le32(CodedBitstreamContext *ctx, PutBitContext *s, const char *name, uint32_t value)
{
    uint32_t dst = 0;
    uint8_t *byte = (uint8_t *)&dst;

    byte[3] = (value >>  0) & 0xff;
    byte[2] = (value >>  8) & 0xff;
    byte[1] = (value >> 16) & 0xff;
    byte[0] = (value >> 24) & 0xff;

    ff_cbs_write_unsigned(ctx, s, TILE_SIZE_BYTES * 8, name, ((void *)0), dst, 0, ((1UL << (TILE_SIZE_BYTES * 8)) - 1));

    return 0;
}

int av1_update_obu_data_length(CodedBitstreamContext *ctx, PutBitContext *s,
        int start_pos, /*AV1RawTileData *td,*/ AV1RawOBU *obu, PutBitContext *pbc_tmp, int add_trailing_bits)
{
    int data_pos, end_pos, obu_size;

    end_pos = put_bits_count(s);
    //obu_size = (end_pos - start_pos + 7) / 8;

    if(add_trailing_bits)
    {
        // Add trailing bits and recalculate. Should not add it for tile data
        ni_av1_write_trailing_bits(ctx, s, 8 - end_pos % 8);
        end_pos = put_bits_count(s);
    }

    obu->obu_size = obu_size = (end_pos - start_pos + 7) / 8;

    //end_pos = put_bits_count(s);
    // Must now be byte-aligned.
    av_assert0(end_pos % 8 == 0);
    flush_put_bits(s);
    start_pos /= 8;
    end_pos   /= 8;

    *s = *pbc_tmp;

    av_log(ctx->log_ctx, AV_LOG_DEBUG, "### %s line %d %s: writing the size: obu->obu_size %ld\n",
            __FILE__, __LINE__, __func__, obu->obu_size);

    ni_av1_write_leb128(ctx, s, "obu_size", obu->obu_size);

    data_pos = put_bits_count(s) / 8;
    flush_put_bits(s);
    av_assert0(data_pos <= start_pos);

    if (8 * obu->obu_size > put_bits_left(s))
        return AVERROR(ENOSPC);

    if (obu->obu_size > 0) {
        memmove(s->buf + data_pos,
                s->buf + start_pos, obu_size);
        skip_put_bytes(s, obu_size);
    }

    return obu_size;
}

int av1_write_obu_header(CodedBitstreamContext *ctx, PutBitContext *rw,
                                AV1RawOBUHeader *current)
{
    ni_av1_write_obu_header(ctx, rw, current);

    return 0;
}

int av1_write_sequence_header_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *current)
{
    ni_av1_write_sequence_header_obu(ctx, rw,  current);

    return 0;
}

int av1_write_temporal_delimiter_obu(CodedBitstreamContext *ctx, PutBitContext *s)
{
    ni_av1_write_temporal_delimiter_obu(ctx, s);

    return 0;
}

#ifdef NOFRAMEOBU
int av1_write_frame_header_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *seq, AV1RawFrameHeader *current)
{
    ni_av1_write_frame_header_obu(ctx, rw, current, 0, NULL);
    return 0;
}

int av1_write_tile_group_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *seq, AV1RawTileGroup *current)
{
  ni_av1_write_tile_group_obu(ctx, rw, current);
  return 0;
}

#else
int av1_write_frame_obu(CodedBitstreamContext *ctx, PutBitContext *rw, AV1RawSequenceHeader *seq, AV1RawFrame *current)
{
    CodedBitstreamAV1Context *priv = ctx->priv_data;
#ifdef PLOG
    av_log(ctx->log_ctx, AV_LOG_INFO, "### %s line %d %s: \n", __FILE__, __LINE__, __func__);
#endif
    ni_av1_write_frame_obu(ctx, rw, current, NULL);
    return 0;
}
#endif
