#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "heatshrink_encoder.h"

#define BUFFER_SIZE 256
#define WINDOW_SIZE 8
#define LOOKAHEAD_SIZE 4

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <input file> <output file>\n", argv[0]);
        return 1;
    }

    FILE *input = fopen(argv[1], "rb");
    FILE *output = fopen(argv[2], "wb");
    if (!input || !output) {
        perror("File error");
        return 1;
    }

    heatshrink_encoder *hse = heatshrink_encoder_alloc(WINDOW_SIZE, LOOKAHEAD_SIZE);
    if (!hse) {
        fprintf(stderr, "Failed to allocate Heatshrink encoder\n");
        return 1;
    }

    uint8_t in_buf[BUFFER_SIZE], out_buf[BUFFER_SIZE];
    size_t input_size, output_size;
    HSE_sink_res sink_res;
    HSE_poll_res poll_res;
    HSE_finish_res finish_res;

    while ((input_size = fread(in_buf, 1, BUFFER_SIZE, input)) > 0) {
        size_t input_index = 0;
        while (input_index < input_size) {
            size_t sunk_size = 0;
            sink_res = heatshrink_encoder_sink(hse, &in_buf[input_index], input_size - input_index, &sunk_size);
            if (sink_res != HSER_SINK_OK) {
                fprintf(stderr, "Sink error!\n");
                heatshrink_encoder_free(hse);
                return 1;
            }
            input_index += sunk_size;

            do {
                poll_res = heatshrink_encoder_poll(hse, out_buf, BUFFER_SIZE, &output_size);
                if (poll_res < 0) {
                    fprintf(stderr, "Poll error!\n");
                    heatshrink_encoder_free(hse);
                    return 1;
                }
                if (output_size > 0) {
                    fwrite(out_buf, 1, output_size, output);
                }
            } while (poll_res == HSER_POLL_MORE);
        }
    }

    finish_res = heatshrink_encoder_finish(hse);
    if (finish_res != HSER_FINISH_DONE) {
        fprintf(stderr, "Finish error!\n");
        heatshrink_encoder_free(hse);
        return 1;
    }

    heatshrink_encoder_free(hse);
    fclose(input);
    fclose(output);
    printf("Compression complete!\n");
    return 0;
}
