#ifndef HUB75_CONFIG_H
#define HUB75_CONFIG_H

#include <stdint.h>

#define DISPLAY_WIDTH   128
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT  32
#endif
#define SCAN_ROWS       (DISPLAY_HEIGHT / 2)
#define COLOR_DEPTH     6

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

#define FRAME_SIZE_RGB565   (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)
#define RECV_BUFFER_SIZE    (FRAME_SIZE_RGB565 + (FRAME_SIZE_RGB565 / 254) + 200)

#define USB_RX_RING_SIZE    (FRAME_SIZE_RGB565 * 4)
#define STATS_INTERVAL_US   1000000u

#ifndef MAX_FRAME_UPDATE_FPS
#define MAX_FRAME_UPDATE_FPS 180u
#endif

#ifndef CPU_CLOCK_KHZ
#define CPU_CLOCK_KHZ 200000u
#endif

#endif
