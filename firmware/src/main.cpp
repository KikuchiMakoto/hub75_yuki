/**
 * HUB75 LED Panel Controller for RP2040
 * PlatformIO / Arduino (Earle Philhower core)
 *
 * Core0: USB CDC receive (Base64 encoded RGB565)
 * Core1: HUB75 panel driving (CPU GPIO-based BCM)
 *
 * Pin connections:
 *   GP0-5:   R0,G0,B0,R1,G1,B1 (RGB data)
 *   GP6:     CLK (clock)
 *   GP7:     LAT (latch)
 *   GP8:     OE  (output enable, active LOW)
 *   GP9-12:  A,B,C,D (row address)
 */

#include <Arduino.h>
// PIO disabled - using CPU GPIO instead
// #include <hardware/pio.h>
// #include <hardware/dma.h>
#ifdef USE_TINYUSB
#include <Adafruit_TinyUSB.h>
#endif
#include "hub75_config.h"
// PIO program disabled
// #include "hub75.pio.h"

// ============================================
// Base64 Decoder
// ============================================
static const uint8_t b64_table[256] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
};

size_t base64_decode_length(const uint8_t* input, size_t len) {
    if (len == 0) return 0;
    size_t padding = 0;
    if (len >= 1 && input[len-1] == '=') padding++;
    if (len >= 2 && input[len-2] == '=') padding++;
    return (len * 3) / 4 - padding;
}

size_t base64_decode(const uint8_t* input, size_t len, uint8_t* output) {
    size_t out_len = 0;
    uint32_t accum = 0;
    int bits = 0;
    
    for (size_t i = 0; i < len; i++) {
        uint8_t c = input[i];
        if (c == '=') break;
        uint8_t val = b64_table[c];
        if (val == 64) continue;
        
        accum = (accum << 6) | val;
        bits += 6;
        
        if (bits >= 8) {
            bits -= 8;
            output[out_len++] = (accum >> bits) & 0xFF;
        }
    }
    return out_len;
}

// ============================================
// Frame Buffers
// ============================================
static uint16_t frame_buffer[2][DISPLAY_WIDTH * DISPLAY_HEIGHT];
static volatile uint8_t write_buf = 0;
static volatile uint8_t read_buf = 0;
static volatile bool new_frame = false;

// BCM bit planes: [row][bit][x] = packed 6-bit RGB
static uint8_t bcm_planes[SCAN_ROWS][COLOR_DEPTH][DISPLAY_WIDTH];

// Receive buffer
static uint8_t recv_buffer[RECV_BUFFER_SIZE];
static size_t recv_pos = 0;

// Gamma table
static uint8_t gamma_tbl[256];

// Boot screen complete flag
static volatile bool boot_complete = false;

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
// HUB75 Initialize (CPU GPIO version)
// ============================================
void hub75_init() {
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

    // PIO initialization removed - using CPU GPIO instead
}

// ============================================
// Shift out one pixel data via GPIO (CPU version)
// ============================================
static inline void shift_out_pixel(uint8_t data) {
    // Set RGB data pins (GP0-5)
    gpio_put(PIN_R0, (data >> 0) & 1);
    gpio_put(PIN_G0, (data >> 1) & 1);
    gpio_put(PIN_B0, (data >> 2) & 1);
    gpio_put(PIN_R1, (data >> 3) & 1);
    gpio_put(PIN_G1, (data >> 4) & 1);
    gpio_put(PIN_B1, (data >> 5) & 1);

    // Clock pulse
    gpio_put(PIN_CLK, 1);
    __asm volatile("nop\nnop\nnop\nnop");
    gpio_put(PIN_CLK, 0);
    __asm volatile("nop\nnop");
}

// ============================================
// HUB75 Refresh (called from Core1) - CPU GPIO version
// ============================================
void hub75_refresh() {
    for (int bit = 0; bit < COLOR_DEPTH; bit++) {
        uint32_t delay = 1 << bit;

        for (int row = 0; row < SCAN_ROWS; row++) {
            // Disable output
            gpio_put(PIN_OE, 1);

            // Shift out pixel data (right to left for chained panels)
            uint8_t* row_data = bcm_planes[row][bit];

            for (int x = DISPLAY_WIDTH - 1; x >= 0; x--) {
                shift_out_pixel(row_data[x]);
            }

            // Set row address
            gpio_put(PIN_ADDR_A, (row >> 0) & 1);
            gpio_put(PIN_ADDR_B, (row >> 1) & 1);
            gpio_put(PIN_ADDR_C, (row >> 2) & 1);
            gpio_put(PIN_ADDR_D, (row >> 3) & 1);

            // Latch
            gpio_put(PIN_LAT, 1);
            __asm volatile("nop\nnop");
            gpio_put(PIN_LAT, 0);

            // Enable output
            gpio_put(PIN_OE, 0);

            // BCM delay
            delayMicroseconds(delay);

            // Disable output
            gpio_put(PIN_OE, 1);
        }
    }
}

// ============================================
// Boot Screen: RGB color sweep animation
// ============================================
void show_boot_screen() {
    // RGB color sweep: Red -> Green -> Blue flows across the screen
    const int ANIM_FRAMES = 60;
    const int SWEEP_WIDTH = 32;

    for (int frame = 0; frame < ANIM_FRAMES; frame++) {
        // Calculate sweep position for each color
        int red_pos = frame * 5 - SWEEP_WIDTH;
        int green_pos = red_pos - 50;
        int blue_pos = green_pos - 50;

        // Fill frame buffer with RGB sweep
        for (int y = 0; y < DISPLAY_HEIGHT; y++) {
            for (int x = 0; x < DISPLAY_WIDTH; x++) {
                uint8_t r = 0, g = 0, b = 0;

                // Red sweep
                if (x >= red_pos && x < red_pos + SWEEP_WIDTH) {
                    int dist = x - red_pos;
                    if (dist < SWEEP_WIDTH / 2) {
                        r = (dist * 255) / (SWEEP_WIDTH / 2);
                    } else {
                        r = ((SWEEP_WIDTH - dist) * 255) / (SWEEP_WIDTH / 2);
                    }
                }

                // Green sweep
                if (x >= green_pos && x < green_pos + SWEEP_WIDTH) {
                    int dist = x - green_pos;
                    if (dist < SWEEP_WIDTH / 2) {
                        g = (dist * 255) / (SWEEP_WIDTH / 2);
                    } else {
                        g = ((SWEEP_WIDTH - dist) * 255) / (SWEEP_WIDTH / 2);
                    }
                }

                // Blue sweep
                if (x >= blue_pos && x < blue_pos + SWEEP_WIDTH) {
                    int dist = x - blue_pos;
                    if (dist < SWEEP_WIDTH / 2) {
                        b = (dist * 255) / (SWEEP_WIDTH / 2);
                    } else {
                        b = ((SWEEP_WIDTH - dist) * 255) / (SWEEP_WIDTH / 2);
                    }
                }

                // Convert to RGB565
                uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                frame_buffer[0][y * DISPLAY_WIDTH + x] = rgb565;
            }
        }

        // Convert to BCM and refresh display multiple times for visibility
        convert_to_bcm(frame_buffer[0]);
        for (int refresh = 0; refresh < 8; refresh++) {
            hub75_refresh();
        }
    }

    // Clear display after boot animation
    memset(frame_buffer[0], 0, sizeof(frame_buffer[0]));
    convert_to_bcm(frame_buffer[0]);
}

// ============================================
// Core1: Display Driver
// ============================================
void setup1() {
    hub75_init();

    // Show boot screen animation
    show_boot_screen();
    boot_complete = true;
}

void loop1() {
    // Check for new frame
    if (new_frame) {
        read_buf = write_buf;
        new_frame = false;
        convert_to_bcm(frame_buffer[read_buf]);
    }

    // Continuous refresh
    hub75_refresh();
}

// ============================================
// Core0: USB CDC Reception
// ============================================
void setup() {
    Serial.begin(115200);  // Baud ignored for USB CDC
    
    memset(recv_buffer, 0, sizeof(recv_buffer));
    
    delay(500);  // Wait for USB
}

void loop() {
    while (Serial.available()) {
        uint8_t c = Serial.read();
        
        if (c == '\n') {
            // Decode frame
            if (recv_pos > 0) {
                size_t expected = base64_decode_length(recv_buffer, recv_pos);
                
                if (expected == FRAME_SIZE_RGB565) {
                    uint8_t target = 1 - read_buf;
                    size_t decoded = base64_decode(recv_buffer, recv_pos,
                                                   (uint8_t*)frame_buffer[target]);
                    
                    if (decoded == FRAME_SIZE_RGB565) {
                        write_buf = target;
                        new_frame = true;
                        Serial.write('K');  // ACK
                    } else {
                        Serial.write('E');
                    }
                } else {
                    Serial.write('E');
                }
            }
            recv_pos = 0;
            
        } else if (c != '\r') {
            if (recv_pos < RECV_BUFFER_SIZE - 1) {
                recv_buffer[recv_pos++] = c;
            } else {
                recv_pos = 0;
            }
        }
    }
}
