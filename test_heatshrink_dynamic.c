#include <stdint.h>
#include <ctype.h>
#include <assert.h>

#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"
#include "greatest.h"

#if !HEATSHRINK_DYNAMIC_ALLOC
#error Must set HEATSHRINK_DYNAMIC_ALLOC to 1 for dynamic allocation test suite.
#endif

static void dump_buf(char *name, uint8_t *buf, uint16_t count) {
    for (int i=0; i<count; i++) {
        uint8_t c = (uint8_t)buf[i];
        printf("%s %d: 0x%02x ('%c')\n", name, i, c, isprint(c) ? c : '.');
    }
}

TEST encoder_alloc_should_reject_invalid_arguments() {
    ASSERT_EQ(NULL, heatshrink_encoder_alloc(
            HEATSHRINK_MIN_WINDOW_BITS - 1, 8));
    ASSERT_EQ(NULL, heatshrink_encoder_alloc(
            HEATSHRINK_MAX_WINDOW_BITS + 1, 8));
    ASSERT_EQ(NULL, heatshrink_encoder_alloc(8, HEATSHRINK_MIN_LOOKAHEAD_BITS - 1));
    ASSERT_EQ(NULL, heatshrink_encoder_alloc(8, 9));
    PASS();
}

TEST encoder_sink_should_reject_nulls() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    uint8_t input[] = {'f', 'o', 'o'};
    uint16_t input_size = 0;
    ASSERT(hse);
    ASSERT_EQ(HSER_SINK_ERROR_NULL, heatshrink_encoder_sink(NULL, input, 3, &input_size));
    ASSERT_EQ(HSER_SINK_ERROR_NULL, heatshrink_encoder_sink(hse, NULL, 3, &input_size));
    ASSERT_EQ(HSER_SINK_ERROR_NULL, heatshrink_encoder_sink(hse, input, 3, NULL));
    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_poll_should_reject_nulls() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    uint8_t output[256];
    uint16_t output_size = 0;
    ASSERT_EQ(HSER_POLL_ERROR_NULL, heatshrink_encoder_poll(NULL,
            output, 256, &output_size));
    ASSERT_EQ(HSER_POLL_ERROR_NULL, heatshrink_encoder_poll(hse,
            NULL, 256, &output_size));
    ASSERT_EQ(HSER_POLL_ERROR_NULL, heatshrink_encoder_poll(hse,
            output, 256, NULL));

    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_finish_should_reject_nulls() {
    ASSERT_EQ(HSER_FINISH_ERROR_NULL, heatshrink_encoder_finish(NULL));
    PASS();
}

TEST encoder_sink_should_accept_input_when_it_will_fit() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    ASSERT(hse);
    uint8_t input[256];
    uint16_t bytes_copied = 0;
    memset(input, '*', 256);
    ASSERT_EQ(HSER_SINK_OK, heatshrink_encoder_sink(hse,
            input, 256, &bytes_copied));
    ASSERT_EQ(256, bytes_copied);

    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_sink_should_accept_partial_input_when_some_will_fit() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    ASSERT(hse);
    uint8_t input[512];
    uint16_t bytes_copied = 0;
    memset(input, '*', 512);
    ASSERT_EQ(HSER_SINK_OK, heatshrink_encoder_sink(hse,
            input, 512, &bytes_copied));
    ASSERT_EQ(256, bytes_copied);

    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_poll_should_indicate_when_no_input_is_provided() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    uint8_t output[512];
    uint16_t output_size = 0;

    HEATSHRINK_ENCODER_POLL_RES res = heatshrink_encoder_poll(hse,
        output, 512, &output_size);
    ASSERT_EQ(HSER_POLL_EMPTY, res);
    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_should_emit_data_without_repetitions_as_literal_sequence() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    ASSERT(hse);
    uint8_t input[5];
    uint8_t output[1024];
    uint16_t copied = 0;
    uint8_t expected[] = { 0x80, 0x40, 0x60, 0x50, 0x38, 0x20 };

    for (int i=0; i<5; i++) { input[i] = i; }
    memset(output, 0, 1024);
    ASSERT_EQ(HSER_SINK_OK, heatshrink_encoder_sink(hse, input, 5, &copied));
    ASSERT_EQ(5, copied);
    
    /* Should get no output yet, since encoder doesn't know input is complete. */
    copied = 0;
    HEATSHRINK_ENCODER_POLL_RES pres = heatshrink_encoder_poll(hse, output, 1024, &copied);
    ASSERT_EQ(HSER_POLL_EMPTY, pres);
    ASSERT_EQ(0, copied);

    /* Mark input stream as done, to force small input to be processed. */
    HEATSHRINK_ENCODER_FINISH_RES fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_MORE, fres);

    pres = heatshrink_encoder_poll(hse, output, 1024, &copied);
    ASSERT_EQ(HSER_POLL_EMPTY, pres);

    for (int i=0; i<sizeof(expected); i++) {
        ASSERT_EQ(expected[i], output[i]);
    }

    ASSERT_EQ(HSER_FINISH_DONE, heatshrink_encoder_finish(hse));
    
    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_should_emit_series_of_same_byte_as_literal_then_backref() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    ASSERT(hse);
    uint8_t input[5];
    uint8_t output[1024];
    uint16_t copied = 0;
    uint8_t expected[] = {0xb0, 0x80, 0x01, 0x80};

    for (int i=0; i<5; i++) { input[i] = 'a'; } /* "aaaaa" */
    memset(output, 0, 1024);
    ASSERT_EQ(HSER_SINK_OK, heatshrink_encoder_sink(hse, input, 5, &copied));
    ASSERT_EQ(5, copied);
    
    /* Should get no output yet, since encoder doesn't know input is complete. */
    copied = 0;
    HEATSHRINK_ENCODER_POLL_RES pres = heatshrink_encoder_poll(hse, output, 1024, &copied);
    ASSERT_EQ(HSER_POLL_EMPTY, pres);
    ASSERT_EQ(0, copied);

    /* Mark input stream as done, to force small input to be processed. */
    HEATSHRINK_ENCODER_FINISH_RES fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_MORE, fres);

    pres = heatshrink_encoder_poll(hse, output, 1024, &copied);
    ASSERT_EQ(HSER_POLL_EMPTY, pres);
    ASSERT_EQ(4, copied);
    if (0) dump_buf("output", output, copied);
    for (int i=0; i<copied; i++) ASSERT_EQ(expected[i], output[i]);

    ASSERT_EQ(HSER_FINISH_DONE, heatshrink_encoder_finish(hse));
    
    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_poll_should_detect_repeated_substring() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 3);
    uint8_t input[] = {'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd'};
    uint8_t output[1024];
    uint8_t expected[] = {0xb0, 0xd8, 0xac, 0x76, 0x40, 0x1b };

    uint16_t copied = 0;
    memset(output, 0, 1024);
    HEATSHRINK_ENCODER_SINK_RES sres = heatshrink_encoder_sink(hse,
        input, sizeof(input), &copied);
    ASSERT_EQ(HSER_SINK_OK, sres);
    ASSERT_EQ(sizeof(input), copied);

    HEATSHRINK_ENCODER_FINISH_RES fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_MORE, fres);

    ASSERT_EQ(HSER_POLL_EMPTY, heatshrink_encoder_poll(hse, output, 1024, &copied));
    fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_DONE, fres);

    if (0) dump_buf("output", output, copied);
    ASSERT_EQ(sizeof(expected), copied);
    for (int i=0; i<sizeof(expected); i++) ASSERT_EQ(expected[i], output[i]);
    heatshrink_encoder_free(hse);
    PASS();
}

TEST encoder_poll_should_detect_repeated_substring_and_preserve_trailing_literal() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 3);
    uint8_t input[] = {'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd', 'e'};
    uint8_t output[1024];
    uint8_t expected[] = {0xb0, 0xd8, 0xac, 0x76, 0x40, 0x1b, 0xb2, 0x80 };
    uint16_t copied = 0;
    memset(output, 0, 1024);
    HEATSHRINK_ENCODER_SINK_RES sres = heatshrink_encoder_sink(hse,
        input, sizeof(input), &copied);
    ASSERT_EQ(HSER_SINK_OK, sres);
    ASSERT_EQ(sizeof(input), copied);

    HEATSHRINK_ENCODER_FINISH_RES fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_MORE, fres);

    ASSERT_EQ(HSER_POLL_EMPTY, heatshrink_encoder_poll(hse, output, 1024, &copied));
    fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_DONE, fres);

    if (0) dump_buf("output", output, copied);
    ASSERT_EQ(sizeof(expected), copied);
    for (int i=0; i<sizeof(expected); i++) ASSERT_EQ(expected[i], output[i]);
    heatshrink_encoder_free(hse);
    PASS();
}

SUITE(encoding) {
    RUN_TEST(encoder_alloc_should_reject_invalid_arguments);

    RUN_TEST(encoder_sink_should_reject_nulls);
    RUN_TEST(encoder_sink_should_accept_input_when_it_will_fit);
    RUN_TEST(encoder_sink_should_accept_partial_input_when_some_will_fit);

    RUN_TEST(encoder_poll_should_reject_nulls);
    RUN_TEST(encoder_poll_should_indicate_when_no_input_is_provided);

    RUN_TEST(encoder_finish_should_reject_nulls);

    RUN_TEST(encoder_should_emit_data_without_repetitions_as_literal_sequence);
    RUN_TEST(encoder_should_emit_series_of_same_byte_as_literal_then_backref);
    RUN_TEST(encoder_poll_should_detect_repeated_substring);
    RUN_TEST(encoder_poll_should_detect_repeated_substring_and_preserve_trailing_literal);
}

TEST decoder_alloc_should_reject_excessively_small_window() {
    ASSERT_EQ(NULL, heatshrink_decoder_alloc(256,
            HEATSHRINK_MIN_WINDOW_BITS - 1, 4));
    PASS();
}

TEST decoder_alloc_should_reject_zero_byte_input_buffer() {
    ASSERT_EQ(NULL, heatshrink_decoder_alloc(0,
            HEATSHRINK_MIN_WINDOW_BITS, 4));
    PASS();
}

TEST decoder_sink_should_reject_null_hsd_pointer() {
    uint8_t input[] = {0,1,2,3,4,5};
    uint16_t count = 0;
    ASSERT_EQ(HSDR_SINK_ERROR_NULL, heatshrink_decoder_sink(NULL, input, 6, &count));
    PASS();
}

TEST decoder_sink_should_reject_null_input_pointer() {
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256,
        HEATSHRINK_MIN_WINDOW_BITS, 4);
    uint16_t count = 0;
    ASSERT_EQ(HSDR_SINK_ERROR_NULL, heatshrink_decoder_sink(hsd, NULL, 6, &count));
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_sink_should_reject_null_count_pointer() {
    uint8_t input[] = {0,1,2,3,4,5};
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256,
        HEATSHRINK_MIN_WINDOW_BITS, 4);
    ASSERT_EQ(HSDR_SINK_ERROR_NULL, heatshrink_decoder_sink(hsd, input, 6, NULL));
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_sink_should_reject_excessively_large_input() {
    uint8_t input[] = {0,1,2,3,4,5};
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(1,
        HEATSHRINK_MIN_WINDOW_BITS, 4);
    uint16_t count = 0;
    // Sink as much as will fit
    HEATSHRINK_DECODER_SINK_RES res = heatshrink_decoder_sink(hsd, input, 6, &count);
    ASSERT_EQ(HSDR_SINK_OK, res);
    ASSERT_EQ(1, count);

    // And now, no more should fit.
    res = heatshrink_decoder_sink(hsd, &input[count], sizeof(input) - count, &count);
    ASSERT_EQ(HSDR_SINK_FULL, res);
    ASSERT_EQ(0, count);
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_sink_should_sink_data_when_preconditions_hold() {
    uint8_t input[] = {0,1,2,3,4,5};
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256,
        HEATSHRINK_MIN_WINDOW_BITS, 4);
    uint16_t count = 0;
    HEATSHRINK_DECODER_SINK_RES res = heatshrink_decoder_sink(hsd, input, 6, &count);
    ASSERT_EQ(HSDR_SINK_OK, res);
    ASSERT_EQ(hsd->input_size, 6);
    ASSERT_EQ(hsd->input_index, 0);
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_return_empty_if_empty() {
    uint8_t output[256];
    uint16_t out_sz = 0;
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256,
        HEATSHRINK_MIN_WINDOW_BITS, 4);
    HEATSHRINK_DECODER_POLL_RES res = heatshrink_decoder_poll(hsd, output, 256, &out_sz);
    ASSERT_EQ(HSDR_POLL_EMPTY, res);
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_reject_null_hsd() {
    uint8_t output[256];
    uint16_t out_sz = 0;
    HEATSHRINK_DECODER_POLL_RES res = heatshrink_decoder_poll(NULL, output, 256, &out_sz);
    ASSERT_EQ(HSDR_POLL_ERROR_NULL, res);
    PASS();
}

TEST decoder_poll_should_reject_null_output_buffer() {
    uint16_t out_sz = 0;
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256,
        HEATSHRINK_MIN_WINDOW_BITS, 4);
    HEATSHRINK_DECODER_POLL_RES res = heatshrink_decoder_poll(hsd, NULL, 256, &out_sz);
    ASSERT_EQ(HSDR_POLL_ERROR_NULL, res);
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_reject_null_output_size_pointer() {
    uint8_t output[256];
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256,
        HEATSHRINK_MIN_WINDOW_BITS, 4);
    HEATSHRINK_DECODER_POLL_RES res = heatshrink_decoder_poll(hsd, output, 256, NULL);
    ASSERT_EQ(HSDR_POLL_ERROR_NULL, res);
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_expand_short_literal() {
    uint8_t input[] = {0xb3, 0x5b, 0xed, 0xe0 }; //"foo"
    uint8_t output[4];
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 7, 3);
    uint16_t count = 0;

    HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, input, sizeof(input), &count);
    ASSERT_EQ(HSDR_SINK_OK, sres);

    uint16_t out_sz = 0;
    HEATSHRINK_DECODER_POLL_RES pres = heatshrink_decoder_poll(hsd, output, 4, &out_sz);
    ASSERT_EQ(HSDR_POLL_EMPTY, pres);
    ASSERT_EQ(3, out_sz);
    ASSERT_EQ('f', output[0]);
    ASSERT_EQ('o', output[1]);
    ASSERT_EQ('o', output[2]);

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_expand_short_literal_and_backref() {
    uint8_t input[] = {0xb3, 0x5b, 0xed, 0xe0, 0x40, 0x80}; //"foofoo"
    uint8_t output[6];
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 7, 7);
    memset(output, 0, sizeof(*output));
    uint16_t count = 0;
    
    HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, input, sizeof(input), &count);
    ASSERT_EQ(HSDR_SINK_OK, sres);

    uint16_t out_sz = 0;
    (void)heatshrink_decoder_poll(hsd, output, 6, &out_sz);

    if (0) dump_buf("output", output, out_sz);
    ASSERT_EQ(6, out_sz);
    ASSERT_EQ('f', output[0]);
    ASSERT_EQ('o', output[1]);
    ASSERT_EQ('o', output[2]);
    ASSERT_EQ('f', output[3]);
    ASSERT_EQ('o', output[4]);
    ASSERT_EQ('o', output[5]);

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_expand_short_self_overlapping_backref() {
    /* "aaaaa" == (literal, 1), ('a'), (backref, 1 back, 4 bytes) */
    uint8_t input[] = {0xb0, 0x80, 0x01, 0x80};
    uint8_t output[6];
    uint8_t expected[] = {'a', 'a', 'a', 'a', 'a'};
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 8, 7);
    uint16_t count = 0;
    
    HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, input, sizeof(input), &count);
    ASSERT_EQ(HSDR_SINK_OK, sres);

    uint16_t out_sz = 0;
    (void)heatshrink_decoder_poll(hsd, output, sizeof(output), &out_sz);

    if (0) dump_buf("output", output, out_sz);
    ASSERT_EQ(sizeof(expected), out_sz);
    for (int i=0; i<sizeof(expected); i++) ASSERT_EQ(expected[i], output[i]);

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_suspend_if_out_of_space_in_output_buffer_during_literal_expansion() {
    uint8_t input[] = {0xb3, 0x5b, 0xed, 0xe0, 0x40, 0x80};
    uint8_t output[1];
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 7, 7);
    uint16_t count = 0;
    
    HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, input, sizeof(input), &count);
    ASSERT_EQ(HSDR_SINK_OK, sres);

    uint16_t out_sz = 0;
    HEATSHRINK_DECODER_POLL_RES pres = heatshrink_decoder_poll(hsd, output, 1, &out_sz);
    ASSERT_EQ(HSDR_POLL_MORE, pres);
    ASSERT_EQ(1, out_sz);
    ASSERT_EQ('f', output[0]);

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_suspend_if_out_of_space_in_output_buffer_during_backref_expansion() {
    uint8_t input[] = {0xb3, 0x5b, 0xed, 0xe0, 0x40, 0x80}; //"foofoo"
    uint8_t output[4];
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 7, 7);
    memset(output, 0, sizeof(*output));
    uint16_t count = 0;
    
    HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, input, 6, &count);
    ASSERT_EQ(HSDR_SINK_OK, sres);

    uint16_t out_sz = 0;
    HEATSHRINK_DECODER_POLL_RES pres = heatshrink_decoder_poll(hsd, output, 4, &out_sz);
    ASSERT_EQ(HSDR_POLL_MORE, pres);
    ASSERT_EQ(4, out_sz);
    ASSERT_EQ('f', output[0]);
    ASSERT_EQ('o', output[1]);
    ASSERT_EQ('o', output[2]);
    ASSERT_EQ('f', output[3]);

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_poll_should_expand_short_literal_and_backref_when_fed_input_byte_by_byte() {
    uint8_t input[] = {0xb3, 0x5b, 0xed, 0xe0, 0x40, 0x80}; //"foofoo"
    uint8_t output[7];
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 7, 7);
    memset(output, 0, sizeof(*output));
    uint16_t count = 0;
    
    HEATSHRINK_DECODER_SINK_RES sres;
    for (int i=0; i<6; i++) {
        sres = heatshrink_decoder_sink(hsd, &input[i], 1, &count);
        ASSERT_EQ(HSDR_SINK_OK, sres);
    }
    heatshrink_decoder_finish(hsd);

    uint16_t out_sz = 0;
    HEATSHRINK_DECODER_POLL_RES pres = heatshrink_decoder_poll(hsd, output, 7, &out_sz);
    ASSERT_EQ(6, out_sz);
    ASSERT_EQ(HSDR_POLL_EMPTY, pres);
    ASSERT_EQ('f', output[0]);
    ASSERT_EQ('o', output[1]);
    ASSERT_EQ('o', output[2]);
    ASSERT_EQ('f', output[3]);
    ASSERT_EQ('o', output[4]);
    ASSERT_EQ('o', output[5]);

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_finish_should_reject_null_input() {
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 7, 7);

    HEATSHRINK_DECODER_FINISH_RES exp = HSDR_FINISH_ERROR_NULL;
    ASSERT_EQ(exp, heatshrink_decoder_finish(NULL));

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST decoder_finish_should_note_when_done() {
    uint8_t input[] = {0xb3, 0x5b, 0xed, 0xe0, 0x40, 0x80}; //"foofoo"

    uint8_t output[7];
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 7, 7);
    memset(output, 0, sizeof(*output));
    uint16_t count = 0;
    
    HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, input, sizeof(input), &count);
    ASSERT_EQ(HSDR_SINK_OK, sres);

    uint16_t out_sz = 0;
    HEATSHRINK_DECODER_POLL_RES pres = heatshrink_decoder_poll(hsd, output, sizeof(output), &out_sz);
    ASSERT_EQ(HSDR_POLL_EMPTY, pres);
    ASSERT_EQ(6, out_sz);
    ASSERT_EQ('f', output[0]);
    ASSERT_EQ('o', output[1]);
    ASSERT_EQ('o', output[2]);
    ASSERT_EQ('f', output[3]);
    ASSERT_EQ('o', output[4]);
    ASSERT_EQ('o', output[5]);

    HEATSHRINK_DECODER_FINISH_RES fres = heatshrink_decoder_finish(hsd);
    ASSERT_EQ(HSDR_FINISH_DONE, fres);

    heatshrink_decoder_free(hsd);
    PASS();
}

TEST gen() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 7);
    uint8_t input[] = {'a', 'a', 'a', 'a', 'a'};
    uint8_t output[1024];
    uint16_t copied = 0;
    memset(output, 0, 1024);
    HEATSHRINK_ENCODER_SINK_RES sres = heatshrink_encoder_sink(hse,
        input, sizeof(input), &copied);
    ASSERT_EQ(HSER_SINK_OK, sres);
    ASSERT_EQ(sizeof(input), copied);

    HEATSHRINK_ENCODER_FINISH_RES fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_MORE, fres);

    ASSERT_EQ(HSER_POLL_EMPTY, heatshrink_encoder_poll(hse, output, 1024, &copied));
    fres = heatshrink_encoder_finish(hse);
    ASSERT_EQ(HSER_FINISH_DONE, fres);
    if (0) {
        printf("{");
        for (int i=0; i<copied; i++) printf("0x%02x, ", output[i]);
        printf("}\n");
    }
    heatshrink_encoder_free(hse);
    PASS();
}

SUITE(decoding) {
    RUN_TEST(decoder_alloc_should_reject_excessively_small_window);
    RUN_TEST(decoder_alloc_should_reject_zero_byte_input_buffer);

    RUN_TEST(decoder_sink_should_reject_null_hsd_pointer);
    RUN_TEST(decoder_sink_should_reject_null_input_pointer);
    RUN_TEST(decoder_sink_should_reject_null_count_pointer);
    RUN_TEST(decoder_sink_should_reject_excessively_large_input);
    RUN_TEST(decoder_sink_should_sink_data_when_preconditions_hold);

    RUN_TEST(gen);

    RUN_TEST(decoder_poll_should_return_empty_if_empty);
    RUN_TEST(decoder_poll_should_reject_null_hsd);
    RUN_TEST(decoder_poll_should_reject_null_output_buffer);
    RUN_TEST(decoder_poll_should_reject_null_output_size_pointer);
    RUN_TEST(decoder_poll_should_expand_short_literal);
    RUN_TEST(decoder_poll_should_expand_short_literal_and_backref);
    RUN_TEST(decoder_poll_should_expand_short_self_overlapping_backref);
    RUN_TEST(decoder_poll_should_suspend_if_out_of_space_in_output_buffer_during_literal_expansion);
    RUN_TEST(decoder_poll_should_suspend_if_out_of_space_in_output_buffer_during_backref_expansion);
    RUN_TEST(decoder_poll_should_expand_short_literal_and_backref_when_fed_input_byte_by_byte);

    RUN_TEST(decoder_finish_should_reject_null_input);
    RUN_TEST(decoder_finish_should_note_when_done);
}

typedef struct {
    uint8_t log_lvl;
    uint8_t window_sz2;
    uint8_t lookahead_sz2;
    uint16_t decoder_input_buffer_size;
} cfg_info;

static int compress_and_expand_and_check(uint8_t *input, uint32_t input_size, cfg_info *cfg) {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(cfg->window_sz2,
        cfg->lookahead_sz2);
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(cfg->decoder_input_buffer_size,
        cfg->window_sz2, cfg->lookahead_sz2);
    size_t comp_sz = input_size + (input_size/2) + 4;
    size_t decomp_sz = input_size + (input_size/2) + 4;
    uint8_t *comp = malloc(comp_sz);
    uint8_t *decomp = malloc(decomp_sz);
    if (comp == NULL) FAILm("malloc fail");
    if (decomp == NULL) FAILm("malloc fail");
    memset(comp, 0, comp_sz);
    memset(decomp, 0, decomp_sz);

    uint16_t count = 0;

    if (cfg->log_lvl > 1) {
        printf("\n^^ COMPRESSING\n");
        dump_buf("input", input, input_size);
    }

    uint32_t sunk = 0;
    uint32_t polled = 0;
    while (sunk < input_size) {
        ASSERT(heatshrink_encoder_sink(hse, &input[sunk], input_size - sunk, &count) >= 0);
        sunk += count;
        if (cfg->log_lvl > 1) printf("^^ sunk %d\n", count);
        if (sunk == input_size) {
            ASSERT_EQ(HSER_FINISH_MORE, heatshrink_encoder_finish(hse));
        }

        HEATSHRINK_ENCODER_POLL_RES pres;
        do {                    /* "turn the crank" */
            pres = heatshrink_encoder_poll(hse, &comp[polled], comp_sz - polled, &count);
            ASSERT(pres >= 0);
            polled += count;
            if (cfg->log_lvl > 1) printf("^^ polled %d\n", count);
        } while (pres == HSER_POLL_MORE);
        ASSERT_EQ(HSER_POLL_EMPTY, pres);
        if (polled >= comp_sz) FAILm("compression should never expand that much");
        if (sunk == input_size) {
            ASSERT_EQ(HSER_FINISH_DONE, heatshrink_encoder_finish(hse));
        }
    }
    if (cfg->log_lvl > 0) printf("in: %u compressed: %u ", input_size, polled);
    uint32_t compressed_size = polled;
    sunk = 0;
    polled = 0;
    
    if (cfg->log_lvl > 1) {
        printf("\n^^ DECOMPRESSING\n");
        dump_buf("comp", comp, compressed_size);
    }
    while (sunk < compressed_size) {
        ASSERT(heatshrink_decoder_sink(hsd, &comp[sunk], compressed_size - sunk, &count) >= 0);
        sunk += count;
        if (cfg->log_lvl > 1) printf("^^ sunk %d\n", count);
        if (sunk == compressed_size) {
            ASSERT_EQ(HSDR_FINISH_MORE, heatshrink_decoder_finish(hsd));
        }

        HEATSHRINK_DECODER_POLL_RES pres;
        do {
            pres = heatshrink_decoder_poll(hsd, &decomp[polled],
                decomp_sz - polled, &count);
            ASSERT(pres >= 0);
            ASSERT(count > 0);
            polled += count;
            if (cfg->log_lvl > 1) printf("^^ polled %d\n", count);
        } while (pres == HSDR_POLL_MORE);
        ASSERT_EQ(HSDR_POLL_EMPTY, pres);
        if (sunk == compressed_size) {
            HEATSHRINK_DECODER_FINISH_RES fres = heatshrink_decoder_finish(hsd);
            ASSERT_EQ(HSDR_FINISH_DONE, fres);
        }

        if (polled > input_size) {
            printf("\nExpected %d, got %d\n", input_size, polled);
            FAILm("Decompressed data is larger than original input");
        }
    }
    if (cfg->log_lvl > 0) printf("decompressed: %u\n", polled);
    if (polled != input_size) {
        FAILm("Decompressed length does not match original input length");
    }

    if (cfg->log_lvl > 1) dump_buf("decomp", decomp, polled);
    for (int i=0; i<input_size; i++) {
        if (input[i] != decomp[i]) {
            printf("*** mismatch at %d\n", i);
            if (0) {
                for (int j=0; j<=/*i*/ input_size; j++) {
                    printf("in[%d] == 0x%02x ('%c') => out[%d] == 0x%02x ('%c')  %c\n",
                        j, input[j], isprint(input[j]) ? input[j] : '.',
                        j, decomp[j], isprint(decomp[j]) ? decomp[j] : '.',
                        input[j] == decomp[j] ? ' ' : 'X');
                }
            }
        }
        ASSERT_EQ(input[i], decomp[i]);
    }
    free(comp);
    free(decomp);
    heatshrink_encoder_free(hse);
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST data_without_duplication_should_match() {
    uint8_t input[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
                       'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
                       's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
    cfg_info cfg;
    cfg.log_lvl = 0;
    cfg.window_sz2 = 8;
    cfg.lookahead_sz2 = 3;
    cfg.decoder_input_buffer_size = 256;
    return compress_and_expand_and_check(input, sizeof(input), &cfg);
}

TEST data_with_simple_repetition_should_compress_and_decompress_properly() {
    uint8_t input[] = {'a', 'b', 'c', 'a', 'b', 'c', 'd', 'a', 'b',
                       'c', 'd', 'e', 'a', 'b', 'c', 'd', 'e', 'f',
                       'a', 'b', 'c', 'd', 'e', 'f', 'g', 'a', 'b',
                       'c', 'd', 'e', 'f', 'g', 'h'};
    cfg_info cfg;
    cfg.log_lvl = 0;
    cfg.window_sz2 = 8;
    cfg.lookahead_sz2 = 3;
    cfg.decoder_input_buffer_size = 256;
    return compress_and_expand_and_check(input, sizeof(input), &cfg);
}

TEST data_without_duplication_should_match_with_absurdly_tiny_buffers() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 3);
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 8, 3);
    uint8_t input[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
                       'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
                       's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
    uint8_t comp[60];
    uint8_t decomp[60];
    uint16_t count = 0;
    int log = 0;

    if (log) dump_buf("input", input, sizeof(input));
    for (int i=0; i<sizeof(input); i++) {
        ASSERT(heatshrink_encoder_sink(hse, &input[i], 1, &count) >= 0);
    }
    ASSERT_EQ(HSER_FINISH_MORE, heatshrink_encoder_finish(hse));

    uint16_t packed_count = 0;
    do {
        ASSERT(heatshrink_encoder_poll(hse, &comp[packed_count], 1, &count) >= 0);
        packed_count += count;
    } while (heatshrink_encoder_finish(hse) == HSER_FINISH_MORE);

    if (log) dump_buf("comp", comp, packed_count);
    for (int i=0; i<packed_count; i++) {
        HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, &comp[i], 1, &count);
        //printf("sres is %d\n", sres);
        ASSERT(sres >= 0);
    }

    for (int i=0; i<sizeof(input); i++) {
        ASSERT(heatshrink_decoder_poll(hsd, &decomp[i], 1, &count) >= 0);
    }

    if (log) dump_buf("decomp", decomp, sizeof(input));
    for (int i=0; i<sizeof(input); i++) ASSERT_EQ(input[i], decomp[i]);
    heatshrink_encoder_free(hse);
    heatshrink_decoder_free(hsd);
    PASS();
}

TEST data_with_simple_repetition_should_match_with_absurdly_tiny_buffers() {
    heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 3);
    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 8, 3);
    uint8_t input[] = {'a', 'b', 'c', 'a', 'b', 'c', 'd', 'a', 'b',
                       'c', 'd', 'e', 'a', 'b', 'c', 'd', 'e', 'f',
                       'a', 'b', 'c', 'd', 'e', 'f', 'g', 'a', 'b',
                       'c', 'd', 'e', 'f', 'g', 'h'};
    uint8_t comp[60];
    uint8_t decomp[60];
    uint16_t count = 0;
    int log = 0;

    if (log) dump_buf("input", input, sizeof(input));
    for (int i=0; i<sizeof(input); i++) {
        ASSERT(heatshrink_encoder_sink(hse, &input[i], 1, &count) >= 0);
    }
    ASSERT_EQ(HSER_FINISH_MORE, heatshrink_encoder_finish(hse));

    uint16_t packed_count = 0;
    do {
        ASSERT(heatshrink_encoder_poll(hse, &comp[packed_count], 1, &count) >= 0);
        packed_count += count;
    } while (heatshrink_encoder_finish(hse) == HSER_FINISH_MORE);

    if (log) dump_buf("comp", comp, packed_count);
    for (int i=0; i<packed_count; i++) {
        HEATSHRINK_DECODER_SINK_RES sres = heatshrink_decoder_sink(hsd, &comp[i], 1, &count);
        //printf("sres is %d\n", sres);
        ASSERT(sres >= 0);
    }

    for (int i=0; i<sizeof(input); i++) {
        ASSERT(heatshrink_decoder_poll(hsd, &decomp[i], 1, &count) >= 0);
    }

    if (log) dump_buf("decomp", decomp, sizeof(input));
    for (int i=0; i<sizeof(input); i++) ASSERT_EQ(input[i], decomp[i]);
    heatshrink_encoder_free(hse);
    heatshrink_decoder_free(hsd);
    PASS();
}

static void fill_with_pseudorandom_letters(uint8_t *buf, uint32_t size, uint32_t seed) {
    uint64_t rn = 9223372036854775783; /* prime under 2^64 */
    for (uint32_t i=0; i<size; i++) {
        rn = rn*seed + seed;
        buf[i] = (rn % 26) + 'a';
    }
}

TEST pseudorandom_data_should_match(uint32_t size, uint32_t seed, cfg_info *cfg) {
    uint8_t input[size];
    if (cfg->log_lvl > 0) {
        printf("\n-- size %u, seed %u, input buf %u\n",
            size, seed, cfg->decoder_input_buffer_size);
    }
    fill_with_pseudorandom_letters(input, size, seed);
    return compress_and_expand_and_check(input, size, cfg);
}

TEST small_input_buffer_should_not_impact_decoder_correctness() {
    int size = 5;
    uint8_t input[size];
    cfg_info cfg;
    cfg.log_lvl = 0;
    cfg.window_sz2 = 8;
    cfg.lookahead_sz2 = 3;
    cfg.decoder_input_buffer_size = 5;
    for (uint16_t i=0; i<size; i++) input[i] = 'a' + (i % 26);
    if (compress_and_expand_and_check(input, size, &cfg) != 0) return -1;
    PASS();
}

TEST regression_backreference_counters_should_not_roll_over() {
    /* Searching was scanning the entire context buffer, not just
     * the maximum range addressable by the backref index.*/
    uint32_t size = 337;
    uint32_t seed = 3;
    uint8_t input[size];
    fill_with_pseudorandom_letters(input, size, seed);
    cfg_info cfg;
    cfg.log_lvl = 0;
    cfg.window_sz2 = 8;
    cfg.lookahead_sz2 = 3;
    cfg.decoder_input_buffer_size = 64; // 1
    return compress_and_expand_and_check(input, size, &cfg);
}

TEST regression_index_fail() {
    /* Failured when indexed, cause unknown.
     *
     * This has something to do with bad data at the very last
     * byte being indexed, due to spillover. */
    uint32_t size = 507;
    uint32_t seed = 3;
    uint8_t input[size];
    fill_with_pseudorandom_letters(input, size, seed);
    cfg_info cfg;
    cfg.log_lvl = 0;
    cfg.window_sz2 = 8;
    cfg.lookahead_sz2 = 3;
    cfg.decoder_input_buffer_size = 64;
    return compress_and_expand_and_check(input, size, &cfg);
}

TEST sixty_four_k() {
    /* Regression: An input buffer of 64k should not cause an
     * overflow that leads to an infinite loop. */
    uint32_t size = 64 * 1024;
    uint32_t seed = 1;
    uint8_t input[size];
    fill_with_pseudorandom_letters(input, size, seed);
    cfg_info cfg;
    cfg.log_lvl = 0;
    cfg.window_sz2 = 8;
    cfg.lookahead_sz2 = 3;
    cfg.decoder_input_buffer_size = 64;
    return compress_and_expand_and_check(input, size, &cfg);
}

SUITE(integration) {
    RUN_TEST(data_without_duplication_should_match);
    RUN_TEST(data_with_simple_repetition_should_compress_and_decompress_properly);
    RUN_TEST(data_without_duplication_should_match_with_absurdly_tiny_buffers);
    RUN_TEST(data_with_simple_repetition_should_match_with_absurdly_tiny_buffers);

    // Regressions from fuzzing
    RUN_TEST(small_input_buffer_should_not_impact_decoder_correctness);
    RUN_TEST(regression_backreference_counters_should_not_roll_over);
    RUN_TEST(regression_index_fail);
    RUN_TEST(sixty_four_k);

#if __STDC_VERSION__ >= 19901L
    printf("\n\nFuzzing:\n");
    for (uint32_t size=1; size < 128*1024L; size <<= 1) {
        if (GREATEST_IS_VERBOSE()) printf(" -- size %u\n", size);
        for (uint16_t ibs=32; ibs<=8192; ibs <<= 1) { /* input buffer size */
            if (GREATEST_IS_VERBOSE()) printf(" -- input buffer %u\n", ibs);
            for (uint32_t seed=1; seed<=10; seed++) {
                if (GREATEST_IS_VERBOSE()) printf(" -- seed %u\n", seed);
                cfg_info cfg;
                cfg.log_lvl = 0;
                cfg.window_sz2 = 8;
                cfg.lookahead_sz2 = 3;
                cfg.decoder_input_buffer_size = ibs;
                RUN_TESTp(pseudorandom_data_should_match, size, seed, &cfg);
            }
        }
    }
#endif
}

/* Add all the definitions that need to be in the test runner's main file. */
GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();      /* command-line arguments, initialization. */
    RUN_SUITE(encoding);
    RUN_SUITE(decoding);
    RUN_SUITE(integration);
    GREATEST_MAIN_END();        /* display results */
}
