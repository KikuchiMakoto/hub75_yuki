/**
 * HUB75 LED Panel Driver for RP2040 (PlatformIO/Arduino)
 * 
 * Configuration and pin definitions
 */

#ifndef HUB75_CONFIG_H
#define HUB75_CONFIG_H

#include <stdint.h>

// ============================================
// Display Configuration
// ============================================
#define DISPLAY_WIDTH   128

// Display height: 32 or 64 (configurable via build flags)
// Use -D DISPLAY_HEIGHT=64 in platformio.ini for 128x64 panels
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT  32
#endif

#define SCAN_ROWS       (DISPLAY_HEIGHT / 2)  // 16 for 32-row, 32 for 64-row

// Color depth for BCM (Binary Code Modulation)
#define COLOR_DEPTH     6   // 6-bit = 64 levels per color

// ============================================
// Pin Configuration
// ============================================
// RGB Data pins (must be consecutive for PIO)
#define PIN_R0          0
#define PIN_G0          1
#define PIN_B0          2
#define PIN_R1          3
#define PIN_G1          4
#define PIN_B1          5

// Control pins
#define PIN_CLK         6
#define PIN_LAT         7
#define PIN_OE          8

// Row address pins (consecutive)
#define PIN_ADDR_A      9
#define PIN_ADDR_B      10
#define PIN_ADDR_C      11
#define PIN_ADDR_D      12
// #define PIN_ADDR_E   13  // For 64-row panels

#define N_ADDR_PINS     4

// ============================================
// Buffer sizes
// ============================================
// Frame size in bytes: 128x32=8KB, 128x64=16KB
#define FRAME_SIZE_RGB565   (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)

// COBS encoding overhead: 1 byte per 254 bytes + 1
// Max encoded size: FRAME_SIZE + ceil(FRAME_SIZE/254) + 1
// 128x32: 8192 + 33 = 8225 -> 8300 with margin
// 128x64: 16384 + 65 = 16449 -> 16600 with margin
#define RECV_BUFFER_SIZE    (FRAME_SIZE_RGB565 + (FRAME_SIZE_RGB565 / 254) + 200)

#endif // HUB75_CONFIG_H
