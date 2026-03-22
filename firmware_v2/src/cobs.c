#include "cobs.h"

size_t cobs_decode(const uint8_t *input, size_t len, uint8_t *output, size_t max_output) {
    if (len == 0) {
        return 0;
    }

    size_t read_idx = 0;
    size_t write_idx = 0;

    while (read_idx < len) {
        uint8_t code = input[read_idx++];
        if (code == 0) {
            return 0;
        }

        size_t copy_len = (size_t)(code - 1);
        if ((read_idx + copy_len) > len) {
            return 0;
        }
        if ((write_idx + copy_len) > max_output) {
            return 0;
        }

        for (size_t i = 0; i < copy_len; ++i) {
            output[write_idx++] = input[read_idx++];
        }

        if (code != 0xFF && read_idx < len) {
            if (write_idx >= max_output) {
                return 0;
            }
            output[write_idx++] = 0x00;
        }
    }

    return write_idx;
}

size_t cobs_encode(const uint8_t *input, size_t len, uint8_t *output, size_t max_output) {
    if (len == 0 || max_output < 2) {
        return 0;
    }

    size_t read_idx = 0;
    size_t write_idx = 0;
    size_t code_idx = 0;
    uint8_t code = 1;

    output[write_idx++] = 0;
    if (write_idx >= max_output) {
        return 0;
    }

    while (read_idx < len) {
        if (input[read_idx] == 0x00) {
            output[code_idx] = code;
            code = 1;
            code_idx = write_idx;
            if (write_idx >= max_output) {
                return 0;
            }
            output[write_idx++] = 0x00;
            read_idx++;
        } else {
            if (write_idx >= max_output) {
                return 0;
            }
            output[write_idx++] = input[read_idx++];
            code++;
            if (code == 0xFF) {
                output[code_idx] = code;
                code = 1;
                code_idx = write_idx;
                if (write_idx >= max_output) {
                    return 0;
                }
                output[write_idx++] = 0x00;
            }
        }
    }

    output[code_idx] = code;
    return write_idx;
}
