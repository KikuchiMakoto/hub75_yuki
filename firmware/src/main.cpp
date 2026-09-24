/**
 * HUB75 LED Panel Controller for RP2040
 * PlatformIO / Arduino (Earle Philhower core)
 *
 * Core0: USB CDC receive (COBS encoded RGB565) + BCM conversion
 * Core1: HUB75 panel refresh ONLY (no other operations for flicker-free display)
 *
 * Build options (platformio.ini):
 *   -D HUB75_USE_PIO=1  : Use PIO for high-speed shifting (default)
 *   -D HUB75_USE_PIO=0  : Use CPU GPIO bit-banging
 *
 * Pin connections:
 *   GP0-5:   R0,G0,B0,R1,G1,B1 (RGB data)
 *   GP6:     CLK (clock)
 *   GP7:     LAT (latch)
 *   GP8:     OE  (output enable, active LOW)
 *   GP9-12:  A,B,C,D (row address)
 */

#include <Arduino.h>
#include <hardware/gpio.h>
#include <hardware/structs/sio.h>
#include <hardware/vreg.h>
#include <hardware/clocks.h>
#include <hardware/timer.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include "hub75.pio.h"

#if USE_TINYUSB
#include <Adafruit_TinyUSB.h>
#endif
#include "hub75_config.h"
#include "standalone_dem.h"

// GPIO masks for fast register access
#define RGB_MASK    ((1 << PIN_R0) | (1 << PIN_G0) | (1 << PIN_B0) | \
                     (1 << PIN_R1) | (1 << PIN_G1) | (1 << PIN_B1))
#define CLK_MASK    (1 << PIN_CLK)
#define LAT_MASK    (1 << PIN_LAT)
#define OE_MASK     (1 << PIN_OE)
#define ADDR_MASK   ((1 << PIN_ADDR_A) | (1 << PIN_ADDR_B) | \
                     (1 << PIN_ADDR_C) | (1 << PIN_ADDR_D))

// ============================================
// COBS (Consistent Overhead Byte Stuffing) Decoder
// ============================================
// Decodes COBS-encoded data back to original payload.
// Returns decoded length, or 0 on error.
// Max overhead: 1 byte per 254 bytes of data + 1 byte
size_t cobs_decode(const uint8_t* input, size_t len, uint8_t* output, size_t max_output) {
    if (len == 0) return 0;

    size_t read_idx = 0;
    size_t write_idx = 0;

    while (read_idx < len) {
        uint8_t code = input[read_idx++];

        if (code == 0) {
            // Invalid: zero byte in encoded data
            return 0;
        }

        // Copy (code - 1) bytes
        uint8_t copy_len = code - 1;
        if (read_idx + copy_len > len) {
            return 0;  // Truncated packet
        }

        for (uint8_t i = 0; i < copy_len; i++) {
            // Bounds check
            if (write_idx >= max_output) {
                return 0;  // Buffer overflow
            }
            output[write_idx++] = input[read_idx++];
        }

        // Add zero byte if code != 0xFF and not at end
        if (code != 0xFF && read_idx < len) {
            if (write_idx >= max_output) {
                return 0;  // Buffer overflow
            }
            output[write_idx++] = 0x00;
        }
    }

    return write_idx;
}

// ============================================
// Frame Buffers
// ============================================
// Simplified single-buffer approach for stable operation
// (Reference: LED_Matrix_firmware_K00798)
static uint16_t frame_buffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static volatile bool frame_ready = false;
static uint16_t standalone_frame[64][64];

// BCM bit planes: [row][bit][x] = packed 6-bit RGB
static uint8_t bcm_planes[SCAN_ROWS][COLOR_DEPTH][DISPLAY_WIDTH];

// COBS receive buffer
static uint8_t recv_buffer[RECV_BUFFER_SIZE];
static size_t recv_pos = 0;

// Decode buffer for COBS output
static uint8_t decode_buffer[FRAME_SIZE_RGB565];

// Gamma table
static uint8_t gamma_tbl[256];

// Boot screen complete flag
static volatile bool boot_complete = false;

// PIO instance and state machine
static PIO hub75_pio = pio0;
static uint sm_data = 0;

// DMA channel for PIO data transfer
static int dma_chan = -1;

// Double buffer for DMA (ping-pong)
// Each pixel is expanded from 8-bit to 32-bit for PIO FIFO
static uint32_t dma_buffer[2][DISPLAY_WIDTH];

// ============================================
// Initialize gamma table
// ============================================
void init_gamma(float gamma_val) {
    for (int i = 0; i < 256; i++) {
        float norm = (float)i / 255.0f;
        gamma_tbl[i] = (uint8_t)(powf(norm, gamma_val) * 255.0f + 0.5f);
    }
}

// ============================================
// Convert RGB565 frame to BCM planes
// ============================================
void convert_to_bcm(uint16_t* pixels) {
    for (int row = 0; row < SCAN_ROWS; row++) {
        int y_upper = row;
        int y_lower = row + SCAN_ROWS;

        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            uint16_t p_up = pixels[y_upper * DISPLAY_WIDTH + x];
            uint16_t p_lo = pixels[y_lower * DISPLAY_WIDTH + x];

            // Extract and scale to 8-bit, then apply gamma
            uint8_t r0 = gamma_tbl[((p_up >> 11) & 0x1F) << 3];
            uint8_t g0 = gamma_tbl[((p_up >> 5) & 0x3F) << 2];
            uint8_t b0 = gamma_tbl[(p_up & 0x1F) << 3];

            uint8_t r1 = gamma_tbl[((p_lo >> 11) & 0x1F) << 3];
            uint8_t g1 = gamma_tbl[((p_lo >> 5) & 0x3F) << 2];
            uint8_t b1 = gamma_tbl[(p_lo & 0x1F) << 3];

            // Scale 8-bit to COLOR_DEPTH bits
            r0 >>= (8 - COLOR_DEPTH);
            g0 >>= (8 - COLOR_DEPTH);
            b0 >>= (8 - COLOR_DEPTH);
            r1 >>= (8 - COLOR_DEPTH);
            g1 >>= (8 - COLOR_DEPTH);
            b1 >>= (8 - COLOR_DEPTH);

            // Pack into bit planes
            for (int bit = 0; bit < COLOR_DEPTH; bit++) {
                uint8_t mask = 1 << bit;
                uint8_t packed = 0;
                if (r0 & mask) packed |= 0x01;
                if (g0 & mask) packed |= 0x02;
                if (b0 & mask) packed |= 0x04;
                if (r1 & mask) packed |= 0x08;
                if (g1 & mask) packed |= 0x10;
                if (b1 & mask) packed |= 0x20;
                bcm_planes[row][bit][x] = packed;
            }
        }
    }
}

// ============================================
// Convert 64x64 DEM frame to BCM planes (Ultra-fast direct bit extraction)
// Prioritizes pure speed and throughput over perceptual gamma curve
// ============================================
void convert_64x64_to_bcm(const uint16_t frame[64][64]) {
    for (int row = 0; row < SCAN_ROWS; row++) {
        const uint16_t *p2_up = &frame[63 - row][0];
        const uint16_t *p2_lo = &frame[47 - row][0];
        const uint16_t *p1_up = &frame[row][0];
        const uint16_t *p1_lo = &frame[16 + row][0];

        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            uint16_t up, lo;
            if (x < 64) {
                // Panel 2 (Bottom Panel, Y: 32..63) - ROTATED 180 DEGREES
                int col = 63 - x;
                up = p2_up[col];
                lo = p2_lo[col];
            } else {
                // Panel 1 (Top Panel, Y: 0..31) - NORMAL ORIENTATION (0 deg)
                int col = x - 64;
                up = p1_up[col];
                lo = p1_lo[col];
            }

            // Directly unpack RGB565 to 6-bit color channels (with 5->6 bit replication for LSB)
            // r: 5 bits -> (r << 1) | (r >> 4) -> (0..63)
            // g: 6 bits -> (0..63)
            // b: 5 bits -> (b << 1) | (b >> 4) -> (0..63)
            uint32_t r0 = ((up >> 10) & 0x3E) | ((up >> 15) & 1);
            uint32_t g0 = (up >> 5)  & 0x3F;
            uint32_t b0 = ((up << 1) & 0x3E) | ((up >> 4) & 1);

            uint32_t r1 = ((lo >> 10) & 0x3E) | ((lo >> 15) & 1);
            uint32_t g1 = (lo >> 5)  & 0x3F;
            uint32_t b1 = ((lo << 1) & 0x3E) | ((lo >> 4) & 1);

            // Direct bit extraction into BCM planes (unrolled 6 planes, single-cycle shifts)
            bcm_planes[row][0][x] = ((r0 & 1)) | ((g0 & 1) << 1) | ((b0 & 1) << 2) |
                                    ((r1 & 1) << 3) | ((g1 & 1) << 4) | ((b1 & 1) << 5);
            bcm_planes[row][1][x] = (((r0 >> 1) & 1)) | (((g0 >> 1) & 1) << 1) | (((b0 >> 1) & 1) << 2) |
                                    (((r1 >> 1) & 1) << 3) | (((g1 >> 1) & 1) << 4) | (((b1 >> 1) & 1) << 5);
            bcm_planes[row][2][x] = (((r0 >> 2) & 1)) | (((g0 >> 2) & 1) << 1) | (((b0 >> 2) & 1) << 2) |
                                    (((r1 >> 2) & 1) << 3) | (((g1 >> 2) & 1) << 4) | (((b1 >> 2) & 1) << 5);
            bcm_planes[row][3][x] = (((r0 >> 3) & 1)) | (((g0 >> 3) & 1) << 1) | (((b0 >> 3) & 1) << 2) |
                                    (((r1 >> 3) & 1) << 3) | (((g1 >> 3) & 1) << 4) | (((b1 >> 3) & 1) << 5);
            bcm_planes[row][4][x] = (((r0 >> 4) & 1)) | (((g0 >> 4) & 1) << 1) | (((b0 >> 4) & 1) << 2) |
                                    (((r1 >> 4) & 1) << 3) | (((g1 >> 4) & 1) << 4) | (((b1 >> 4) & 1) << 5);
            bcm_planes[row][5][x] = (((r0 >> 5) & 1)) | (((g0 >> 5) & 1) << 1) | (((b0 >> 5) & 1) << 2) |
                                    (((r1 >> 5) & 1) << 3) | (((g1 >> 5) & 1) << 4) | (((b1 >> 5) & 1) << 5);
        }
    }
}

// ============================================
// HUB75 Initialize - GPIO only (for boot screen)
// ============================================
void hub75_gpio_init() {
    // Initialize GPIO pins with robust drive strength for 250 MHz / ribbon cable
    for (int pin = PIN_R0; pin <= PIN_ADDR_D; pin++) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
        gpio_put(pin, 0);
    }
    gpio_put(PIN_OE, 1);  // Display off

    // Initialize gamma table
    init_gamma(2.2f);

    // Clear buffers
    memset(frame_buffer, 0, sizeof(frame_buffer));
    memset(bcm_planes, 0, sizeof(bcm_planes));
}

// ============================================
// HUB75 Initialize - PIO
// ============================================
void hub75_pio_init() {
    // Load and init PIO program
    // This takes over GP0-5 (RGB) and GP6 (CLK) from GPIO control
    uint offset = pio_add_program(hub75_pio, &hub75_data_program);
    hub75_data_program_init(hub75_pio, sm_data, offset, PIN_R0, PIN_CLK);
}

// ============================================
// DMA Initialize - for PIO data transfer
// ============================================
void hub75_dma_init() {
    // Claim a free DMA channel
    dma_chan = dma_claim_unused_channel(true);

    // Configure DMA channel
    dma_channel_config c = dma_channel_get_default_config(dma_chan);

    // Transfer 32-bit words
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

    // Increment read address, fixed write address (PIO FIFO)
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);

    // Pace transfers based on PIO TX FIFO
    channel_config_set_dreq(&c, pio_get_dreq(hub75_pio, sm_data, true));

    // Save config (will be used for each transfer)
    dma_channel_set_config(dma_chan, &c, false);

    // Set write address to PIO TX FIFO (fixed for all transfers)
    dma_channel_set_write_addr(dma_chan, &hub75_pio->txf[sm_data], false);
}

// ============================================
// Set row address atomically (glitch-free)
// ============================================
static inline void __not_in_flash_func(set_row_address)(int row) {
    uint32_t addr_bits = (((row >> 0) & 1) << PIN_ADDR_A) |
                         (((row >> 1) & 1) << PIN_ADDR_B) |
                         (((row >> 2) & 1) << PIN_ADDR_C) |
                         (((row >> 3) & 1) << PIN_ADDR_D);
    gpio_put_masked(ADDR_MASK, addr_bits);
}

// ============================================
// Prepare DMA buffer for a specific row/bit
// ============================================
static inline void __not_in_flash_func(prepare_dma_buffer)(int buf_idx, int row, int bit) {
    uint8_t* row_data = bcm_planes[row][bit];
    uint32_t* buf = dma_buffer[buf_idx];
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
        // Reverse order for right-to-left shifting
        buf[x] = row_data[DISPLAY_WIDTH - 1 - x];
    }
}

// ============================================
// HUB75 Refresh - PIO + DMA version (Phase 2)
// Double-buffered: prepares next row while current row is being transferred
// ============================================
void __not_in_flash_func(hub75_refresh)() {
    int buf_idx = 0;

    for (int bit = 0; bit < COLOR_DEPTH; bit++) {
        uint32_t delay_us = 1 << bit;

        for (int row = 0; row < SCAN_ROWS; row++) {
            // 1. Disable output (blanking)
            sio_hw->gpio_set = OE_MASK;

            // 2. Prepare current row's DMA buffer
            prepare_dma_buffer(buf_idx, row, bit);

            // 3. Start DMA transfer
            dma_channel_set_read_addr(dma_chan, dma_buffer[buf_idx], false);
            dma_channel_set_trans_count(dma_chan, DISPLAY_WIDTH, true);

            // 4. While DMA is running, prepare next buffer (pipelining)
            int next_buf = 1 - buf_idx;
            int next_row = row + 1;
            int next_bit = bit;
            if (next_row >= SCAN_ROWS) {
                next_row = 0;
                next_bit = bit + 1;
            }
            if (next_bit < COLOR_DEPTH) {
                prepare_dma_buffer(next_buf, next_row, next_bit);
            }

            // 5. Wait for DMA complete
            dma_channel_wait_for_finish_blocking(dma_chan);

            // 6. Wait for PIO to finish shifting the final pixel.
            // TX-FIFO-empty only means the last word left the FIFO; the 6-bit
            // shift + CLK edges still need ~1 pixel time (~135 ns at the
            // 133 MHz-equivalent PIO rate). 1 us covers it at any sysclk.
            while (!pio_sm_is_tx_fifo_empty(hub75_pio, sm_data)) {
                tight_loop_contents();
            }
            busy_wait_us_32(1);

            // 7. Latch pulse: latch shifted data into LED driver IC outputs
            // Must latch BEFORE switching row address to prevent row ghosting/doubling.
            sio_hw->gpio_set = LAT_MASK;
            busy_wait_us_32(1);
            sio_hw->gpio_clr = LAT_MASK;
            busy_wait_us_32(1);

            // 8. Switch row address atomically while display is disabled (OE HIGH)
            set_row_address(row);
            busy_wait_us_32(1); // Settling delay for 74HC138 decoder and row MOSFETs

            // 9. Enable output
            sio_hw->gpio_clr = OE_MASK;

            // 10. BCM delay (display time for this bit plane)
            busy_wait_us_32(delay_us);

            // 11. Disable output before next row
            sio_hw->gpio_set = OE_MASK;

            // Switch to next buffer
            buf_idx = next_buf;
        }
    }
}

// ============================================
// Core1: Display Refresh ONLY (flicker-free)
// ============================================
void setup1() {
    // Initialize GPIO pins first (before PIO takes over)
    hub75_gpio_init();
    boot_complete = true;

    // Initialize PIO and DMA for HUB75 hardware driving
    hub75_pio_init();
    hub75_dma_init();
}

void loop1() {
    // Core1: Display refresh ONLY - 100% dedicated to uninterrupted HUB75 driving
    // Guarantees zero display flicker and rock-solid ~350 Hz refresh rate
    hub75_refresh();
}

// ============================================
// Core0: Standalone DEM Simulation + USB CDC Receiver
// ============================================
static uint32_t last_usb_frame_time = 0;

void setup() {
    // 1. Stable Overclock to 250 MHz (raise VREG to 1.20V first)
    // 250 MHz is the optimal sweet spot where USB CDC remains fully functional
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    delay(10);
    set_sys_clock_khz(250000, true);

    Serial.begin(115200);  // Baud ignored for USB CDC
    memset(recv_buffer, 0, sizeof(recv_buffer));

    // 2. Initialize Standalone 2D-DEM & ADXL335 (GP26 X, GP27 Y)
    dem_init();

    delay(200);
}

void loop() {
    // 1. Check for incoming USB CDC serial frames from PC
    while (Serial.available()) {
        uint8_t c = Serial.read();

        if (c == 0x00) {
            // Packet delimiter received - decode COBS packet
            if (recv_pos > 0) {
                size_t decoded_len = cobs_decode(recv_buffer, recv_pos,
                                                 decode_buffer, FRAME_SIZE_RGB565);
                if (decoded_len == FRAME_SIZE_RGB565) {
                    memcpy(frame_buffer, decode_buffer, FRAME_SIZE_RGB565);
                    convert_to_bcm(frame_buffer);
                    last_usb_frame_time = millis();
                }
            }
            recv_pos = 0;
        } else {
            if (recv_pos < RECV_BUFFER_SIZE) {
                recv_buffer[recv_pos++] = c;
            } else {
                recv_pos = 0;
            }
        }
    }

    // 2. Standalone Mode: if no USB serial frame received in last 500 ms,
    // execute real-time DEM physics simulation with ADXL335!
    if (millis() - last_usb_frame_time > 500) {
        static uint32_t s_frame_count = 0;
        static uint32_t s_last_fps_time = 0;
        static uint32_t s_total_step_us = 0;

        uint32_t t_start = time_us_32();

        // Step DEM simulation
        dem_step();

        // Render directly to frame buffer & convert to BCM planes on Core0
        // (Core1 stays 100% dedicated to uninterrupted refresh, preventing all flicker)
        dem_render(standalone_frame);
        convert_64x64_to_bcm(standalone_frame);

        uint32_t t_step = time_us_32();

        s_frame_count++;
        s_total_step_us += (t_step - t_start);

        uint32_t now_ms = millis();
        // Telemetry only while USB CDC is actually connected: a blocking
        // printf into an undrained CDC FIFO stalls the whole loop (fps collapse).
        if (now_ms - s_last_fps_time >= 1000 && Serial) {
            float fps = (float)s_frame_count * 1000.0f / (float)(now_ms - s_last_fps_time);
            uint32_t avg_step_us = s_frame_count ? (s_total_step_us / s_frame_count) : 0;
            Serial.printf("[DEM] FPS: %.1f | Step: %lu us | Sensor: %s (raw: %u,%u | g: %.2f,%.2f)\n",
                   fps, avg_step_us,
                   g_dem_sensor_connected ? "OK" : "NO_SENSOR (Default +1G Down)",
                   g_dem_last_raw_x, g_dem_last_raw_y,
                   (float)g_dem_last_gx / (float)DEM_GRAVITY_SCALE,
                   (float)g_dem_last_gy / (float)DEM_GRAVITY_SCALE);
            s_frame_count = 0;
            s_total_step_us = 0;
            s_last_fps_time = now_ms;
        }
    }
}
