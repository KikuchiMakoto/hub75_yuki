/**
 * HUB75 LED Panel Controller for RP2040
 * PlatformIO / Arduino (Earle Philhower core)
 *
 * Core0: USB CDC receive (Base64 encoded RGB565) + BCM conversion
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

// Default to PIO mode if not specified
#ifndef HUB75_USE_PIO
#define HUB75_USE_PIO 1
#endif

#if HUB75_USE_PIO
#include <hardware/pio.h>
#include "hub75.pio.h"
#endif

#ifdef USE_TINYUSB
#include <Adafruit_TinyUSB.h>
#endif
#include "hub75_config.h"

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
        for (uint8_t i = 1; i < code && read_idx < len; i++) {
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

#if HUB75_USE_PIO
// PIO instance and state machine
static PIO hub75_pio = pio0;
static uint sm_data = 0;
#endif

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
// HUB75 Initialize - GPIO only (for boot screen)
// ============================================
void hub75_gpio_init() {
    // Initialize GPIO pins
    for (int pin = PIN_R0; pin <= PIN_ADDR_D; pin++) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }
    gpio_put(PIN_OE, 1);  // Display off

    // Initialize gamma table
    init_gamma(2.2f);

    // Clear buffers
    memset(frame_buffer, 0, sizeof(frame_buffer));
    memset(bcm_planes, 0, sizeof(bcm_planes));
}

#if HUB75_USE_PIO
// ============================================
// HUB75 Initialize - PIO (call after boot screen)
// ============================================
void hub75_pio_init() {
    // Load and init PIO program
    // This takes over GP0-5 (RGB) and GP6 (CLK) from GPIO control
    uint offset = pio_add_program(hub75_pio, &hub75_data_program);
    hub75_data_program_init(hub75_pio, sm_data, offset, PIN_R0, PIN_CLK);
}
#endif

// ============================================
// HUB75 Initialize - Full (legacy, for non-PIO mode)
// ============================================
void hub75_init() {
    hub75_gpio_init();
#if HUB75_USE_PIO
    hub75_pio_init();
#endif
}

// ============================================
// Set row address using direct register access
// ============================================
static inline void __not_in_flash_func(set_row_address)(int row) {
    sio_hw->gpio_clr = ADDR_MASK;
    uint32_t addr_bits = (((row >> 0) & 1) << PIN_ADDR_A) |
                         (((row >> 1) & 1) << PIN_ADDR_B) |
                         (((row >> 2) & 1) << PIN_ADDR_C) |
                         (((row >> 3) & 1) << PIN_ADDR_D);
    sio_hw->gpio_set = addr_bits;
}

#if HUB75_USE_PIO
// ============================================
// HUB75 Refresh - PIO version
// ============================================
void __not_in_flash_func(hub75_refresh)() {
    for (int bit = 0; bit < COLOR_DEPTH; bit++) {
        uint32_t delay_us = 1 << bit;

        for (int row = 0; row < SCAN_ROWS; row++) {
            // 1. Disable output
            sio_hw->gpio_set = OE_MASK;

            // 2. Shift out pixel data via PIO (right to left for chained panels)
            uint8_t* row_data = bcm_planes[row][bit];
            for (int x = DISPLAY_WIDTH - 1; x >= 0; x--) {
                while (pio_sm_is_tx_fifo_full(hub75_pio, sm_data)) {
                    tight_loop_contents();
                }
                pio_sm_put(hub75_pio, sm_data, row_data[x]);
            }

            // Wait for shift complete
            while (!pio_sm_is_tx_fifo_empty(hub75_pio, sm_data)) {
                tight_loop_contents();
            }
            __asm volatile("nop\nnop\nnop\nnop");

            // 3. Set row address
            set_row_address(row);

            // 4. Latch pulse
            sio_hw->gpio_set = LAT_MASK;
            __asm volatile("nop\nnop\nnop\nnop");
            sio_hw->gpio_clr = LAT_MASK;

            // 5. Enable output
            sio_hw->gpio_clr = OE_MASK;

            // 6. BCM delay
            delayMicroseconds(delay_us);

            // 7. Disable output before next row
            sio_hw->gpio_set = OE_MASK;
        }
    }
}

#else // CPU GPIO version

// ============================================
// Shift out one pixel data via GPIO (CPU version - fast register access)
// ============================================
static inline void __not_in_flash_func(shift_out_pixel)(uint8_t data) {
    // Clear RGB pins, then set the ones that should be high
    // Data bits 0-5 map directly to GPIO 0-5
    sio_hw->gpio_clr = RGB_MASK;
    sio_hw->gpio_set = (data & 0x3F);

    // Clock pulse - rising edge latches data into shift register
    sio_hw->gpio_set = CLK_MASK;
    __asm volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");
    sio_hw->gpio_clr = CLK_MASK;
}

// ============================================
// HUB75 Refresh - CPU GPIO version
// ============================================
void __not_in_flash_func(hub75_refresh)() {
    for (int bit = 0; bit < COLOR_DEPTH; bit++) {
        uint32_t delay_us = 1 << bit;

        for (int row = 0; row < SCAN_ROWS; row++) {
            // 1. Disable output (OE HIGH)
            sio_hw->gpio_set = OE_MASK;

            // 2. Shift out pixel data (right to left for chained panels)
            uint8_t* row_data = bcm_planes[row][bit];
            for (int x = DISPLAY_WIDTH - 1; x >= 0; x--) {
                shift_out_pixel(row_data[x]);
            }

            // 3. Set row address
            set_row_address(row);

            // 4. Latch pulse
            sio_hw->gpio_set = LAT_MASK;
            __asm volatile("nop\nnop\nnop\nnop");
            sio_hw->gpio_clr = LAT_MASK;

            // 5. Enable output (OE LOW)
            sio_hw->gpio_clr = OE_MASK;

            // 6. BCM delay - display this bit plane
            delayMicroseconds(delay_us);

            // 7. Disable output before next row
            sio_hw->gpio_set = OE_MASK;
        }
    }
}
#endif // HUB75_USE_PIO

// ============================================
// Simple row display for boot screen (no BCM, max brightness)
// Uses direct GPIO for both PIO and non-PIO modes
// ============================================
void __not_in_flash_func(display_solid_color)(uint8_t color_mask, int duration_ms) {
    // color_mask: bit0=R0, bit1=G0, bit2=B0, bit3=R1, bit4=G1, bit5=B1
    uint32_t start = millis();

    while (millis() - start < (uint32_t)duration_ms) {
        for (int row = 0; row < SCAN_ROWS; row++) {
            // 1. Disable output
            sio_hw->gpio_set = OE_MASK;

            // 2. Shift out all pixels with the solid color (direct GPIO)
            for (int x = 0; x < DISPLAY_WIDTH; x++) {
                sio_hw->gpio_clr = RGB_MASK;
                sio_hw->gpio_set = (color_mask & 0x3F);

                sio_hw->gpio_set = CLK_MASK;
                __asm volatile("nop\nnop\nnop\nnop");
                sio_hw->gpio_clr = CLK_MASK;
            }

            // 3. Set row address
            set_row_address(row);

            // 4. Latch
            sio_hw->gpio_set = LAT_MASK;
            __asm volatile("nop\nnop\nnop\nnop");
            sio_hw->gpio_clr = LAT_MASK;

            // 5. Enable output
            sio_hw->gpio_clr = OE_MASK;

            // 6. Display time per row
            delayMicroseconds(100);

            // 7. Disable before next row
            sio_hw->gpio_set = OE_MASK;
        }
    }
}

// ============================================
// Boot Screen: RGB color animation (max brightness)
// ============================================
void show_boot_screen() {
    // Show solid RED (R0=1, R1=8 -> 0x09)
    display_solid_color(0x09, 500);

    // Show solid GREEN (G0=2, G1=16 -> 0x12)
    display_solid_color(0x12, 500);

    // Show solid BLUE (B0=4, B1=32 -> 0x24)
    display_solid_color(0x24, 500);

    // Show WHITE briefly (all colors on -> 0x3F)
    display_solid_color(0x3F, 300);

    // Clear display after boot
    memset(bcm_planes, 0, sizeof(bcm_planes));
}

// ============================================
// Core1: Display Refresh ONLY (flicker-free)
// ============================================
void setup1() {
    // Initialize GPIO pins first (before PIO takes over)
    hub75_gpio_init();

    // Show boot screen animation using direct GPIO
    show_boot_screen();
    boot_complete = true;

#if HUB75_USE_PIO
    // Now initialize PIO (takes over GP0-5 and GP6 from GPIO)
    hub75_pio_init();
#endif
}

void loop1() {
    // Core1: Display refresh ONLY - no other operations
    // Frame check and BCM conversion moved to Core0 to prevent display flickering
    hub75_refresh();
}

// ============================================
// Core0: USB CDC Reception + BCM Conversion
// ============================================
void setup() {
    Serial.begin(115200);  // Baud ignored for USB CDC

    memset(recv_buffer, 0, sizeof(recv_buffer));

    delay(500);  // Wait for USB
}

void loop() {
    // Simplified frame reception (Reference: LED_Matrix_firmware_K00798)
    // Process serial data and update display immediately when frame is complete
    while (Serial.available()) {
        uint8_t c = Serial.read();

        if (c == 0x00) {
            // Packet delimiter received - decode COBS packet
            if (recv_pos > 0) {
                // Decode COBS into temporary buffer
                size_t decoded_len = cobs_decode(recv_buffer, recv_pos,
                                                 decode_buffer, FRAME_SIZE_RGB565);

                // Verify size matches expected frame size
                if (decoded_len == FRAME_SIZE_RGB565) {
                    // Valid frame - copy directly and convert immediately
                    // This approach is simpler and more stable for video playback
                    memcpy(frame_buffer, decode_buffer, FRAME_SIZE_RGB565);
                    convert_to_bcm(frame_buffer);
                }
                // Invalid packets are silently discarded
            }
            // Reset for next packet
            recv_pos = 0;

        } else {
            // Accumulate COBS-encoded data
            if (recv_pos < RECV_BUFFER_SIZE) {
                recv_buffer[recv_pos++] = c;
            } else {
                // Buffer overflow - discard packet and reset
                recv_pos = 0;
            }
        }
    }
}
