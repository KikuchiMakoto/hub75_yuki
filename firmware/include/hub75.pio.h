/**
 * HUB75 PIO Program Header
 * 
 * Generated from hub75.pio
 * This header is compatible with both Pico SDK and Arduino (Earle Philhower core)
 */

#ifndef HUB75_PIO_H
#define HUB75_PIO_H

#include <hardware/pio.h>

// ============================================
// hub75_data program
// Shifts out 6-bit RGB data with clock side-set
// ============================================

#define hub75_data_wrap_target 0
#define hub75_data_wrap 2

static const uint16_t hub75_data_program_instructions[] = {
    //     .wrap_target
    0x80a0, //  0: pull   block           side 0        ; get 32-bit data from FIFO
    0x6706, //  1: out    pins, 6         side 0 [7]    ; output 6 bits, data setup time
    0x1700, //  2: jmp    0               side 1 [7]    ; CLK HIGH, hold for shift register
    //     .wrap
};

static const struct pio_program hub75_data_program = {
    .instructions = hub75_data_program_instructions,
    .length = 3,
    .origin = -1,
};

static inline pio_sm_config hub75_data_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + hub75_data_wrap_target, offset + hub75_data_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}

/**
 * Initialize the hub75_data PIO program
 * 
 * @param pio    PIO instance (pio0 or pio1)
 * @param sm     State machine number (0-3)
 * @param offset Program offset in instruction memory
 * @param rgb_base_pin First RGB data pin (6 consecutive pins)
 * @param clock_pin Clock pin (side-set)
 */
static inline void hub75_data_program_init(PIO pio, uint sm, uint offset,
                                            uint rgb_base_pin, uint clock_pin) {
    // Configure 6 consecutive pins for RGB data
    pio_sm_set_consecutive_pindirs(pio, sm, rgb_base_pin, 6, true);
    for (uint i = rgb_base_pin; i < rgb_base_pin + 6; ++i) {
        pio_gpio_init(pio, i);
    }
    
    // Configure clock pin for side-set
    pio_sm_set_consecutive_pindirs(pio, sm, clock_pin, 1, true);
    pio_gpio_init(pio, clock_pin);
    
    // Get default config
    pio_sm_config c = hub75_data_program_get_default_config(offset);
    
    // Set OUT pins (6 RGB data pins)
    sm_config_set_out_pins(&c, rgb_base_pin, 6);
    
    // Set side-set pin (clock)
    sm_config_set_sideset_pins(&c, clock_pin);
    
    // Shift right, autopull at 6 bits
    sm_config_set_out_shift(&c, true, true, 6);
    
    // Join FIFO for TX only
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    
    // Run at full speed
    sm_config_set_clkdiv(&c, 1.0f);
    
    // Initialize and enable
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

#endif // HUB75_PIO_H
