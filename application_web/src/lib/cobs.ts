/**
 * COBS (Consistent Overhead Byte Stuffing) Encoder
 *
 * Encodes data to remove all zero bytes, replacing them with overhead codes.
 * The packet is terminated with a zero byte.
 */
export function cobsEncode(data: Uint8Array): Uint8Array {
  if (data.length === 0) {
    return new Uint8Array([0x01]);
  }

  const output: number[] = [];
  let codeIndex = 0;
  let code = 1;

  output.push(0); // Placeholder for first code

  for (const byte of data) {
    if (byte === 0) {
      // Found zero - write code and start new segment
      output[codeIndex] = code;
      codeIndex = output.length;
      output.push(0); // Placeholder for next code
      code = 1;
    } else {
      // Copy non-zero byte
      output.push(byte);
      code += 1;

      if (code === 0xFF) {
        // Segment full (254 bytes) - write code and start new segment
        output[codeIndex] = code;
        codeIndex = output.length;
        output.push(0); // Placeholder for next code
        code = 1;
      }
    }
  }

  // Write final code
  output[codeIndex] = code;

  return new Uint8Array(output);
}
