#ifndef HUB75_PIO_H
#define HUB75_PIO_H

#include "hardware/pio.h"

/* ======================================================================== */
/*  SM0: hub75_data_v3  (5 pixels / 32-bit word, 93.75% FIFO efficiency)   */
/* ======================================================================== */
#define hub75_data_v3_wrap_target 0
#define hub75_data_v3_wrap 9

static const uint16_t hub75_data_v3_program_instructions[] = {
    /* 0 */ 0x4306, /* out pins, 6 side 0 [3]  ; pixel 0, CLK=LOW */
    /* 1 */ 0xb322, /* nop side 1 [3]           ; CLK=HIGH  (mov y,y) */
    /* 2 */ 0x4306, /* out pins, 6 side 0 [3]  ; pixel 1 */
    /* 3 */ 0xb322, /* nop side 1 [3] */
    /* 4 */ 0x4306, /* out pins, 6 side 0 [3]  ; pixel 2 */
    /* 5 */ 0xb322, /* nop side 1 [3] */
    /* 6 */ 0x4306, /* out pins, 6 side 0 [3]  ; pixel 3 */
    /* 7 */ 0xb322, /* nop side 1 [3] */
    /* 8 */ 0x4306, /* out pins, 6 side 0 [3]  ; pixel 4 */
    /* 9 */ 0xb022, /* nop side 1               ; CLK=HIGH (mov y,y) */
};

static const struct pio_program hub75_data_v3_program = {
    .instructions = hub75_data_v3_program_instructions,
    .length = 10,
    .origin = -1,
};

static inline pio_sm_config hub75_data_v3_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + hub75_data_v3_wrap_target,
                            offset + hub75_data_v3_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}

/* ======================================================================== */
/*  SM1: hub75_latoe_v3  (LAT pulse + OE window + IRQ notification)        */
/* ======================================================================== */
#define hub75_latoe_v3_wrap_target 0
#define hub75_latoe_v3_wrap 7

static const uint16_t hub75_latoe_v3_program_instructions[] = {
    /* 0 */ 0x80a0, /* pull block side 0        ; OE duration from FIFO */
    /* 1 */ 0xa022, /* mov x, osr side 0        ; copy to X */
    /* 2 */ 0xb222, /* nop side 1 [2]           ; LAT=HIGH (mov y,y) */
    /* 3 */ 0xa022, /* nop side 0               ; LAT=LOW */
    /* 4 */ 0xe000, /* set pins, 0 side 0       ; OE=LOW (display ON) */
    /* 5 */ 0x2024, /* jmp x--, 4 side 0        ; wait X cycles */
    /* 6 */ 0xf001, /* set pins, 1 side 1       ; OE=HIGH (display OFF) */
    /* 7 */ 0xc000, /* irq set 0 side 0         ; notify Core1 */
};

static const struct pio_program hub75_latoe_v3_program = {
    .instructions = hub75_latoe_v3_program_instructions,
    .length = 8,
    .origin = -1,
};

static inline pio_sm_config hub75_latoe_v3_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + hub75_latoe_v3_wrap_target,
                            offset + hub75_latoe_v3_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}

/* ======================================================================== */
/*  SM2: hub75_addr_v3  (row address output, future expansion)             */
/* ======================================================================== */
#define hub75_addr_v3_wrap_target 0
#define hub75_addr_v3_wrap 0

static const uint16_t hub75_addr_v3_program_instructions[] = {
    /* 0 */ 0xa022, /* mov pins, x              ; output X to ADDR pins */
};

static const struct pio_program hub75_addr_v3_program = {
    .instructions = hub75_addr_v3_program_instructions,
    .length = 1,
    .origin = -1,
};

static inline pio_sm_config hub75_addr_v3_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + hub75_addr_v3_wrap_target,
                            offset + hub75_addr_v3_wrap);
    return c;
}

/* ======================================================================== */
/*  SM3: hub75_seq_v3  (sequencer, future expansion)                       */
/* ======================================================================== */
#define hub75_seq_v3_wrap_target 0
#define hub75_seq_v3_wrap 1

static const uint16_t hub75_seq_v3_program_instructions[] = {
    /* 0 */ 0x80a0, /* pull block               ; receive control word */
    /* 1 */ 0xa022, /* mov y, osr               ; copy to Y */
};

static const struct pio_program hub75_seq_v3_program = {
    .instructions = hub75_seq_v3_program_instructions,
    .length = 2,
    .origin = -1,
};

static inline pio_sm_config hub75_seq_v3_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + hub75_seq_v3_wrap_target,
                            offset + hub75_seq_v3_wrap);
    return c;
}

#endif
