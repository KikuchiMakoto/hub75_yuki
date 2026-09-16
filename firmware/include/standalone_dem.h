#ifndef STANDALONE_DEM_H
#define STANDALONE_DEM_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <hardware/adc.h>
#include <pico/platform.h>
#include "adxl335_config.h"
#include "turbo_lut.h"

// Number of silica sand particles (optimized for solid 30-40 FPS in SRAM)
#define DEM_N 768

// 64x64 container dimensions in pixels
#define BOX_WIDTH  64.0f
#define BOX_HEIGHT 64.0f

// Particle radius = 0.72 pixels (diameter = 1.44 pixels, zero gaps on 64x64)
#define PARTICLE_R    0.72f
#define PARTICLE_DIAM 1.44f
#define PARTICLE_DIAM2 (PARTICLE_DIAM * PARTICLE_DIAM)

// Spatial Grid: 32x32 cells (each cell is 2.0x2.0 pixels >= DIAM)
#define GRID_DIM 32
#define CELL_SIZE 2.0f
#define INV_CELL_SIZE 0.5f

// Particle state arrays (pure float / f32)
static float dem_pos_x[DEM_N];
static float dem_pos_y[DEM_N];
static float dem_vel_x[DEM_N];
static float dem_vel_y[DEM_N];
static float dem_force_x[DEM_N];
static float dem_force_y[DEM_N];
static float dem_contact_force[DEM_N];

// Spatial grid linked-cell arrays
static uint16_t dem_grid_head[GRID_DIM * GRID_DIM];
static uint16_t dem_grid_next[DEM_N];

// Intermediate 64x64 frame buffer
static uint16_t dem_raw_frame[64][64];

// Physics parameters (calibrated in f32)
static const float dem_dt = 0.010f;        // 3 substeps per frame = 30-35 FPS physics
static const float dem_kn = 320.0f;        // contact spring stiffness
static const float dem_damp = 4.0f;        // dashpot damping
static const float dem_friction = 0.45f;   // Coulomb friction
static const float dem_max_a = 500.0f;     // max acceleration clamp (pixels/s^2)
static const float dem_max_v = 110.0f;     // max velocity clamp (pixels/s)

static inline void dem_init(void) {
    // Initialize ADC for ADXL335 on GP26 (X) and GP27 (Y)
    adc_init();
    adc_gpio_init(ADXL335_PIN_X);
    adc_gpio_init(ADXL335_PIN_Y);

    // Seed particle positions in bottom half of 64x64 box
    int cols = 32;
    float spacing_x = 56.0f / (float)cols;
    float spacing_y = PARTICLE_DIAM * 0.95f;

    for (int i = 0; i < DEM_N; i++) {
        int col = i % cols;
        int row = i / cols;
        float x = 4.0f + (float)col * spacing_x + ((row % 2) ? (spacing_x * 0.5f) : 0.0f);
        float y = 60.0f - (float)row * spacing_y;
        if (y < 4.0f) y = 4.0f;
        dem_pos_x[i] = x;
        dem_pos_y[i] = y;
        dem_vel_x[i] = 0.0f;
        dem_vel_y[i] = 0.0f;
        dem_force_x[i] = 0.0f;
        dem_force_y[i] = 0.0f;
        dem_contact_force[i] = 0.0f;
    }
}

// Critical hot-path function placed directly in RAM for 20x execution speedup
static void __not_in_flash_func(dem_substep)(float gx, float gy) {
    // 1. Reset forces & apply gravity
    for (int i = 0; i < DEM_N; i++) {
        dem_force_x[i] = gx;
        dem_force_y[i] = gy;
        dem_contact_force[i] = 0.0f;
    }

    // 2. Build spatial grid (O(N) linked-cell)
    memset(dem_grid_head, 0xFF, sizeof(dem_grid_head));
    for (int i = 0; i < DEM_N; i++) {
        int cx = (int)(dem_pos_x[i] * INV_CELL_SIZE);
        int cy = (int)(dem_pos_y[i] * INV_CELL_SIZE);
        if (cx < 0) cx = 0; else if (cx >= GRID_DIM) cx = GRID_DIM - 1;
        if (cy < 0) cy = 0; else if (cy >= GRID_DIM) cy = GRID_DIM - 1;
        int cell = cy * GRID_DIM + cx;
        dem_grid_next[i] = dem_grid_head[cell];
        dem_grid_head[cell] = (uint16_t)i;
    }

    // 3. Fast neighbor collision checks via 32x32 grid
    for (int i = 0; i < DEM_N; i++) {
        int cx = (int)(dem_pos_x[i] * INV_CELL_SIZE);
        int cy = (int)(dem_pos_y[i] * INV_CELL_SIZE);
        if (cx < 0) cx = 0; else if (cx >= GRID_DIM) cx = GRID_DIM - 1;
        if (cy < 0) cy = 0; else if (cy >= GRID_DIM) cy = GRID_DIM - 1;

        float px = dem_pos_x[i];
        float py = dem_pos_y[i];
        float vx = dem_vel_x[i];
        float vy = dem_vel_y[i];

        float fx = 0.0f;
        float fy = 0.0f;
        float f_accum = 0.0f;

        for (int dy = -1; dy <= 1; dy++) {
            int ncy = cy + dy;
            if (ncy < 0 || ncy >= GRID_DIM) continue;
            for (int dx = -1; dx <= 1; dx++) {
                int ncx = cx + dx;
                if (ncx < 0 || ncx >= GRID_DIM) continue;

                int cell = ncy * GRID_DIM + ncx;
                for (int j = dem_grid_head[cell]; j != 0xFFFF; j = dem_grid_next[j]) {
                    if (j <= i) continue;

                    float diff_x = px - dem_pos_x[j];
                    float diff_y = py - dem_pos_y[j];
                    if (diff_x >= PARTICLE_DIAM || diff_x <= -PARTICLE_DIAM ||
                        diff_y >= PARTICLE_DIAM || diff_y <= -PARTICLE_DIAM) continue;

                    float d2 = diff_x * diff_x + diff_y * diff_y;
                    if (d2 > 0.0001f && d2 < PARTICLE_DIAM2) {
                        float dist = sqrtf(d2);
                        if (dist > 0.0001f) {
                            float overlap = PARTICLE_DIAM - dist;
                            float inv_d = 1.0f / dist;
                            float ndir_x = diff_x * inv_d;
                            float ndir_y = diff_y * inv_d;

                            float vrel_x = vx - dem_vel_x[j];
                            float vrel_y = vy - dem_vel_y[j];
                            float vn = vrel_x * ndir_x + vrel_y * ndir_y;

                            float fn = dem_kn * overlap - dem_damp * vn;
                            if (fn < 0.0f) fn = 0.0f;
                            else if (fn > 250.0f) fn = 250.0f;

                            // Fast Tangential Coulomb friction (zero sqrt, zero div)
                            float vt_x = vrel_x - vn * ndir_x;
                            float vt_y = vrel_y - vn * ndir_y;
                            float ft_max = dem_friction * fn;
                            float ft_x = -vt_x * 12.0f;
                            float ft_y = -vt_y * 12.0f;
                            if (ft_x > ft_max) ft_x = ft_max; else if (ft_x < -ft_max) ft_x = -ft_max;
                            if (ft_y > ft_max) ft_y = ft_max; else if (ft_y < -ft_max) ft_y = -ft_max;

                            float cfx = fn * ndir_x + ft_x;
                            float cfy = fn * ndir_y + ft_y;

                            fx += cfx;
                            fy += cfy;
                            dem_force_x[j] -= cfx;
                            dem_force_y[j] -= cfy;
                            f_accum += fn;
                            dem_contact_force[j] += fn;
                        }
                    }
                }
            }
        }
        dem_force_x[i] += fx;
        dem_force_y[i] += fy;
        dem_contact_force[i] += f_accum;
    }

    // 4. Container 4 walls (0..64 pixels)
    float min_coord = PARTICLE_R;
    float max_coord = BOX_WIDTH - PARTICLE_R;

    for (int i = 0; i < DEM_N; i++) {
        // Left wall
        if (dem_pos_x[i] < min_coord) {
            float ov = min_coord - dem_pos_x[i];
            float fn = dem_kn * ov + dem_damp * (dem_vel_x[i] < 0.0f ? -dem_vel_x[i] : 0.0f);
            if (fn > 400.0f) fn = 400.0f;
            dem_force_x[i] += fn;
            dem_contact_force[i] += fn;
        }
        // Right wall
        else if (dem_pos_x[i] > max_coord) {
            float ov = dem_pos_x[i] - max_coord;
            float fn = dem_kn * ov + dem_damp * (dem_vel_x[i] > 0.0f ? dem_vel_x[i] : 0.0f);
            if (fn > 400.0f) fn = 400.0f;
            dem_force_x[i] -= fn;
            dem_contact_force[i] += fn;
        }

        // Top wall
        if (dem_pos_y[i] < min_coord) {
            float ov = min_coord - dem_pos_y[i];
            float fn = dem_kn * ov + dem_damp * (dem_vel_y[i] < 0.0f ? -dem_vel_y[i] : 0.0f);
            if (fn > 400.0f) fn = 400.0f;
            dem_force_y[i] += fn;
            dem_contact_force[i] += fn;
        }
        // Bottom wall
        else if (dem_pos_y[i] > max_coord) {
            float ov = dem_pos_y[i] - max_coord;
            float fn = dem_kn * ov + dem_damp * (dem_vel_y[i] > 0.0f ? dem_vel_y[i] : 0.0f);
            if (fn > 400.0f) fn = 400.0f;
            dem_force_y[i] -= fn;
            dem_contact_force[i] += fn;
        }

        // 5. Symplectic Euler integration with acceleration & velocity clamp
        float fx = dem_force_x[i];
        float fy = dem_force_y[i];
        if (fx > dem_max_a) fx = dem_max_a; else if (fx < -dem_max_a) fx = -dem_max_a;
        if (fy > dem_max_a) fy = dem_max_a; else if (fy < -dem_max_a) fy = -dem_max_a;

        dem_vel_x[i] += fx * dem_dt;
        dem_vel_y[i] += fy * dem_dt;

        if (dem_vel_x[i] > dem_max_v) dem_vel_x[i] = dem_max_v;
        else if (dem_vel_x[i] < -dem_max_v) dem_vel_x[i] = -dem_max_v;
        if (dem_vel_y[i] > dem_max_v) dem_vel_y[i] = dem_max_v;
        else if (dem_vel_y[i] < -dem_max_v) dem_vel_y[i] = -dem_max_v;

        dem_pos_x[i] += dem_vel_x[i] * dem_dt;
        dem_pos_y[i] += dem_vel_y[i] * dem_dt;

        // Impenetrable emergency containment (strictly prevents leaving box)
        float hard_min = PARTICLE_R * 0.25f;
        float hard_max = BOX_WIDTH - hard_min;
        if (dem_pos_x[i] < hard_min) { dem_pos_x[i] = hard_min; if (dem_vel_x[i] < 0.0f) dem_vel_x[i] = -0.2f * dem_vel_x[i]; }
        else if (dem_pos_x[i] > hard_max) { dem_pos_x[i] = hard_max; if (dem_vel_x[i] > 0.0f) dem_vel_x[i] = -0.2f * dem_vel_x[i]; }
        if (dem_pos_y[i] < hard_min) { dem_pos_y[i] = hard_min; if (dem_vel_y[i] < 0.0f) dem_vel_y[i] = -0.2f * dem_vel_y[i]; }
        else if (dem_pos_y[i] > hard_max) { dem_pos_y[i] = hard_max; if (dem_vel_y[i] > 0.0f) dem_vel_y[i] = -0.2f * dem_vel_y[i]; }
    }
}

static void __not_in_flash_func(dem_step)(void) {
    // Read ADXL335 on GP26 (ADC0) and GP27 (ADC1)
    adc_select_input(ADXL335_ADC_CH_X);
    uint16_t raw_x = adc_read();
    adc_select_input(ADXL335_ADC_CH_Y);
    uint16_t raw_y = adc_read();

    float delta_x = (float)((int)raw_x - ADXL335_ZERO_G_COUNT);
    float delta_y = (float)((int)raw_y - ADXL335_ZERO_G_COUNT);

    float gx = (delta_x / (float)ADXL335_COUNTS_PER_G) * 250.0f;
    float gy = (delta_y / (float)ADXL335_COUNTS_PER_G) * 250.0f;

    // Run 3 substeps per display frame for blazing 30-35 FPS throughput
    for (int s = 0; s < 3; s++) {
        dem_substep(gx, gy);
    }
}

static void __not_in_flash_func(dem_render)(uint16_t out_frame[64][64]) {
    // 1. Clear raw frame to dark background
    memset(dem_raw_frame, 0, sizeof(dem_raw_frame));

    // 2. Splat particles with Turbo stress color
    for (int i = 0; i < DEM_N; i++) {
        int px = (int)dem_pos_x[i];
        int py = (int)dem_pos_y[i];

        int lut_idx = (int)(dem_contact_force[i] * 1.8f);
        if (lut_idx > 255) lut_idx = 255;
        uint16_t color = turbo_rgb565_lut[lut_idx];

        if (px >= 0 && px < 64 && py >= 0 && py < 64) {
            dem_raw_frame[py][px] = color;
            if (px + 1 < 64) dem_raw_frame[py][px + 1] = color;
            if (py + 1 < 64) dem_raw_frame[py + 1][px] = color;
        }
    }

    // 3. Fast 1-pass hole-fill filter
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint16_t c = dem_raw_frame[y][x];
            if (c == 0) {
                int lit = 0;
                uint16_t sample_c = 0;
                if (x > 0 && dem_raw_frame[y][x - 1]) { lit++; sample_c = dem_raw_frame[y][x - 1]; }
                if (x < 63 && dem_raw_frame[y][x + 1]) { lit++; sample_c = dem_raw_frame[y][x + 1]; }
                if (y > 0 && dem_raw_frame[y - 1][x]) { lit++; sample_c = dem_raw_frame[y - 1][x]; }
                if (y < 63 && dem_raw_frame[y + 1][x]) { lit++; sample_c = dem_raw_frame[y + 1][x]; }
                if (lit >= 2) {
                    out_frame[y][x] = sample_c;
                } else {
                    out_frame[y][x] = 0x0841; // subtle unlit LED gray
                }
            } else {
                out_frame[y][x] = c;
            }
        }
    }
}

#endif // STANDALONE_DEM_H
