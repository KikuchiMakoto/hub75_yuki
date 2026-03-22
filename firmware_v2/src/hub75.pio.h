#ifndef HUB75_PIO_H
#define HUB75_PIO_H

#include "hardware/pio.h"

#define hub75_data_wrap_target 0
#define hub75_data_wrap 2

static const uint16_t hub75_data_program_instructions[] = {
    0x80a0,
    0x6706,
    0x1700,
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

#endif
