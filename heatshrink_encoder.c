#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "heatshrink_encoder.h"

typedef enum {
    HSES_NOT_FULL,              /* input buffer not full enough */
    HSES_FILLED,                /* buffer is full */
    HSES_SEARCH,                /* searching for patterns */
    HSES_YIELD_TAG_BIT,         /* yield tag bit */
    HSES_YIELD_LITERAL,         /* emit literal byte */
    HSES_YIELD_BR_INDEX,        /* yielding backref index */
    HSES_YIELD_BR_LENGTH,       /* yielding backref length */
    HSES_SAVE_BACKLOG,          /* copying buffer to backlog */
    HSES_FLUSH_BITS,            /* flush bit buffer */
    HSES_DONE,                  /* done */
} HEATSHRINK_ENCODER_STATE;

#include <assert.h>

#if HEATSHRINK_DEBUGGING_LOGS
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#define LOG(...) printf(__VA_ARGS__)
#define ASSERT(X) assert(X)
static const char *state_names[] = {
    "not_full",
    "filled",
    "search",
    "yield_tag_bit",
    "yield_literal",
    "yield_br_index",
    "yield_br_length",
    "save_backlog",
    "flush_bits",
    "done",
};
#else
#define LOG(...) /* no-op */
#define ASSERT(X) /* no-op */
#endif

typedef enum {
    FLAG_IS_FINISHING = 0x01,
    FLAG_HAS_LITERAL = 0x02,
    FLAG_ON_FINAL_LITERAL = 0x04,
    FLAG_BACKLOG_IS_PARTIAL = 0x08,
    FLAG_BACKLOG_IS_FILLED = 0x10,
} ENCODER_FLAGS;

typedef struct {
    uint8_t *buf;               /* output buffer */
    size_t buf_size;            /* buffer size */
    uint16_t *output_size;      /* bytes pushed to buffer, so far */
} output_info;

#define MATCH_NOT_FOUND ((uint16_t)-1)

static uint16_t get_input_offset(heatshrink_encoder *hse);
static uint16_t get_input_buffer_size(heatshrink_encoder *hse);
static uint16_t get_lookahead_size(heatshrink_encoder *hse);
static void add_tag_bit(heatshrink_encoder *hse, output_info *oi, uint8_t tag);
static int can_take_byte(output_info *oi);
static int is_finishing(heatshrink_encoder *hse);
static int backlog_is_partial(heatshrink_encoder *hse);
static int backlog_is_filled(heatshrink_encoder *hse);
static int on_final_literal(heatshrink_encoder *hse);
static void save_backlog(heatshrink_encoder *hse);
static int has_literal(heatshrink_encoder *hse);

/* Push COUNT (max 8) bits to the output buffer, which has room. */
static void push_bits(heatshrink_encoder *hse, uint8_t count, uint8_t bits,
    output_info *oi);
static uint8_t push_outgoing_bits(heatshrink_encoder *hse, output_info *oi);
static void push_literal_byte(heatshrink_encoder *hse, output_info *oi);

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_encoder *heatshrink_encoder_alloc(uint8_t window_sz2,
        uint8_t lookahead_sz2) {
    if ((window_sz2 < HEATSHRINK_MIN_WINDOW_BITS) ||
        (window_sz2 > HEATSHRINK_MAX_WINDOW_BITS) ||
        (lookahead_sz2 < HEATSHRINK_MIN_LOOKAHEAD_BITS) ||
        (lookahead_sz2 > window_sz2)) {
        return NULL;
    }
    size_t buf_sz = (2 << window_sz2);
    heatshrink_encoder *hse = HEATSHRINK_MALLOC(sizeof(*hse) + buf_sz);
    if (hse == NULL) return NULL;
    hse->window_sz2 = window_sz2;
    hse->lookahead_sz2 = lookahead_sz2;
    heatshrink_encoder_reset(hse);

#if HEATSHRINK_USE_INDEX
    size_t index_sz = buf_sz*sizeof(uint16_t);
    hse->search_index = HEATSHRINK_MALLOC(index_sz + sizeof(struct hs_index));
    if (hse->search_index == NULL) {
        HEATSHRINK_FREE(hse, sizeof(*hse) + buf_sz);
        return NULL;
    }
    hse->search_index->size = index_sz;
#endif

    LOG("-- allocated encoder with buffer size of %zu (%u byte input size)\n",
        buf_sz, get_input_buffer_size(hse));
    return hse;
}

void heatshrink_encoder_free(heatshrink_encoder *hse) {
    size_t buf_sz = (2 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
#if HEATSHRINK_USE_INDEX
    size_t index_sz = sizeof(struct hs_index) + hse->search_index->size;
    HEATSHRINK_FREE(hse->search_index, index_sz);
    (void)index_sz;
#endif
    HEATSHRINK_FREE(hse, sizeof(heatshrink_encoder) + buf_sz);
    (void)buf_sz;
}
#endif

void heatshrink_encoder_reset(heatshrink_encoder *hse) {
    size_t buf_sz = (2 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
    memset(hse->buffer, 0, buf_sz);
    hse->input_size = 0;
    hse->state = HSES_NOT_FULL;
    hse->match_scan_index = 0;
    hse->flags = 0;
    hse->bit_index = 0x80;
    hse->current_byte = 0x00;
    hse->match_length = 0;

    hse->outgoing_bits = 0x0000;
    hse->outgoing_bits_count = 0;

    #ifdef LOOP_DETECT
    hse->loop_detect = (uint32_t)-1;
    #endif
}

HEATSHRINK_ENCODER_SINK_RES heatshrink_encoder_sink(heatshrink_encoder *hse,
        uint8_t *in_buf, size_t size, uint16_t *input_size) {
    if ((hse == NULL) || (in_buf == NULL) || (input_size == NULL))
        return HSER_SINK_ERROR_NULL;

    /* Sinking more content after saying the content is done, tsk tsk */
    if (is_finishing(hse)) return HSER_SINK_ERROR_MISUSE;

    /* Sinking more content before processing is done */
    if (hse->state != HSES_NOT_FULL) return HSER_SINK_ERROR_MISUSE;

    uint16_t write_offset = get_input_offset(hse) + hse->input_size;
    uint16_t ibs = get_input_buffer_size(hse);
    uint16_t rem = ibs - hse->input_size;
    uint16_t cp_sz = rem < size ? rem : size;

    memcpy(&hse->buffer[write_offset], in_buf, cp_sz);
    *input_size = cp_sz;
    hse->input_size += cp_sz;

    LOG("-- sunk %u bytes (of %zu) into encoder at %d, input buffer now has %u\n",
        cp_sz, size, write_offset, hse->input_size);
    if (cp_sz == rem) {
        LOG("-- internal buffer is now full\n");
        hse->state = HSES_FILLED;
    }

    return HSER_SINK_OK;
}


/***************
 * Compression *
 ***************/

static uint16_t find_longest_match(heatshrink_encoder *hse, uint16_t start,
    uint16_t end, uint16_t maxlen, uint16_t *match_length);
static void do_indexing(heatshrink_encoder *hse);

static HEATSHRINK_ENCODER_STATE st_step_search(heatshrink_encoder *hse);
static HEATSHRINK_ENCODER_STATE st_yield_tag_bit(heatshrink_encoder *hse,
    output_info *oi);
static HEATSHRINK_ENCODER_STATE st_yield_literal(heatshrink_encoder *hse,
    output_info *oi);
static HEATSHRINK_ENCODER_STATE st_yield_br_index(heatshrink_encoder *hse,
    output_info *oi);
static HEATSHRINK_ENCODER_STATE st_yield_br_length(heatshrink_encoder *hse,
    output_info *oi);
static HEATSHRINK_ENCODER_STATE st_save_backlog(heatshrink_encoder *hse);
static HEATSHRINK_ENCODER_STATE st_flush_bit_buffer(heatshrink_encoder *hse,
    output_info *oi);

HEATSHRINK_ENCODER_POLL_RES heatshrink_encoder_poll(heatshrink_encoder *hse,
        uint8_t *out_buf, size_t out_buf_size, uint16_t *output_size) {
    if ((hse == NULL) || (out_buf == NULL) || (output_size == NULL))
        return HSER_POLL_ERROR_NULL;
    if (out_buf_size == 0) {
        LOG("-- MISUSE: output buffer size is 0\n");
        return HSER_POLL_ERROR_MISUSE;
    }
    *output_size = 0;

    output_info oi;
    oi.buf = out_buf;
    oi.buf_size = out_buf_size;
    oi.output_size = output_size;

    while (1) {
        LOG("-- polling, state %u (%s), flags 0x%02x\n",
            hse->state, state_names[hse->state], hse->flags);

        uint8_t in_state = hse->state;
        switch (in_state) {
        case HSES_NOT_FULL:
            return HSER_POLL_EMPTY;
        case HSES_FILLED:
            do_indexing(hse);
            hse->state = HSES_SEARCH;
            break;
        case HSES_SEARCH:
            hse->state = st_step_search(hse);
            break;
        case HSES_YIELD_TAG_BIT:
            hse->state = st_yield_tag_bit(hse, &oi);
            break;
        case HSES_YIELD_LITERAL:
            hse->state = st_yield_literal(hse, &oi);
            break;
        case HSES_YIELD_BR_INDEX:
            hse->state = st_yield_br_index(hse, &oi);
            break;
        case HSES_YIELD_BR_LENGTH:
            hse->state = st_yield_br_length(hse, &oi);
            break;
        case HSES_SAVE_BACKLOG:
            hse->state = st_save_backlog(hse);
            break;
        case HSES_FLUSH_BITS:
            hse->state = st_flush_bit_buffer(hse, &oi);
        case HSES_DONE:
            return HSER_POLL_EMPTY;
        default:
            LOG("-- bad state %s\n", state_names[hse->state]);
            return HSER_POLL_ERROR_MISUSE;
        }

        if (hse->state == in_state) {
            /* Check if output buffer is exhausted. */
            if (*output_size == out_buf_size) return HSER_POLL_MORE;
        }
    }
}

HEATSHRINK_ENCODER_FINISH_RES heatshrink_encoder_finish(heatshrink_encoder *hse) {
    if (hse == NULL) return HSER_FINISH_ERROR_NULL;
    LOG("-- setting is_finishing flag\n");
    hse->flags |= FLAG_IS_FINISHING;
    if (hse->state == HSES_NOT_FULL) hse->state = HSES_FILLED;
    return hse->state == HSES_DONE ? HSER_FINISH_DONE : HSER_FINISH_MORE;
}

static HEATSHRINK_ENCODER_STATE st_step_search(heatshrink_encoder *hse) {
    uint16_t window_length = get_input_buffer_size(hse);
    uint16_t lookahead_sz = get_lookahead_size(hse);
    uint16_t msi = hse->match_scan_index;
    LOG("## step_search, scan @ +%d (%d/%d), input size %d\n",
        msi, hse->input_size + msi, 2*window_length, hse->input_size);

    bool fin = is_finishing(hse);
    if (msi >= hse->input_size - (fin ? 0 : lookahead_sz)) {
        /* Current search buffer is exhausted, copy it into the
         * backlog and await more input. */
        LOG("-- end of search @ %d, saving backlog\n", msi);
        return HSES_SAVE_BACKLOG;
    }

    uint16_t input_offset = get_input_offset(hse);
    uint16_t end = input_offset + msi;

    uint16_t start = 0;
    if (backlog_is_filled(hse)) { /* last WINDOW_LENGTH bytes */
        start = end - window_length + 1;
    } else if (backlog_is_partial(hse)) { /* clamp to available data */
        start = end - window_length + 1;
        if (start < lookahead_sz) start = lookahead_sz;
    } else {              /* only scan available input */
        start = input_offset;
    }

    uint16_t max_possible = lookahead_sz;
    if (hse->input_size - msi < lookahead_sz) {
        max_possible = hse->input_size - msi;
    }
    
    uint16_t match_length = 0;
    uint16_t match_pos = find_longest_match(hse,
        start, end, max_possible, &match_length);
    
    if (match_pos == MATCH_NOT_FOUND) {
        LOG("ss Match not found\n");
        hse->match_scan_index++;
        hse->flags |= FLAG_HAS_LITERAL;
        hse->match_length = 0;
        return HSES_YIELD_TAG_BIT;
    } else {
        LOG("ss Found match of %d bytes at %d\n", match_length, match_pos);
        hse->match_pos = match_pos;
        hse->match_length = match_length;
        ASSERT(match_pos < 1 << hse->window_sz2 /*window_length*/);

        #if 0 /* log match */
        printf("E match %d %d '", match_pos, match_length);
        for (int i=0; i<match_length; i++) {
            printf("%c", hse->buffer[end - match_pos + i]);
        }
        printf("'\n");
        #endif

        return HSES_YIELD_TAG_BIT;
    }
}

static HEATSHRINK_ENCODER_STATE st_yield_tag_bit(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        if (hse->match_length == 0) {
            add_tag_bit(hse, oi, HEATSHRINK_LITERAL_MARKER);
            return HSES_YIELD_LITERAL;
        } else {
            add_tag_bit(hse, oi, HEATSHRINK_BACKREF_MARKER);
            hse->outgoing_bits = hse->match_pos - 1;
            hse->outgoing_bits_count = HEATSHRINK_ENCODER_WINDOW_BITS(hse);
            return HSES_YIELD_BR_INDEX;
        }
    } else {
        return HSES_YIELD_TAG_BIT; /* output is full, continue */
    }
}

static HEATSHRINK_ENCODER_STATE st_yield_literal(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        push_literal_byte(hse, oi);
        hse->flags &= ~FLAG_HAS_LITERAL;
        if (on_final_literal(hse)) return HSES_FLUSH_BITS;
        return hse->match_length > 0 ? HSES_YIELD_TAG_BIT : HSES_SEARCH;
    } else {
        return HSES_YIELD_LITERAL;
    }
}

static HEATSHRINK_ENCODER_STATE st_yield_br_index(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        LOG("-- yielding backref index %u\n", hse->match_pos);
        if (push_outgoing_bits(hse, oi) > 0) {
            return HSES_YIELD_BR_INDEX; /* continue */
        } else {
            hse->outgoing_bits = hse->match_length - 1;
            hse->outgoing_bits_count = HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse);
            return HSES_YIELD_BR_LENGTH; /* done */
        }
    } else {
        return HSES_YIELD_BR_INDEX; /* continue */
    }
}

static HEATSHRINK_ENCODER_STATE st_yield_br_length(heatshrink_encoder *hse,
        output_info *oi) {
    if (can_take_byte(oi)) {
        LOG("-- yielding backref length %u\n", hse->match_length);
        if (push_outgoing_bits(hse, oi) > 0) {
            return HSES_YIELD_BR_LENGTH;
        } else {
            hse->match_scan_index += hse->match_length;
            hse->match_length = 0;
            return HSES_SEARCH;
        }
    } else {
        return HSES_YIELD_BR_LENGTH;
    }
}

static HEATSHRINK_ENCODER_STATE st_save_backlog(heatshrink_encoder *hse) {
    if (is_finishing(hse)) {
        /* copy remaining literal (if necessary) */
        if (has_literal(hse)) {
            hse->flags |= FLAG_ON_FINAL_LITERAL;
            return HSES_YIELD_TAG_BIT;
        } else {
            return HSES_FLUSH_BITS;
        }
    } else {
        LOG("-- saving backlog\n");
        save_backlog(hse);
        return HSES_NOT_FULL;
    }
}

static HEATSHRINK_ENCODER_STATE st_flush_bit_buffer(heatshrink_encoder *hse,
        output_info *oi) {
    if (hse->bit_index == 0x80) {
        LOG("-- done!\n");
        return HSES_DONE;
    } else if (can_take_byte(oi)) {
        LOG("-- flushing remaining byte (bit_index == 0x%02x)\n", hse->bit_index);
        oi->buf[(*oi->output_size)++] = hse->current_byte;
        LOG("-- done!\n");
        return HSES_DONE;
    } else {
        return HSES_FLUSH_BITS;
    }
}

static void add_tag_bit(heatshrink_encoder *hse, output_info *oi, uint8_t tag) {
    LOG("-- adding tag bit: %d\n", tag);
    push_bits(hse, 1, tag, oi);
}

static uint16_t get_input_offset(heatshrink_encoder *hse) {
    return get_input_buffer_size(hse);
}

static uint16_t get_input_buffer_size(heatshrink_encoder *hse) {
    return (1 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
}

static uint16_t get_lookahead_size(heatshrink_encoder *hse) {
    return (1 << HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse));
}

static void do_indexing(heatshrink_encoder *hse) {
#if HEATSHRINK_USE_INDEX
    /* Lookup table for last offset a byte appears at, or MATCH_NOT_FOUND */
    uint16_t last[256];
    struct hs_index *hsi = HEATSHRINK_ENCODER_INDEX(hse);
    uint16_t buf_sz = get_input_buffer_size(hse);
    uint16_t input_sz = 2*buf_sz;

    memset(hsi->index, 0xFF, input_sz * sizeof(uint16_t));
    memset(last, 0xFF, 256 * sizeof(uint16_t));

    uint8_t *data = hse->buffer;

    uint16_t input_offset = get_input_offset(hse);
    uint16_t end = input_offset + hse->input_size;

    /* hsi->index[offset] => previous offset w/ same byte. */
    for (int i=0; i<end; i++) {
        uint8_t v = data[i];
        uint16_t lv = last[v];
        hsi->index[i] = lv;
        last[v] = i;
    }
#endif
}

static int is_finishing(heatshrink_encoder *hse) {
    return hse->flags & FLAG_IS_FINISHING;
}

static int backlog_is_partial(heatshrink_encoder *hse) {
    return hse->flags & FLAG_BACKLOG_IS_PARTIAL;
}

static int backlog_is_filled(heatshrink_encoder *hse) {
    return hse->flags & FLAG_BACKLOG_IS_FILLED;
}

static int on_final_literal(heatshrink_encoder *hse) {
    return hse->flags & FLAG_ON_FINAL_LITERAL;
}

static int has_literal(heatshrink_encoder *hse) {
    return (hse->flags & FLAG_HAS_LITERAL);
}

static int can_take_byte(output_info *oi) {
    return *oi->output_size < oi->buf_size;
}

/* Return the longest match for the bytes at buf[end:end+maxlen] between
 * buf[start] and buf[end-1]. If no match is found, return -1. */
static uint16_t find_longest_match(heatshrink_encoder *hse, uint16_t start,
        uint16_t end, uint16_t maxlen, uint16_t *match_length) {
    LOG("-- scanning for match of buf[%u:%u] between buf[%u:%u] (max %u bytes)\n",
        end, end + maxlen, start, end + maxlen - 1, maxlen);
    uint8_t *buf = hse->buffer;

    /* Skip search at self. */
    if (start == end) return MATCH_NOT_FOUND;

    uint16_t match_maxlen = 0;
    uint16_t match_index = MATCH_NOT_FOUND;
    uint16_t needle_index = end;
    uint16_t break_even_point = 2;
    uint16_t len = 0;
#if HEATSHRINK_USE_INDEX
    struct hs_index *hsi = HEATSHRINK_ENCODER_INDEX(hse);
    uint16_t pos = hsi->index[end];

    if (pos < start) return MATCH_NOT_FOUND;
    while (pos != MATCH_NOT_FOUND) {
        for (len=0; len<maxlen; len++) {
            if (0) LOG("    -- checking char %c at %d against %c at %d\n",
                buf[pos + len], pos + len, buf[needle_index + len],
                needle_index + len);
            if (buf[pos + len] != buf[needle_index + len]) break;
        }
        if (len > break_even_point) {
            if (len > match_maxlen) {
                match_maxlen = len;
                match_index = pos;
                if (len == maxlen) break; /* don't keep searching */
            }
        }
        pos = hsi->index[pos];
        if (pos < start) break;
    }
#else    
    for (uint16_t pos=end - 1; ; pos--) {
        for (len=0; len<maxlen; len++) {
            if (0) LOG("  --> cmp buf[%d] == 0x%02x against %02x (start %u)\n",
                pos + len, buf[pos + len], buf[needle_index + len], start);
            if (buf[pos + len] != buf[needle_index + len]) break;
        }
        if (len > break_even_point) {
            if (len > match_maxlen) {
                match_maxlen = len;
                match_index = pos;
                if (len == maxlen) break; /* don't keep searching */
            }
        }
        /* start may be 0, so can't use i >= start */
        if (pos == start) break;
    }
#endif
    
    if (match_maxlen > 0) {
        LOG("-- best match: %u bytes at -%u\n",
            match_maxlen, needle_index - match_index);
        *match_length = match_maxlen;
        return needle_index - match_index;
    }
    LOG("-- none found\n");
    return MATCH_NOT_FOUND;
}

static uint8_t push_outgoing_bits(heatshrink_encoder *hse, output_info *oi) {
    uint8_t count = 0;
    uint8_t bits = 0;
    if (hse->outgoing_bits_count > 8) {
        count = 8;
        bits = hse->outgoing_bits >> (hse->outgoing_bits_count - 8);
    } else {
        count = hse->outgoing_bits_count;
        bits = hse->outgoing_bits;
    }
    LOG("-- pushing %d outgoing bits: 0x%02x\n", count, bits);
    push_bits(hse, count, bits, oi);
    hse->outgoing_bits_count -= count;
    return count;
}

/* Push COUNT (max 8) bits to the output buffer, which has room.
 * Bytes are set from the lowest bits, up. */
static void push_bits(heatshrink_encoder *hse, uint8_t count, uint8_t bits,
        output_info *oi) {
    LOG("++ push_bits: %d bits, input of 0x%02x\n", count, bits);
    for (int i=count - 1; i>=0; i--) {
        uint8_t bit = bits & (1 << i);
        if (bit) hse->current_byte |= hse->bit_index;
        if (0) LOG("  -- setting bit %d at bit index 0x%02x, byte => 0x%02x\n",
            bit ? 1 : 0, hse->bit_index, hse->current_byte);
        hse->bit_index >>= 1;
        if (hse->bit_index == 0x00) {
            hse->bit_index = 0x80;
            LOG(" > pushing byte 0x%02x\n", hse->current_byte);
            oi->buf[(*oi->output_size)++] = hse->current_byte;
            hse->current_byte = 0x00;
        }
    }
}

static void push_literal_byte(heatshrink_encoder *hse, output_info *oi) {
    uint16_t processed_offset = hse->match_scan_index - 1;
    uint16_t input_offset = get_input_offset(hse) + processed_offset;
    uint8_t c = hse->buffer[input_offset];
    LOG("-- yielded literal byte 0x%02x ('%c') from +%d\n",
        c, isprint(c) ? c : '.', input_offset);
    push_bits(hse, 8, c, oi);
}

static void save_backlog(heatshrink_encoder *hse) {
    size_t input_buf_sz = get_input_buffer_size(hse);
    
    uint16_t msi = hse->match_scan_index;
    
    /* Copy processed data to beginning of buffer, so it can be
     * used for future matches. Don't bother checking whether the
     * input is less than the maximum size, because if it isn't,
     * we're done anyway. */
    uint16_t rem = input_buf_sz - msi; // unprocessed bytes
    uint16_t shift_sz = input_buf_sz + rem;

    memmove(&hse->buffer[0],
        &hse->buffer[input_buf_sz - rem],
        shift_sz);
        
    if (backlog_is_partial(hse)) {
        /* The whole backlog is filled in now, so include it in scans. */
        hse->flags |= FLAG_BACKLOG_IS_FILLED;
    } else {
        /* Include backlog, except for the first lookahead_sz bytes, which
         * are still undefined. */
        hse->flags |= FLAG_BACKLOG_IS_PARTIAL;
    }
    hse->match_scan_index = 0;
    hse->input_size -= input_buf_sz - rem;
}
