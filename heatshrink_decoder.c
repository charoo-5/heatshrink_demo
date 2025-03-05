#include <stdlib.h>
#include <string.h>
#include "heatshrink_decoder.h"

typedef enum {
    HSDS_EMPTY,
    HSDS_INPUT_AVAILABLE,
    HSDS_YIELD_LITERAL,
    HSDS_BACKREF_INDEX,
    HSDS_BACKREF_COUNT,
    HSDS_YIELD_BACKREF,
    HSDS_CHECK_FOR_MORE_INPUT,
} HEATSHRINK_DECODER_STATE;

#if HEATSHRINK_DEBUGGING_LOGS
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define ASSERT(X) assert(X)
static const char *state_names[] = {
    "empty",
    "input_available",
    "yield_literal",
    "backref_index",
    "backref_count",
    "yield_backref",
    "check_for_more_input",
};
#else
#define LOG(...) /* no-op */
#define ASSERT(X) /* no-op */
#endif

typedef struct {
    uint8_t *buf;               /* output buffer */
    size_t buf_size;            /* buffer size */
    uint16_t *output_size;      /* bytes pushed to buffer, so far */
} output_info;

/* Forward references. */
static uint32_t get_bits(heatshrink_decoder *hsd, uint8_t count);
static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint8_t byte);

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_decoder *heatshrink_decoder_alloc(uint16_t input_buffer_size,
                                             uint8_t window_sz2,
                                             uint8_t lookahead_sz2) {
    if ((window_sz2 < HEATSHRINK_MIN_WINDOW_BITS) ||
        (window_sz2 > HEATSHRINK_MAX_WINDOW_BITS) ||
        (input_buffer_size == 0) ||
        (lookahead_sz2 < HEATSHRINK_MIN_LOOKAHEAD_BITS) ||
        (lookahead_sz2 > window_sz2)) {
        return NULL;
    }
    size_t buffers_sz = (1 << window_sz2) + input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    heatshrink_decoder *hsd = HEATSHRINK_MALLOC(sz);
    if (hsd == NULL) return NULL;
    hsd->input_buffer_size = input_buffer_size;
    hsd->window_sz2 = window_sz2;
    hsd->lookahead_sz2 = lookahead_sz2;
    heatshrink_decoder_reset(hsd);
    LOG("-- allocated decoder with buffer size of %zu (%zu + %u + %u)\n",
        sz, sizeof(heatshrink_decoder), (1 << window_sz2), input_buffer_size);
    return hsd;
}

void heatshrink_decoder_free(heatshrink_decoder *hsd) {
    size_t buffers_sz = (1 << hsd->window_sz2) + hsd->input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    HEATSHRINK_FREE(hsd, sz);
    (void)sz;   /* may not be used by free */
}
#endif

void heatshrink_decoder_reset(heatshrink_decoder *hsd) {
    size_t buf_sz = 1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd);
    size_t input_sz = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd);
    memset(hsd->buffers, 0, buf_sz + input_sz);
    hsd->state = HSDS_EMPTY;
    hsd->input_size = 0;
    hsd->input_index = 0;
    hsd->bit_index = 0x00;
    hsd->current_byte = 0x00;
    hsd->output_count = 0;
    hsd->output_index = 0;
    hsd->head_index = 0;
    hsd->bit_accumulator = 0x00000000;
}

/* Copy SIZE bytes into the decoder's input buffer, if it will fit. */
HEATSHRINK_DECODER_SINK_RES heatshrink_decoder_sink(heatshrink_decoder *hsd,
        uint8_t *in_buf, size_t size, uint16_t *input_size) {
    if ((hsd == NULL) || (in_buf == NULL) || (input_size == NULL)) return HSDR_SINK_ERROR_NULL;

    size_t rem = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd) - hsd->input_size;
    if (rem == 0) {
        *input_size = 0;
        return HSDR_SINK_FULL;
    }

    size = rem < size ? rem : size;
    LOG("-- sinking %d bytes\n", size);
    /* copy into input buffer (at head of buffers) */
    memcpy(&hsd->buffers[hsd->input_size], in_buf, size);
    hsd->input_size += size;
    if (hsd->state == HSDS_EMPTY) {
        hsd->state = HSDS_INPUT_AVAILABLE;
        hsd->input_index = 0;
    }
    *input_size = size;
    return HSDR_SINK_OK;
}


/*****************
 * Decompression *
 *****************/

#define BACKREF_COUNT_BITS(HSD) (HEATSHRINK_DECODER_LOOKAHEAD_BITS(HSD))
#define BACKREF_INDEX_BITS(HSD) (HEATSHRINK_DECODER_WINDOW_BITS(HSD))

// States
static HEATSHRINK_DECODER_STATE st_input_available(heatshrink_decoder *hsd);
static HEATSHRINK_DECODER_STATE st_yield_literal(heatshrink_decoder *hsd,
    output_info *oi);
static HEATSHRINK_DECODER_STATE st_backref_index(heatshrink_decoder *hsd);
static HEATSHRINK_DECODER_STATE st_backref_count(heatshrink_decoder *hsd);
static HEATSHRINK_DECODER_STATE st_yield_backref(heatshrink_decoder *hsd,
    output_info *oi);
static HEATSHRINK_DECODER_STATE st_check_for_input(heatshrink_decoder *hsd);

HEATSHRINK_DECODER_POLL_RES heatshrink_decoder_poll(heatshrink_decoder *hsd,
        uint8_t *out_buf, size_t out_buf_size, uint16_t *output_size) {
    if ((hsd == NULL) || (out_buf == NULL) || (output_size == NULL)) return HSDR_POLL_ERROR_NULL;
    *output_size = 0;

    output_info oi;
    oi.buf = out_buf;
    oi.buf_size = out_buf_size;
    oi.output_size = output_size;

    while (1) {
        LOG("-- poll, state is %d (%s), input_size %d\n",
            hsd->state, state_names[hsd->state], hsd->input_size);
        uint8_t in_state = hsd->state;
        switch (in_state) {
        case HSDS_EMPTY:
            return HSDR_POLL_EMPTY;
        case HSDS_INPUT_AVAILABLE:
            hsd->state = st_input_available(hsd);
            break;
        case HSDS_YIELD_LITERAL:
            hsd->state = st_yield_literal(hsd, &oi);
            break;
        case HSDS_BACKREF_INDEX:
            hsd->state = st_backref_index(hsd);
            break;
        case HSDS_BACKREF_COUNT:
            hsd->state = st_backref_count(hsd);
            break;
        case HSDS_YIELD_BACKREF:
            hsd->state = st_yield_backref(hsd, &oi);
            break;
        case HSDS_CHECK_FOR_MORE_INPUT:
            hsd->state = st_check_for_input(hsd);
            break;
        default:
            return HSDR_POLL_ERROR_UNKNOWN;
        }
        
        /* If the current state cannot advance, check if input or output
         * buffer are exhausted. */
        if (hsd->state == in_state) {
            if (*output_size == out_buf_size) return HSDR_POLL_MORE;
            return HSDR_POLL_EMPTY;
        }
    }
}

static HEATSHRINK_DECODER_STATE st_input_available(heatshrink_decoder *hsd) {
    uint32_t bits = get_bits(hsd, 1);  // get tag bit
    return bits ? HSDS_YIELD_LITERAL : HSDS_BACKREF_INDEX;
}

static HEATSHRINK_DECODER_STATE st_yield_literal(heatshrink_decoder *hsd,
        output_info *oi) {
    /* Emit a repeated section from the window buffer, and add it (again)
     * to the window buffer. (Note that the repetition can include
     * itself.)*/
    if (*oi->output_size < oi->buf_size) {
        uint32_t byte = get_bits(hsd, 8);
        if (byte == (uint32_t)-1) return HSDS_YIELD_LITERAL; /* out of input */
        uint8_t *buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        uint16_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd))  - 1;
        uint8_t c = byte & 0xFF;
        LOG("-- emitting literal byte 0x%02x ('%c')\n", c, isprint(c) ? c : '.');
        buf[hsd->head_index++ & mask] = c;
        push_byte(hsd, oi, c);
        return HSDS_CHECK_FOR_MORE_INPUT;
    } else {
        return HSDS_YIELD_LITERAL;
    }
}

static HEATSHRINK_DECODER_STATE st_backref_index(heatshrink_decoder *hsd) {
    uint32_t bits = get_bits(hsd, BACKREF_INDEX_BITS(hsd));
    LOG("-- backref index, got 0x%04x (+1)\n", bits);
    if (bits == (uint32_t)-1) return HSDS_BACKREF_INDEX;
    hsd->output_index = bits + 1;
    return HSDS_BACKREF_COUNT;
}

static HEATSHRINK_DECODER_STATE st_backref_count(heatshrink_decoder *hsd) {
    uint32_t bits = get_bits(hsd, BACKREF_COUNT_BITS(hsd));
    LOG("-- backref count, got 0x%04x (+1)\n", bits);
    if (bits == (uint32_t)-1) return HSDS_BACKREF_COUNT;
    hsd->output_count = bits + 1;
    return HSDS_YIELD_BACKREF;
}

static HEATSHRINK_DECODER_STATE st_yield_backref(heatshrink_decoder *hsd,
        output_info *oi) {
    size_t count = oi->buf_size - *oi->output_size;
    if (count > 0) {
        if (hsd->output_count < count) count = hsd->output_count;
        uint8_t *buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        uint16_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd)) - 1;
        uint16_t neg_offset = hsd->output_index;
        LOG("-- emitting %u bytes from -%u bytes back\n", count, neg_offset);
        ASSERT(neg_offset < mask + 1);
        ASSERT(count <= 1 << BACKREF_COUNT_BITS(hsd));

        for (int i=0; i<count; i++) {
            uint8_t c = buf[(hsd->head_index - neg_offset) & mask];
            push_byte(hsd, oi, c);
            buf[hsd->head_index & mask] = c;
            hsd->head_index++;
            LOG("  -- ++ 0x%02x\n", c);
        }
        hsd->output_count -= count;
        if (hsd->output_count == 0) return HSDS_CHECK_FOR_MORE_INPUT;
    }
    return HSDS_YIELD_BACKREF;
}

static HEATSHRINK_DECODER_STATE st_check_for_input(heatshrink_decoder *hsd) {
    return (hsd->input_size == 0) ? HSDS_EMPTY : HSDS_INPUT_AVAILABLE;
}
    
/* Get the next COUNT bits from the input buffer, saving incremental progress.
 * Returns (uint32_t)-1 on end of input, or if more than 31 bits are requested. */
static uint32_t get_bits(heatshrink_decoder *hsd, uint8_t count) {
    if (count > 31) return (uint32_t)-1;
    LOG("-- popping %u bit(s)\n", count);

    /* If we aren't able to get COUNT bits, suspend immediately, because we
     * don't track how many bits of COUNT we've accumulated before suspend. */
    if (hsd->input_size == 0) {
        if (hsd->bit_index < (1 << (count - 1))) return -1;
    }

    for (int i = 0; i < count; i++) {
        if (hsd->bit_index == 0x00) {
            if (hsd->input_size == 0) {
                LOG("  -- out of bits, suspending w/ accumulator of %u\n",
                    hsd->bit_accumulator);
                return (uint32_t)-1;
            }
            hsd->current_byte = hsd->buffers[hsd->input_index++];
            LOG("  -- pulled byte 0x%02x\n", hsd->current_byte);
            if (hsd->input_index == hsd->input_size) {
                hsd->input_index = 0; /* input is exhausted */
                hsd->input_size = 0;
            }
            hsd->bit_index = 0x80;
        }
        hsd->bit_accumulator <<= 1;
        if (hsd->current_byte & hsd->bit_index) {
            hsd->bit_accumulator |= 0x01;
            if (0) LOG("  -- got 1, accumulator 0x%04x, bit_index 0x%02x\n",
                hsd->bit_accumulator, hsd->bit_index);
        } else {
            if (0) LOG("  -- got 0, accumulator 0x%04x, bit_index 0x%02x\n",
                hsd->bit_accumulator, hsd->bit_index);
        }
        hsd->bit_index >>= 1;
    }

    uint32_t res = 0;
    res = hsd->bit_accumulator;
    hsd->bit_accumulator = 0x00000000;
    if (count > 1) LOG("  -- accumulated %08x\n", res);
    return res;
}

HEATSHRINK_DECODER_FINISH_RES heatshrink_decoder_finish(heatshrink_decoder *hsd) {
    if (hsd == NULL) return HSDR_FINISH_ERROR_NULL;
    switch (hsd->state) {
    case HSDS_EMPTY:
        return HSDR_FINISH_DONE;

    /* If we want to finish with no input, but are in these two states, it's
     * because the 0-bit padding to the last byte looks like a backref
     * marker bit followed by all 0s for index and count bits. */
    case HSDS_BACKREF_INDEX:
    case HSDS_BACKREF_COUNT:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;
    /* fall through */
    default:
        return HSDR_FINISH_MORE;
    }
}

static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint8_t byte) {
    LOG(" -- pushing byte: 0x%02x ('%c')\n", byte, isprint(byte) ? byte : '.');
    oi->buf[(*oi->output_size)++] = byte;
}
