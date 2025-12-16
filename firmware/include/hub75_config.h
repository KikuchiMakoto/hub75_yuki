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
#define DISPLAY_HEIGHT  32
#define SCAN_ROWS       (DISPLAY_HEIGHT / 2)  // 16 for 1/16 scan

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
#define FRAME_SIZE_RGB565   (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)  // 8192 bytes
#define RECV_BUFFER_SIZE    12000  // Base64 + margin

// ============================================
// Binary transfer mode
// ============================================
// Magic header for binary mode: 0xFF 0x00 (not valid Base64)
#define BINARY_MAGIC_0      0xFF
#define BINARY_MAGIC_1      0x00

#endif // HUB75_CONFIG_H
