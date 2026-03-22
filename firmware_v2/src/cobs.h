/**
 * COBS (Consistent Overhead Byte Stuffing) Encoding/Decoding
 *
 * COBS is a method to transform packet data to eliminate zero bytes,
 * allowing zero bytes to be used as packet delimiters.
 */

#ifndef COBS_H
#define COBS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decode a COBS-encoded packet
 *
 * @param input      Pointer to encoded data (excluding delimiter)
 * @param len        Length of encoded data
 * @param output     Pointer to output buffer for decoded data
 * @param max_output Maximum capacity of output buffer
 * @return           Decoded length on success, 0 on error (invalid encoding or buffer overflow)
 */
size_t cobs_decode(const uint8_t* input, size_t len, uint8_t* output, size_t max_output);

/**
 * Encode data using COBS
 *
 * @param input      Pointer to raw data to encode
 * @param len        Length of raw data
 * @param output     Pointer to output buffer (must be len + ceil(len/254) + 1)
 * @param max_output Maximum capacity of output buffer
 * @return           Encoded length on success, 0 on error
 */
size_t cobs_encode(const uint8_t* input, size_t len, uint8_t* output, size_t max_output);

#ifdef __cplusplus
}
#endif

#endif // COBS_H
