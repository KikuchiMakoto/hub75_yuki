#ifndef HUB75_CONFIG_H
#define HUB75_CONFIG_H

#include <stdint.h>

/* ======================================================================== */
/*  Display Configuration                                                    */
/* ======================================================================== */
#define DISPLAY_WIDTH   128
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT  32
#endif
#define SCAN_ROWS       (DISPLAY_HEIGHT / 2)

/* 9-bit native BCM (512 levels per colour)                                */
#define COLOR_DEPTH     9

/* 5 pixels packed into one 32-bit word (30 bits used, 93.75% efficiency)  */
#define PIXELS_PER_WORD 5
#define WORDS_PER_ROW   ((DISPLAY_WIDTH + PIXELS_PER_WORD - 1) / PIXELS_PER_WORD)
_Static_assert((WORDS_PER_ROW * PIXELS_PER_WORD) >= DISPLAY_WIDTH,
               "WORDS_PER_ROW must cover DISPLAY_WIDTH");

/* ======================================================================== */
/*  Pin Configuration                                                        */
/* ======================================================================== */
#define PIN_R0          0
#define PIN_G0          1
#define PIN_B0          2
#define PIN_R1          3
#define PIN_G1          4
#define PIN_B1          5

#define PIN_CLK         6
#define PIN_LAT         7
#define PIN_OE          8

#define PIN_ADDR_A      9
#define PIN_ADDR_B      10
#define PIN_ADDR_C      11
#define PIN_ADDR_D      12

#define N_ADDR_PINS     4

/* ======================================================================== */
/*  Buffer sizes                                                             */
/* ======================================================================== */
#define FRAME_SIZE_RGB565   (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)
#define RECV_BUFFER_SIZE    (FRAME_SIZE_RGB565 + (FRAME_SIZE_RGB565 / 254) + 200)

#define USB_RX_RING_SIZE    (FRAME_SIZE_RGB565 * 4)
#define STATS_INTERVAL_US   1000000u

#ifndef MAX_FRAME_UPDATE_FPS
#define MAX_FRAME_UPDATE_FPS 60u
#endif

#ifndef CPU_CLOCK_KHZ
#define CPU_CLOCK_KHZ 200000u
#endif

/* ======================================================================== */
/*  Build-time invariants                                                    */
/* ======================================================================== */
_Static_assert(MAX_FRAME_UPDATE_FPS > 0, "MAX_FRAME_UPDATE_FPS must be > 0");
_Static_assert(COLOR_DEPTH == 9u, "COLOR_DEPTH must be 9 for v3 native");
_Static_assert(COLOR_DEPTH >= 1u && COLOR_DEPTH <= 12u, "COLOR_DEPTH range");
_Static_assert((DISPLAY_WIDTH % 4u) == 0u, "DISPLAY_WIDTH multiple of 4");
_Static_assert(DISPLAY_WIDTH >= PIXELS_PER_WORD, "DISPLAY_WIDTH too small");

#endif
