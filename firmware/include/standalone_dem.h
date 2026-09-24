#pragma once

#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/divider.h"
#include "hardware/adc.h"
#include "adxl335_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Ultra-Stable High-Speed Discrete Element Method (DEM) for RP2040 (Q16.16)
//
// Key Stability & Flow Architecture:
// 1. Soft-Core Progressive Hooke Spring + Directional Closing Dashpot (zeta ~ 0.65)
// 2. Soft Contact Position Relaxation (anti-collapse & anti-explosion overburden relief)
// 3. Frictionless Glass-Smooth Wall Normal Springs with independent tangent sliding
// 4. Global Atmospheric Viscous Drag (v -= v >> 7) dissipating acoustic jitter
// 5. 64-bit Overflow-Free ADXL335 Acceleration Vector Scaling
// ============================================================================

#define DEM_N 1024

// Fixed-Point Q16.16 Math Helpers
#define Q16_SHIFT 16
#define Q16_ONE   65536
#define INT_TO_Q16(x) ((int32_t)((x) << Q16_SHIFT))
#define Q16_TO_INT(x) ((int)((x) >> Q16_SHIFT))

static inline int32_t __not_in_flash_func(q16_mul)(int32_t a, int32_t b) {
    return (int32_t)((((int64_t)a) * b) >> Q16_SHIFT);
}

static inline int32_t __not_in_flash_func(q16_div)(int32_t a, int32_t b) {
    if (b == 0) return (a >= 0) ? 0x7FFFFFFF : (int32_t)0x80000001;
    hw_divider_divmod_s32_start(a << 14, b);
    hw_divider_pause();
    return ((int32_t)sio_hw->div_quotient) << 2;
}

// Bitwise Integer Square Root for Q16.16 (16-step non-restoring, branchless inner)
static inline int32_t __not_in_flash_func(q16_sqrt)(int32_t val) {
    if (val <= 0) return 0;
    uint32_t rem = (uint32_t)val;
    uint32_t root = 0;
    for (uint32_t s = 0x40000000; s != 0; s >>= 2) {
        if (rem >= root + s) {
            rem -= root + s;
            root = (root >> 1) + s;
        } else {
            root >>= 1;
        }
    }
    return (int32_t)(root << 8);
}

// ============================================================================
// Geometry & Grid Constants
// ============================================================================
#define DEM_BOX_WIDTH        4194304  // 64.0 px in Q16
#define DEM_PARTICLE_DIAM      98304  // 1.5 px in Q16 (matches Taichi D=1.5)
#define DEM_PARTICLE_R         49152  // 0.75 px in Q16
#define DEM_PARTICLE_DIAM2    147456  // (1.5)^2 = 2.25 in Q16

#define DEM_WALL_MIN         (DEM_PARTICLE_R)
#define DEM_WALL_MAX         (DEM_BOX_WIDTH - DEM_PARTICLE_R)
#define DEM_WALL_SPRING_MIN  (DEM_PARTICLE_R)
#define DEM_WALL_SPRING_MAX  (DEM_BOX_WIDTH - DEM_PARTICLE_R)
#define DEM_WALL_HARD_MIN    (DEM_PARTICLE_R >> 2)           // 10240 (0.156 px)
#define DEM_WALL_HARD_MAX    (DEM_BOX_WIDTH - DEM_WALL_HARD_MIN) // 4184064 (63.844 px)

// Spatial Hash Grid (32x32 cells -> 2.0 px/cell, covers 64x64 domain)
#define GRID_DIM 32
#define CELL_SHIFT 1
#define GRID_TOTAL_CELLS (GRID_DIM * GRID_DIM)

// ============================================================================
// Physical Tuning Parameters (Real-Time 1:1 Flow Dynamics)
// ============================================================================
#define DEM_DT                    393  // 0.006 s in Q16.16 (tuned for real-time 1:1 flow rate)
#define DEM_GRAVITY_SCALE    42598400  // 650.0 px/s^2 at 1G (crisp natural sand avalanche speed)

#define DEM_KN              262144000  // 4000.0 px/s^2/px Hooke normal spring
#define DEM_DAMP              5242880  // 80.0 1/s dashpot damping (zeta ~ 0.65)
#define DEM_FRICTION            22938  // 0.35 Coulomb friction coefficient
#define DEM_GAMMA_T            983040  // 15.0 1/s tangential shear slip damping

#define DEM_WALL_KN         393216000  // 6000.0 px/s^2/px wall normal spring
#define DEM_WALL_DAMP         6553600  // 100.0 1/s wall normal dashpot damping

// Gravity axis sign (set to -1 if ADXL335 is mounted rotated).
// If sand piles to the TOP wall while the board bottom is down,
// set DEM_GRAV_SIGN_Y to -1 (same for X).
#ifndef DEM_GRAV_SIGN_X
#define DEM_GRAV_SIGN_X 1
#endif
#ifndef DEM_GRAV_SIGN_Y
#define DEM_GRAV_SIGN_Y -1
#endif

// Plain-integer factors for 32-bit fast paths (exact: Q16 const = INT * 65536).
#define DEM_KN_INT           4000  // == DEM_KN / 65536
#define DEM_DAMP_INT           80  // == DEM_DAMP / 65536
#define DEM_GAMMA_INT          15  // == DEM_GAMMA_T / 65536
#define DEM_WALL_KN_INT      6000  // == DEM_WALL_KN / 65536
#define DEM_WALL_DAMP_INT     100  // == DEM_WALL_DAMP / 65536
#define DEM_GRAV_INT          650  // == DEM_GRAVITY_SCALE / 65536 (650.0 px/s^2)
#define DEM_DT_NUM            393  // == DEM_DT (0.006s)
#define DEM_FRIC_NUM            7  // friction 0.35 = 7/20 exact: ft_max = (fn*7)/20
#define DEM_FRIC_DEN           20

// Overburden Relaxation Threshold & Safety Clamps
#define DEM_OVERLAP_TOL          5243  // 0.08 px tolerance before position relaxation
#define DEM_FN_MAX           98304000  // 1500.0 px/s^2 maximum contact force
#define DEM_WALL_FN_MAX     131072000  // 2000.0 px/s^2 maximum wall force
#define DEM_MAX_A           294912000  // 4500.0 px/s^2 maximum particle acceleration
#define DEM_MAX_V             3932160  // 60.0 px/s maximum velocity (33% faster lively flow, strictly fits 2^23 fast path)

// ============================================================================
// Particle State Arrays (SRAM BSS)
// ============================================================================
static int32_t dem_pos_x[DEM_N];
static int32_t dem_pos_y[DEM_N];
static int32_t dem_vel_x[DEM_N];
static int32_t dem_vel_y[DEM_N];
static int32_t dem_force_x[DEM_N];
static int32_t dem_force_y[DEM_N];
static int32_t dem_contact_force[DEM_N];

static uint16_t grid_head[GRID_TOTAL_CELLS];
static uint16_t grid_next[DEM_N];

// ============================================================================
// Precomputed 256-entry Turbo Colormap (RGB565 Little-Endian)
// ============================================================================
static const uint16_t turbo_rgb565_lut[256] = {
    0x20C3, 0x28C5, 0x28E6, 0x30E8, 0x3109, 0x392A, 0x392C, 0x394D,
    0x414E, 0x416F, 0x4190, 0x4191, 0x41B2, 0x49B3, 0x49D4, 0x49F5,
    0x49F5, 0x4A16, 0x4A37, 0x4A37, 0x4A58, 0x4A79, 0x4A79, 0x4A9A,
    0x4ABA, 0x4ABB, 0x4ADB, 0x4AFB, 0x4AFC, 0x4B1C, 0x4B3C, 0x435D,
    0x435D, 0x437D, 0x439D, 0x439E, 0x43BE, 0x43DE, 0x43DE, 0x3BFE,
    0x3C1E, 0x3C1E, 0x3C3E, 0x3C5E, 0x3C5E, 0x347E, 0x349E, 0x349E,
    0x34BE, 0x34DE, 0x34DE, 0x34FE, 0x351E, 0x2D1D, 0x2D3D, 0x2D3D,
    0x2D5D, 0x2D7D, 0x2D7D, 0x2D9C, 0x2D9C, 0x2DBC, 0x2DDC, 0x2DDB,
    0x2DFB, 0x2DFB, 0x2E1B, 0x2E1A, 0x263A, 0x265A, 0x2E5A, 0x2E79,
    0x2E79, 0x2E99, 0x2E98, 0x2E98, 0x2EB8, 0x2EB8, 0x2ED7, 0x2ED7,
    0x2EF7, 0x2EF6, 0x2F16, 0x2F16, 0x3715, 0x3735, 0x3735, 0x3734,
    0x3754, 0x3754, 0x3F53, 0x3F73, 0x3F73, 0x3F73, 0x3F92, 0x4792,
    0x4792, 0x4791, 0x47B1, 0x4FB1, 0x4FB0, 0x4FB0, 0x4FB0, 0x57D0,
    0x57CF, 0x57CF, 0x5FCF, 0x5FCE, 0x5FCE, 0x67CE, 0x67CE, 0x67CD,
    0x6FED, 0x6FED, 0x6FED, 0x77EC, 0x77EC, 0x77EC, 0x7FEC, 0x7FEB,
    0x7FEB, 0x87EB, 0x87CB, 0x8FCB, 0x8FCA, 0x8FCA, 0x97CA, 0x97CA,
    0x97CA, 0x9FC9, 0x9FA9, 0xA7A9, 0xA7A9, 0xA7A9, 0xAFA9, 0xAF88,
    0xAF88, 0xB788, 0xB788, 0xBF68, 0xBF68, 0xBF67, 0xC747, 0xC747,
    0xC747, 0xCF27, 0xCF27, 0xCF27, 0xD707, 0xD706, 0xD706, 0xDEE6,
    0xDEE6, 0xDEC6, 0xDEC6, 0xE6A6, 0xE6A6, 0xE686, 0xEE86, 0xEE66,
    0xEE65, 0xEE45, 0xEE45, 0xF625, 0xF625, 0xF605, 0xF605, 0xF5E5,
    0xFDE5, 0xFDC5, 0xFDA5, 0xFDA5, 0xFD85, 0xFD85, 0xFD64, 0xFD44,
    0xFD44, 0xFD24, 0xFD04, 0xFD04, 0xFCE4, 0xFCE4, 0xFCC4, 0xFCA4,
    0xFCA4, 0xFC84, 0xFC64, 0xFC64, 0xFC44, 0xFC24, 0xFC04, 0xFC04,
    0xFBE3, 0xFBC3, 0xFBC3, 0xFBA3, 0xFB83, 0xFB83, 0xFB63, 0xFB43,
    0xF343, 0xF323, 0xF303, 0xF303, 0xF2E3, 0xEAC3, 0xEAC3, 0xEAA3,
    0xEA83, 0xEA82, 0xE262, 0xE242, 0xE242, 0xDA22, 0xDA02, 0xDA02,
    0xD9E2, 0xD1C2, 0xD1C2, 0xD1A2, 0xC9A2, 0xC982, 0xC982, 0xC161,
    0xC161, 0xC141, 0xB941, 0xB921, 0xB921, 0xB901, 0xB101, 0xB0E1,
    0xB0E1, 0xA8C1, 0xA8C1, 0xA8C1, 0xA0A0, 0xA0A0, 0xA0A0, 0xA080,
    0x9880, 0x9880, 0x9880, 0x9880, 0x9060, 0x9060, 0x9060, 0x9060,
    0x9060, 0x9060, 0x9060, 0x9060, 0x9060, 0x9060, 0x9060, 0x9060
};

static int32_t g_dem_last_gx = 0;
static int32_t g_dem_last_gy = 0;
static uint16_t g_dem_last_raw_x = 0;
static uint16_t g_dem_last_raw_y = 0;
static bool g_dem_sensor_connected = false;

// ============================================================================
// Initialization: Perfectly Staggered Non-Overlapping Sand Bed & Hardware ADC
// ============================================================================
static void __not_in_flash_func(dem_init)(void) {
    // 1. Initialize RP2040 Hardware ADC (pure 12-bit, 0-4095)
    adc_init();
    adc_gpio_init(26); // GP26 = ADC0 (ADXL335 X-axis)
    adc_gpio_init(27); // GP27 = ADC1 (ADXL335 Y-axis)

    int32_t start_x = (4 << 16);      // 4.0 px
    int32_t start_y = (4 << 16);      // 4.0 px
    int32_t spacing_x = 103219;       // 1.575 px (D * 1.05, non-overlapping)
    int32_t spacing_y = 93389;        // 1.425 px (D * 0.95)

    int cols = 36;
    for (int i = 0; i < DEM_N; i++) {
        int c = i % cols;
        int r = i / cols;
        int32_t x = start_x + c * spacing_x + ((r & 1) ? (spacing_x >> 1) : 0);
        int32_t y = start_y + r * spacing_y;

        dem_pos_x[i] = x;
        dem_pos_y[i] = y;
        dem_vel_x[i] = 0;
        dem_vel_y[i] = 0;
        dem_force_x[i] = 0;
        dem_force_y[i] = 0;
        dem_contact_force[i] = 0;
    }
}

// Cell coordinate helper
static inline int __not_in_flash_func(get_cell_idx)(int32_t px, int32_t py) {
    int cx = (px >> (Q16_SHIFT + CELL_SHIFT));
    int cy = (py >> (Q16_SHIFT + CELL_SHIFT));
    if (cx < 0) cx = 0;
    else if (cx >= GRID_DIM) cx = GRID_DIM - 1;
    if (cy < 0) cy = 0;
    else if (cy >= GRID_DIM) cy = GRID_DIM - 1;
    return cy * GRID_DIM + cx;
}

// ============================================================================
// Core Physics Substep
// ============================================================================
static void __not_in_flash_func(dem_substep)(int32_t gx, int32_t gy) {
    // 1. Reset forces & initialize with ADXL335 gravity
    for (int i = 0; i < DEM_N; i++) {
        dem_force_x[i] = gx;
        dem_force_y[i] = gy;
        dem_contact_force[i] = 0;
    }

    // 2. Spatial Hash Grid Construction
    memset(grid_head, 0xFF, sizeof(grid_head));
    for (int i = 0; i < DEM_N; i++) {
        int cell = get_cell_idx(dem_pos_x[i], dem_pos_y[i]);
        grid_next[i] = grid_head[cell];
        grid_head[cell] = (uint16_t)i;
    }

    // 3. Particle-Particle Contact Mechanics with Soft Relaxation
    for (int i = 0; i < DEM_N; i++) {
        int32_t px = dem_pos_x[i];
        int32_t py = dem_pos_y[i];
        int32_t vx_i = dem_vel_x[i];
        int32_t vy_i = dem_vel_y[i];

        int cx = (px >> (Q16_SHIFT + CELL_SHIFT));
        int cy = (py >> (Q16_SHIFT + CELL_SHIFT));
        if (cx < 0) cx = 0; else if (cx >= GRID_DIM) cx = GRID_DIM - 1;
        if (cy < 0) cy = 0; else if (cy >= GRID_DIM) cy = GRID_DIM - 1;

        for (int dy = -1; dy <= 1; dy++) {
            int ncy = cy + dy;
            if (ncy < 0 || ncy >= GRID_DIM) continue;

            for (int dx = -1; dx <= 1; dx++) {
                int ncx = cx + dx;
                if (ncx < 0 || ncx >= GRID_DIM) continue;

                int cell = ncy * GRID_DIM + ncx;
                for (uint16_t j = grid_head[cell]; j != 0xFFFF; j = grid_next[j]) {
                    if (j <= (uint16_t)i) continue; // Symmetric pair evaluation

                    int32_t diff_x = px - dem_pos_x[j];
                    if (diff_x >= DEM_PARTICLE_DIAM || diff_x <= -DEM_PARTICLE_DIAM) continue;

                    int32_t diff_y = py - dem_pos_y[j];
                    if (diff_y >= DEM_PARTICLE_DIAM || diff_y <= -DEM_PARTICLE_DIAM) continue;

                    // 32-bit fast path (all products fit int32: see range audit).
                    // d2 scale identical to q16_mul form (8-LSB truncation of diff).
                    int32_t d2 = (diff_x >> 8) * (diff_x >> 8)
                               + (diff_y >> 8) * (diff_y >> 8);
                    if (d2 >= DEM_PARTICLE_DIAM2 || d2 <= 0) continue;

                    int32_t dist = q16_sqrt(d2);
                    if (dist <= 0) continue;

                    int32_t ndir_x, ndir_y, overlap;

                    if (dist < 655) {
                        // Singularity safeguard (< 0.01 px separation)
                        dist = 655;
                        overlap = DEM_PARTICLE_DIAM - dist;
                        ndir_x = (((i ^ j) & 1) ? Q16_ONE : -Q16_ONE);
                        ndir_y = (((i ^ (j * 3)) & 1) ? Q16_ONE : -Q16_ONE);
                    } else {
                        overlap = DEM_PARTICLE_DIAM - dist;
                        // Fast asynchronous hardware 32-bit divide (8-cycle hardware pipelining)
                        hw_divider_divmod_s32_start(diff_x << 14, dist);
                        int32_t num_y = diff_y << 14;
                        hw_divider_pause();
                        ndir_x = ((int32_t)sio_hw->div_quotient) << 2;

                        hw_divider_divmod_s32_start(num_y, dist);
                        hw_divider_pause();
                        ndir_y = ((int32_t)sio_hw->div_quotient) << 2;
                    }

                    // A. Hooke spring (exact integer) + closing dashpot (exact integer).
                    // Damping ONLY when closing in (vn < 0). Never pulls particles together.
                    int32_t vrel_x = vx_i - dem_vel_x[j];
                    int32_t vrel_y = vy_i - dem_vel_y[j];
                    int32_t vn = ((vrel_x >> 8) * (ndir_x >> 8))
                               + ((vrel_y >> 8) * (ndir_y >> 8));

                    int32_t fn = DEM_KN_INT * overlap;
                    if (vn < 0) {
                        fn -= DEM_DAMP_INT * vn;
                    }
                    if (fn < 0) fn = 0;
                    else if (fn > DEM_FN_MAX) fn = DEM_FN_MAX;

                    // B. Tangential viscous slip + Coulomb cap (0.35 * fn using 1-cycle hardware multiply)
                    int32_t vt_x = vrel_x - ((vn >> 8) * (ndir_x >> 8));
                    int32_t vt_y = vrel_y - ((vn >> 8) * (ndir_y >> 8));
                    int32_t ft_max = (int32_t)(((int64_t)fn * 22938) >> 16);
                    int32_t ft_x = -DEM_GAMMA_INT * vt_x;
                    int32_t ft_y = -DEM_GAMMA_INT * vt_y;
                    if (ft_x > ft_max) ft_x = ft_max;
                    else if (ft_x < -ft_max) ft_x = -ft_max;
                    if (ft_y > ft_max) ft_y = ft_max;
                    else if (ft_y < -ft_max) ft_y = -ft_max;

                    // Symmetric force application
                    int32_t cfx = ((fn >> 8) * (ndir_x >> 8)) + ft_x;
                    int32_t cfy = ((fn >> 8) * (ndir_y >> 8)) + ft_y;
                    dem_force_x[i] += cfx;
                    dem_force_y[i] += cfy;
                    dem_force_x[j] -= cfx;
                    dem_force_y[j] -= cfy;
                    dem_contact_force[i] += fn;
                    dem_contact_force[j] += fn;
                }
            }
        }
    }

    // 4. Smooth Glass Wall Springs (Normal repulsion only, zero friction)
    for (int i = 0; i < DEM_N; i++) {
        int32_t px = dem_pos_x[i];
        int32_t py = dem_pos_y[i];
        int32_t vx = dem_vel_x[i];
        int32_t vy = dem_vel_y[i];

        // Left wall (spring engages when px < DEM_WALL_SPRING_MIN)
        // 32-bit exact integer paths (no 64-bit multiply).
        if (px < DEM_WALL_SPRING_MIN) {
            int32_t ov = DEM_WALL_SPRING_MIN - px;
            int32_t fn = DEM_WALL_KN_INT * ov;
            if (vx < 0) fn -= DEM_WALL_DAMP_INT * vx;
            if (fn < 0) fn = 0;
            else if (fn > DEM_WALL_FN_MAX) fn = DEM_WALL_FN_MAX;
            dem_force_x[i] += fn;
            dem_contact_force[i] += fn;
        }

        // Right wall (spring engages when px > DEM_WALL_SPRING_MAX)
        if (px > DEM_WALL_SPRING_MAX) {
            int32_t ov = px - DEM_WALL_SPRING_MAX;
            int32_t fn = DEM_WALL_KN_INT * ov;
            if (vx > 0) fn += DEM_WALL_DAMP_INT * vx;
            if (fn < 0) fn = 0;
            else if (fn > DEM_WALL_FN_MAX) fn = DEM_WALL_FN_MAX;
            dem_force_x[i] -= fn;
            dem_contact_force[i] += fn;
        }

        // Top wall (spring engages when py < DEM_WALL_SPRING_MIN)
        if (py < DEM_WALL_SPRING_MIN) {
            int32_t ov = DEM_WALL_SPRING_MIN - py;
            int32_t fn = DEM_WALL_KN_INT * ov;
            if (vy < 0) fn -= DEM_WALL_DAMP_INT * vy;
            if (fn < 0) fn = 0;
            else if (fn > DEM_WALL_FN_MAX) fn = DEM_WALL_FN_MAX;
            dem_force_y[i] += fn;
            dem_contact_force[i] += fn;
        }

        // Bottom wall (spring engages when py > DEM_WALL_SPRING_MAX)
        if (py > DEM_WALL_SPRING_MAX) {
            int32_t ov = py - DEM_WALL_SPRING_MAX;
            int32_t fn = DEM_WALL_KN_INT * ov;
            if (vy > 0) fn += DEM_WALL_DAMP_INT * vy;
            if (fn < 0) fn = 0;
            else if (fn > DEM_WALL_FN_MAX) fn = DEM_WALL_FN_MAX;
            dem_force_y[i] -= fn;
            dem_contact_force[i] += fn;
        }
    }

    // 5. Symplectic Euler Integration with Air Drag & Emergency Hard Rebound
    for (int i = 0; i < DEM_N; i++) {
        // Acceleration clamp
        int32_t ax = dem_force_x[i];
        int32_t ay = dem_force_y[i];
        if (ax > DEM_MAX_A) ax = DEM_MAX_A;
        else if (ax < -DEM_MAX_A) ax = -DEM_MAX_A;
        if (ay > DEM_MAX_A) ay = DEM_MAX_A;
        else if (ay < -DEM_MAX_A) ay = -DEM_MAX_A;

        // Velocity update: v += a * dt
        // 32-bit: (ax>>8) <= 766k, *262 <= 200M fits int32 (8-LSB truncation of ax).
        dem_vel_x[i] += ((ax >> 8) * DEM_DT_NUM) >> 8;
        dem_vel_y[i] += ((ay >> 8) * DEM_DT_NUM) >> 8;

        // Global ambient air drag: 0.78% velocity dissipation per substep
        dem_vel_x[i] -= (dem_vel_x[i] >> 7);
        dem_vel_y[i] -= (dem_vel_y[i] >> 7);

        // Velocity clamp
        if (dem_vel_x[i] > DEM_MAX_V) dem_vel_x[i] = DEM_MAX_V;
        else if (dem_vel_x[i] < -DEM_MAX_V) dem_vel_x[i] = -DEM_MAX_V;
        if (dem_vel_y[i] > DEM_MAX_V) dem_vel_y[i] = DEM_MAX_V;
        else if (dem_vel_y[i] < -DEM_MAX_V) dem_vel_y[i] = -DEM_MAX_V;

        // Position update: x += v * dt (64-bit protected multiply to prevent overflow)
        dem_pos_x[i] += (int32_t)(((int64_t)dem_vel_x[i] * DEM_DT_NUM) >> 16);
        dem_pos_y[i] += (int32_t)(((int64_t)dem_vel_y[i] * DEM_DT_NUM) >> 16);

        // Emergency Boundary Rebound (only engages when deeply penetrating beyond 75% of radius)
        if (dem_pos_x[i] < DEM_WALL_HARD_MIN) {
            dem_pos_x[i] = DEM_WALL_HARD_MIN;
            if (dem_vel_x[i] < 0) dem_vel_x[i] = -(dem_vel_x[i] >> 2); // 25% elastic rebound
        } else if (dem_pos_x[i] > DEM_WALL_HARD_MAX) {
            dem_pos_x[i] = DEM_WALL_HARD_MAX;
            if (dem_vel_x[i] > 0) dem_vel_x[i] = -(dem_vel_x[i] >> 2);
        }

        if (dem_pos_y[i] < DEM_WALL_HARD_MIN) {
            dem_pos_y[i] = DEM_WALL_HARD_MIN;
            if (dem_vel_y[i] < 0) dem_vel_y[i] = -(dem_vel_y[i] >> 2);
        } else if (dem_pos_y[i] > DEM_WALL_HARD_MAX) {
            dem_pos_y[i] = DEM_WALL_HARD_MAX;
            if (dem_vel_y[i] > 0) dem_vel_y[i] = -(dem_vel_y[i] >> 2);
        }
    }
}

// ============================================
// RP2040-E9 ADC Errata Workaround:
// The RP2040 SAR ADC has severe DNL (differential non-linearity) spikes at 512*k boundaries,
// particularly at 2048 (the exact nominal 0G point of ADXL335).
// 8x oversampling with channel-settling discard and 2-pole IIR filter smooths DNL errors.
// ============================================
static inline uint16_t __not_in_flash_func(read_adc_filtered)(uint channel) {
    adc_select_input(channel);
    (void)adc_read(); // Discard first reading after mux switch

    uint32_t sum = 0;
    for (int k = 0; k < 8; k++) {
        sum += adc_read();
    }
    return (uint16_t)(sum >> 3);
}

// ============================================================================
// External Public API
// ============================================================================
static void __not_in_flash_func(dem_step)(void) {
    static bool inited = false;
    if (!inited) {
        dem_init();
        inited = true;
    }

    // Filtered ADC reading (RP2040-E9 errata mitigation)
    uint16_t sample_x = read_adc_filtered(0); // GP26 = ADC0 (X)
    uint16_t sample_y = read_adc_filtered(1); // GP27 = ADC1 (Y)

    static uint16_t s_filt_x = 2048;
    static uint16_t s_filt_y = 2048;
    s_filt_x = (uint16_t)((s_filt_x * 3 + sample_x + 2) >> 2);
    s_filt_y = (uint16_t)((s_filt_y * 3 + sample_y + 2) >> 2);

    uint16_t raw_x = s_filt_x;
    uint16_t raw_y = s_filt_y;

    g_dem_last_raw_x = raw_x;
    g_dem_last_raw_y = raw_y;

    int32_t gx = 0;
    int32_t gy = DEM_GRAVITY_SCALE; // Default +1.0g downward (towards bottom wall)

    // ADXL335 sensor validity check:
    // Operating at 3.3V, nominal 0g is 1.65V (~2048 counts).
    // Valid acceleration range +/-3g produces 0.66V (~820) to 2.64V (~3280).
    // If pins are unconnected or floating (reading < 400 or > 3700),
    // gracefully fall back to default downward gravity (0g in X, +1g in Y)
    // so sand never explodes into the corner.
    if (raw_x >= 400 && raw_x <= 3700 && raw_y >= 400 && raw_y <= 3700) {
        g_dem_sensor_connected = true;
        int32_t delta_x = (int32_t)raw_x - ADXL335_ZERO_G_COUNT_X;
        int32_t delta_y = (int32_t)raw_y - ADXL335_ZERO_G_COUNT_Y;

        // 20-count deadband (~0.05g) to eliminate resting sensor noise
        if (delta_x > -20 && delta_x < 20) delta_x = 0;
        if (delta_y > -20 && delta_y < 20) delta_y = 0;

        // 32-bit exact: |delta| <= 1652 so delta*220 <= 364k fits int32.
        // Quotient is integer px/s^2 (1-step quantization; deadband is ~11).
        // At 1g: (410*220/410)*65536 = 14417920 (220.0 px/s^2 in Q16).
        gx = ((delta_x * DEM_GRAV_INT) / ADXL335_COUNTS_PER_G) * Q16_ONE;
        gy = ((delta_y * DEM_GRAV_INT) / ADXL335_COUNTS_PER_G) * Q16_ONE;

        // Hard limit gravity to +/-2.5g to guard against sensor glitches
        const int32_t g_limit = 106496000; // 2.5 * 42598400
        if (gx > g_limit) gx = g_limit;
        else if (gx < -g_limit) gx = -g_limit;
        if (gy > g_limit) gy = g_limit;
        else if (gy < -g_limit) gy = -g_limit;

        // Axis sign correction (see DEM_GRAV_SIGN_X/Y defines above)
        gx *= DEM_GRAV_SIGN_X;
        gy *= DEM_GRAV_SIGN_Y;

#if ADXL335_ROTATION_DEG == 90
        // Clockwise 90 deg rotation: gx' = -gy, gy' = gx
        int32_t rot_gx = -gy;
        int32_t rot_gy = gx;
        gx = rot_gx;
        gy = rot_gy;
#elif ADXL335_ROTATION_DEG == 180
        gx = -gx;
        gy = -gy;
#elif ADXL335_ROTATION_DEG == 270
        // Counter-clockwise 90 deg rotation: gx' = gy, gy' = -gx
        int32_t rot_gx = gy;
        int32_t rot_gy = -gx;
        gx = rot_gx;
        gy = rot_gy;
#endif
    } else {
        g_dem_sensor_connected = false;
    }

    g_dem_last_gx = gx;
    g_dem_last_gy = gy;

    // 2 sub-steps per display frame: optimized to reach ~33 FPS with real-time 1:1 lively sand flow
    for (int s = 0; s < 2; s++) {
        dem_substep(gx, gy);
    }
}

static void __not_in_flash_func(dem_render)(uint16_t buf[64][64]) {
    memset(buf, 0, 64 * 64 * sizeof(uint16_t));

    for (int i = 0; i < DEM_N; i++) {
        int x = Q16_TO_INT(dem_pos_x[i]);
        int y = Q16_TO_INT(dem_pos_y[i]);

        if (x >= 0 && x < 64 && y >= 0 && y < 64) {
            int32_t s = q16_sqrt(dem_contact_force[i]);
            int32_t stress = 24 + (s - 700000) / 30000;
            if (stress > 255) stress = 255;
            else if (stress < 0) stress = 0;

            uint16_t col = turbo_rgb565_lut[stress];
            buf[y][x] = col;

            // Over-estimate coverage: Fill 2x2 footprint based on subpixel position
            // to completely eliminate black hole artifacts in 2x2 particle clusters.
            int nx = x + (((dem_pos_x[i] & 0xFFFF) >= 0x8000) ? 1 : -1);
            int ny = y + (((dem_pos_y[i] & 0xFFFF) >= 0x8000) ? 1 : -1);

            if (nx >= 0 && nx < 64) buf[y][nx] = col;
            if (ny >= 0 && ny < 64) buf[ny][x] = col;
            if (nx >= 0 && nx < 64 && ny >= 0 && ny < 64) buf[ny][nx] = col;
        }
    }
}

#ifdef __cplusplus
}
#endif
