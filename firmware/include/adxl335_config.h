#ifndef ADXL335_CONFIG_H
#define ADXL335_CONFIG_H

#include <stdint.h>

// RP2040 GPIO pin to ADC mapping for ADXL335 (3.3V)
// 2D DEM only requires X and Y acceleration axes (Z is unused)
// GP26 -> ADC0 (X-axis)
// GP27 -> ADC1 (Y-axis)
// 3V3  -> ADXL335 VCC
// GND  -> ADXL335 GND

#define ADXL335_PIN_X      26  // GP26, ADC0 (X-axis)
#define ADXL335_PIN_Y      27  // GP27, ADC1 (Y-axis)

#define ADXL335_ADC_CH_X   0   // ADC0 (GP26)
#define ADXL335_ADC_CH_Y   1   // ADC1 (GP27)

// ADXL335 Calibration Constants at 3.3V supply
// Nominal 0g voltage = VCC / 2 = 1.65V -> ADC count ~ 2048 (12-bit, 0-4095)
// Sensitivity = 330 mV/g -> 330mV / (3300mV / 4096) = 410 counts/g
#define ADXL335_COUNTS_PER_G  410

// ============================================
// 0G Voltage & Calibration Offset Tuning
// Adjust the 0g offset in raw ADC counts (+/- counts) or directly override count values.
// Formula: Voltage_V = Count * (3.3V / 4096)  =>  1 count ~ 0.806 mV
// ============================================
#ifndef ADXL335_ZERO_G_OFFSET_X
#define ADXL335_ZERO_G_OFFSET_X     0       // 0G calibration offset for X-axis (raw counts, e.g. +12, -25)
#endif

#ifndef ADXL335_ZERO_G_OFFSET_Y
#define ADXL335_ZERO_G_OFFSET_Y     0       // 0G calibration offset for Y-axis (raw counts, e.g. +18, -15)
#endif

#ifndef ADXL335_ZERO_G_COUNT_X
#define ADXL335_ZERO_G_COUNT_X      (2048 + (ADXL335_ZERO_G_OFFSET_X))
#endif

#ifndef ADXL335_ZERO_G_COUNT_Y
#define ADXL335_ZERO_G_COUNT_Y      (2048 + (ADXL335_ZERO_G_OFFSET_Y))
#endif

// Legacy fallback
#ifndef ADXL335_ZERO_G_COUNT
#define ADXL335_ZERO_G_COUNT        2048
#endif

// Fixed-point conversion helpers (Q8.8 or integer)
static inline int16_t adxl335_raw_to_mG(uint16_t raw_adc) {
    int32_t delta = (int32_t)raw_adc - ADXL335_ZERO_G_COUNT;
    // delta * 1000 / 410 = delta * 100 / 41
    return (int16_t)((delta * 100) / 41);
}

// Axis-specific conversion helpers
static inline int32_t adxl335_raw_x_to_q16_accel(uint16_t raw_adc) {
    int32_t delta = (int32_t)raw_adc - ADXL335_ZERO_G_COUNT_X;
    return delta * 1918;
}

static inline int32_t adxl335_raw_y_to_q16_accel(uint16_t raw_adc) {
    int32_t delta = (int32_t)raw_adc - ADXL335_ZERO_G_COUNT_Y;
    return delta * 1918;
}

// Direct 1-cycle conversion from 12-bit ADC raw count to Q16.16 acceleration (m/s^2 * 65536)
// Calibrated for ADXL335 (+/-3G range) at 3.3V supply (330mV/g, 410 counts/g) and g = 12.0 m/s^2:
// Scale multiplier K = (12.0 * 65536) / 410 = 1918
// Result is exact Q16.16 acceleration:
// - At 0G  (ADC ~ 2048): delta = 0     -> 0 m/s^2
// - At +1G (ADC ~ 2458): delta = +410  -> +786,380 (~12.00 m/s^2 in Q16)
// - At +3G (ADC ~ 3278): delta = +1230 -> +2,359,140 (~36.00 m/s^2 in Q16)
// - At -3G (ADC ~  818): delta = -1230 -> -2,359,140 (~ -36.00 m/s^2 in Q16)
// Single-cycle 32-bit integer multiplication on Cortex-M0+ (zero floating point operations)
static inline int32_t adxl335_raw_to_q16_accel(uint16_t raw_adc) {
    int32_t delta = (int32_t)raw_adc - ADXL335_ZERO_G_COUNT;
    return delta * 1918;
}

#endif // ADXL335_CONFIG_H
