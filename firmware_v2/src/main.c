#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hub75_config.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#if defined(__has_include)
#if __has_include("hardware/vreg.h")
#include "hardware/vreg.h"
#define HAVE_HARDWARE_VREG 1
#endif
#endif
#include "hub75.pio.h"
#include "tusb.h"

#include "cobs.h"

#ifndef __compiler_memory_barrier
#define __compiler_memory_barrier() __asm volatile("" ::: "memory")
#endif

#ifndef __dmb
#define __dmb() __compiler_memory_barrier()
#endif

#define LAT_MASK  (1u << PIN_LAT)
#define OE_MASK   (1u << PIN_OE)
#define ADDR_MASK ((1u << PIN_ADDR_A) | (1u << PIN_ADDR_B) | (1u << PIN_ADDR_C) | (1u << PIN_ADDR_D))

#define USB_BLOCK_COUNT 2u
#define USB_BLOCK_SIZE  2048u

#define PACKET_QUEUE_COUNT 3u
#define SCAN_BUFFER_COUNT 3u

#define PARSE_BUDGET_MIN_BYTES 512u
#define PARSE_BUDGET_MAX_BYTES 4096u
#define TUD_GAP_SHRINK_US      700u

#define DMA_WAIT_TIMEOUT_US 3000u

#ifndef MAX_FRAME_UPDATE_FPS
#define MAX_FRAME_UPDATE_FPS 180u
#endif

#define FRAME_UPDATE_PERIOD_US (1000000u / MAX_FRAME_UPDATE_FPS)

typedef struct {
    uint8_t data[USB_BLOCK_SIZE];
    volatile uint16_t len;
    volatile uint8_t ready;
} usb_block_t;

typedef struct {
    uint8_t data[RECV_BUFFER_SIZE];
    volatile uint16_t len;
    volatile uint8_t ready;
} packet_slot_t;

static PIO g_pio = pio0;
static uint g_sm_data = 0;
static int g_dma_chan = -1;

static uint8_t __attribute__((aligned(4))) g_bcm_planes[SCAN_BUFFER_COUNT][SCAN_ROWS][COLOR_DEPTH][DISPLAY_WIDTH];
static uint32_t __attribute__((aligned(4))) g_dma_buffer[2][DISPLAY_WIDTH];

static usb_block_t g_usb_blocks[USB_BLOCK_COUNT];
static uint8_t g_usb_prod_idx = 0;
static uint8_t g_usb_cons_idx = 0;
static uint16_t g_usb_cons_pos = 0;

static packet_slot_t g_packet_slots[PACKET_QUEUE_COUNT];
static uint8_t g_pkt_prod_idx = 0;
static volatile uint8_t g_pkt_cons_idx = 0;
static uint16_t g_parser_pos = 0;
static bool g_parser_discard = false;
static bool g_usb_resync = false;

static uint8_t __attribute__((aligned(4))) g_decode_buf[FRAME_SIZE_RGB565];

static uint8_t g_gamma_lut[256];
static uint8_t g_r5_to_cd[32];
static uint8_t g_g6_to_cd[64];
static uint8_t g_b5_to_cd[32];

static volatile uint8_t g_display_idx = 0;
static volatile uint8_t g_pending_idx = 0;
static volatile bool g_swap_pending = false;
static volatile bool g_dma_done = false;
static volatile uint32_t g_dma_wait_timeouts = 0;
static uint64_t g_next_swap_allowed_us = 0;

static volatile uint32_t g_rx_total_bytes = 0;
static volatile uint32_t g_decoded_frames = 0;
static volatile uint32_t g_displayed_frames = 0;
static volatile uint32_t g_scan_frames = 0;
static volatile uint32_t g_dropped_frames_core0 = 0;
static volatile uint32_t g_dropped_frames_core1 = 0;
static volatile uint32_t g_cobs_errors_core1 = 0;

static volatile uint32_t g_usb_drop_bytes = 0;
static volatile uint32_t g_drop_swap_pending = 0;
static volatile uint32_t g_drop_packet_overflow = 0;
static volatile uint32_t g_drop_packet_q_overrun = 0;

static volatile uint32_t g_tud_gap_us_max = 0;
static volatile uint32_t g_decode_us_max = 0;
static volatile uint32_t g_convert_us_max = 0;
static volatile uint32_t g_usb_block_highwater = 0;
static volatile uint32_t g_packet_queue_highwater = 0;
static uint32_t g_last_tud_us = 0;

static uint32_t g_stats_last_rx = 0;
static uint32_t g_stats_last_dec = 0;
static uint32_t g_stats_last_disp = 0;
static uint32_t g_stats_last_scan = 0;
static uint32_t g_stats_last_drop = 0;
static uint32_t g_stats_last_usb_drop = 0;
static uint32_t g_stats_last_dma_to = 0;
static uint64_t g_stats_last_us = 0;

_Static_assert(MAX_FRAME_UPDATE_FPS > 0, "MAX_FRAME_UPDATE_FPS must be > 0");
_Static_assert(USB_BLOCK_COUNT == 2u, "USB_BLOCK_COUNT must be 2 for SPSC");
_Static_assert(RECV_BUFFER_SIZE <= 65535u, "RECV_BUFFER_SIZE must fit packet slot length");

static inline uint32_t min_u32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static inline void update_max_u32(volatile uint32_t *current_max, uint32_t value) {
    if (value > *current_max) {
        *current_max = value;
    }
}

static uint32_t usb_pending_bytes(void) {
    uint32_t total = 0;

    for (uint32_t i = 0; i < USB_BLOCK_COUNT; ++i) {
        const usb_block_t *blk = &g_usb_blocks[i];
        if (!blk->ready) {
            continue;
        }

        if (i == g_usb_cons_idx) {
            if (blk->len > g_usb_cons_pos) {
                total += (uint32_t)(blk->len - g_usb_cons_pos);
            }
        } else {
            total += blk->len;
        }
    }

    return total;
}

static uint32_t packet_queue_depth(void) {
    uint32_t depth = 0;
    for (uint32_t i = 0; i < PACKET_QUEUE_COUNT; ++i) {
        if (g_packet_slots[i].ready) {
            depth++;
        }
    }
    return depth;
}

static inline uint32_t compute_parse_budget(void) {
    uint32_t pending = usb_pending_bytes();
    if (pending == 0) {
        return 0;
    }

    uint32_t budget = pending;
    if (budget > PARSE_BUDGET_MAX_BYTES) {
        budget = PARSE_BUDGET_MAX_BYTES;
    }
    if (budget < PARSE_BUDGET_MIN_BYTES) {
        budget = PARSE_BUDGET_MIN_BYTES;
    }

    if (g_tud_gap_us_max > TUD_GAP_SHRINK_US && budget > PARSE_BUDGET_MIN_BYTES) {
        budget >>= 1u;
        if (budget < PARSE_BUDGET_MIN_BYTES) {
            budget = PARSE_BUDGET_MIN_BYTES;
        }
    }

    return budget;
}

static inline void usb_publish_block(usb_block_t *blk) {
    if (blk->len == 0 || blk->ready) {
        return;
    }

    __dmb();
    blk->ready = 1u;
    update_max_u32(&g_usb_block_highwater, usb_pending_bytes());

    uint8_t next = g_usb_prod_idx ^ 1u;
    g_usb_prod_idx = next;
    if (!g_usb_blocks[next].ready) {
        g_usb_blocks[next].len = 0;
    }
}

static inline void packet_publish_slot(packet_slot_t *slot) {
    __dmb();
    slot->ready = 1u;
    update_max_u32(&g_packet_queue_highwater, packet_queue_depth());

    uint8_t next = (uint8_t)((g_pkt_prod_idx + 1u) % PACKET_QUEUE_COUNT);
    g_pkt_prod_idx = next;
    if (!g_packet_slots[next].ready) {
        g_packet_slots[next].len = 0;
    }
}

static inline void packet_release_slot(packet_slot_t *slot) {
    slot->len = 0;
    __dmb();
    slot->ready = 0u;
}

static bool stream_pop_usb_byte(uint8_t *out) {
    usb_block_t *blk = &g_usb_blocks[g_usb_cons_idx];

    if (!blk->ready) {
        return false;
    }

    if (g_usb_cons_pos == 0) {
        __dmb();
    }

    if (g_usb_cons_pos >= blk->len) {
        blk->len = 0;
        __dmb();
        blk->ready = 0u;
        g_usb_cons_pos = 0;
        g_usb_cons_idx ^= 1u;
        blk = &g_usb_blocks[g_usb_cons_idx];
        if (!blk->ready) {
            return false;
        }
        __dmb();
    }

    *out = blk->data[g_usb_cons_pos++];

    if (g_usb_cons_pos >= blk->len) {
        blk->len = 0;
        __dmb();
        blk->ready = 0u;
        g_usb_cons_pos = 0;
        g_usb_cons_idx ^= 1u;
    }

    return true;
}

static void sync_drop_until_delimiter(const uint8_t *buf, uint32_t *index, uint32_t size) {
    uint32_t start = *index;

    while (*index < size) {
        if (buf[*index] == 0x00u) {
            (*index)++;
            g_usb_resync = false;
            g_parser_pos = 0;
            g_parser_discard = false;
            break;
        }
        (*index)++;
    }

    g_usb_drop_bytes += (*index - start);
}

static void init_system_clock(void) {
#if CPU_CLOCK_KHZ > 200000u
#if defined(HAVE_HARDWARE_VREG)
#if defined(VREG_VOLTAGE_1_20)
    vreg_set_voltage(VREG_VOLTAGE_1_20);
#elif defined(VREG_VOLTAGE_1_15)
    vreg_set_voltage(VREG_VOLTAGE_1_15);
#endif
#endif
    sleep_ms(2);
#endif
    bool ok = set_sys_clock_khz((uint32_t)CPU_CLOCK_KHZ, true);
    if (!ok) {
        set_sys_clock_khz(133000u, true);
    }
}

static void init_gamma_and_color_luts(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t y = i * i;
        g_gamma_lut[i] = (uint8_t)((y + 255u) / 255u);
    }

    for (uint32_t i = 0; i < 32; ++i) {
        uint8_t v8 = (uint8_t)((i << 3) | (i >> 2));
        g_r5_to_cd[i] = (uint8_t)(g_gamma_lut[v8] >> (8 - COLOR_DEPTH));
        g_b5_to_cd[i] = (uint8_t)(g_gamma_lut[v8] >> (8 - COLOR_DEPTH));
    }

    for (uint32_t i = 0; i < 64; ++i) {
        uint8_t v8 = (uint8_t)((i << 2) | (i >> 4));
        g_g6_to_cd[i] = (uint8_t)(g_gamma_lut[v8] >> (8 - COLOR_DEPTH));
    }
}

void __not_in_flash_func(set_row_address)(uint32_t row) {
    sio_hw->gpio_clr = ADDR_MASK;
    uint32_t bits = (((row >> 0u) & 1u) << PIN_ADDR_A) |
                    (((row >> 1u) & 1u) << PIN_ADDR_B) |
                    (((row >> 2u) & 1u) << PIN_ADDR_C) |
                    (((row >> 3u) & 1u) << PIN_ADDR_D);
    sio_hw->gpio_set = bits;
}

void __not_in_flash_func(prepare_dma_buffer)(uint8_t plane_idx, uint32_t buf_idx, uint32_t row, uint32_t bit) {
    const uint8_t *src = g_bcm_planes[plane_idx][row][bit];
    uint32_t *dst = g_dma_buffer[buf_idx];

    for (uint32_t x = 0; x < DISPLAY_WIDTH; ++x) {
        dst[x] = src[DISPLAY_WIDTH - 1u - x];
    }
}

static void hub75_gpio_init(void) {
    for (uint32_t pin = PIN_R0; pin <= PIN_ADDR_D; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }
    gpio_put(PIN_OE, 1);
}

static void hub75_pio_init(void) {
    uint offset = pio_add_program(g_pio, &hub75_data_program);
    pio_sm_config c = hub75_data_program_get_default_config(offset);

    pio_sm_set_consecutive_pindirs(g_pio, g_sm_data, PIN_R0, 6, true);
    for (uint32_t pin = PIN_R0; pin < (PIN_R0 + 6u); ++pin) {
        pio_gpio_init(g_pio, pin);
    }

    pio_sm_set_consecutive_pindirs(g_pio, g_sm_data, PIN_CLK, 1, true);
    pio_gpio_init(g_pio, PIN_CLK);

    sm_config_set_out_pins(&c, PIN_R0, 6);
    sm_config_set_sideset_pins(&c, PIN_CLK);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, 1.0f);

    pio_sm_init(g_pio, g_sm_data, offset, &c);
    pio_sm_set_enabled(g_pio, g_sm_data, true);
}

static void hub75_dma_init(void) {
    g_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)g_dma_chan);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm_data, true));

    dma_channel_set_config((uint)g_dma_chan, &c, false);
    dma_channel_set_write_addr((uint)g_dma_chan, &g_pio->txf[g_sm_data], false);
    dma_channel_set_irq0_enabled((uint)g_dma_chan, true);
}

void __not_in_flash_func(dma_irq0_handler)(void) {
    uint32_t mask = 1u << (uint32_t)g_dma_chan;
    if (dma_hw->ints0 & mask) {
        dma_hw->ints0 = mask;
        g_dma_done = true;
    }
}

void __not_in_flash_func(hub75_refresh_once)(void) {
    uint8_t display_idx = g_display_idx;
    uint32_t buf_idx = 0;

    for (uint32_t bit = 0; bit < COLOR_DEPTH; ++bit) {
        uint32_t delay_us = 1u << bit;

        for (uint32_t row = 0; row < SCAN_ROWS; ++row) {
            sio_hw->gpio_set = OE_MASK;

            prepare_dma_buffer(display_idx, buf_idx, row, bit);
            dma_channel_set_read_addr((uint)g_dma_chan, g_dma_buffer[buf_idx], false);
            g_dma_done = false;
            dma_channel_set_trans_count((uint)g_dma_chan, DISPLAY_WIDTH, true);

            uint32_t next_buf = 1u - buf_idx;
            uint32_t next_row = row + 1u;
            uint32_t next_bit = bit;
            if (next_row >= SCAN_ROWS) {
                next_row = 0;
                next_bit = bit + 1u;
            }
            if (next_bit < COLOR_DEPTH) {
                prepare_dma_buffer(display_idx, next_buf, next_row, next_bit);
            }

            uint32_t wait_start = time_us_32();
            while (!g_dma_done) {
                __wfe();
                if ((uint32_t)(time_us_32() - wait_start) > DMA_WAIT_TIMEOUT_US) {
                    if (!dma_channel_is_busy((uint)g_dma_chan)) {
                        g_dma_done = true;
                    } else {
                        g_dma_wait_timeouts++;
                        dma_channel_abort((uint)g_dma_chan);
                        dma_hw->ints0 = 1u << (uint32_t)g_dma_chan;
                        g_dma_done = true;
                    }
                }
            }

            while (!pio_sm_is_tx_fifo_empty(g_pio, g_sm_data)) {
                tight_loop_contents();
            }
            __asm volatile("nop\nnop\nnop\nnop");

            set_row_address(row);
            sio_hw->gpio_set = LAT_MASK;
            __asm volatile("nop\nnop\nnop\nnop");
            sio_hw->gpio_clr = LAT_MASK;

            sio_hw->gpio_clr = OE_MASK;
            busy_wait_us_32(delay_us);
            sio_hw->gpio_set = OE_MASK;

            buf_idx = next_buf;
        }
    }

    g_scan_frames++;

    if (g_swap_pending) {
        uint64_t now = time_us_64();
        if (now >= g_next_swap_allowed_us) {
            __dmb();
            g_display_idx = g_pending_idx;
            g_swap_pending = false;
            g_displayed_frames++;
            g_next_swap_allowed_us = now + FRAME_UPDATE_PERIOD_US;
        }
    }
}

void __not_in_flash_func(convert_rgb565_to_bcm)(const uint8_t *src, uint8_t plane_idx) {
    for (uint32_t row = 0; row < SCAN_ROWS; ++row) {
        uint32_t y_upper = row;
        uint32_t y_lower = row + SCAN_ROWS;

        const uint8_t *up = src + (y_upper * DISPLAY_WIDTH * 2u);
        const uint8_t *lo = src + (y_lower * DISPLAY_WIDTH * 2u);

        uint8_t *plane0 = g_bcm_planes[plane_idx][row][0];
        uint8_t *plane1 = g_bcm_planes[plane_idx][row][1];
        uint8_t *plane2 = g_bcm_planes[plane_idx][row][2];
        uint8_t *plane3 = g_bcm_planes[plane_idx][row][3];
        uint8_t *plane4 = g_bcm_planes[plane_idx][row][4];
        uint8_t *plane5 = g_bcm_planes[plane_idx][row][5];

        for (uint32_t x = 0; x < DISPLAY_WIDTH; ++x) {
            uint16_t p_up = (uint16_t)up[0] | ((uint16_t)up[1] << 8u);
            uint16_t p_lo = (uint16_t)lo[0] | ((uint16_t)lo[1] << 8u);
            up += 2;
            lo += 2;

            uint8_t r0 = g_r5_to_cd[(p_up >> 11u) & 0x1Fu];
            uint8_t g0 = g_g6_to_cd[(p_up >> 5u) & 0x3Fu];
            uint8_t b0 = g_b5_to_cd[p_up & 0x1Fu];
            uint8_t r1 = g_r5_to_cd[(p_lo >> 11u) & 0x1Fu];
            uint8_t g1 = g_g6_to_cd[(p_lo >> 5u) & 0x3Fu];
            uint8_t b1 = g_b5_to_cd[p_lo & 0x1Fu];

            plane0[x] = (uint8_t)(((r0 >> 0u) & 1u) |
                                  (((g0 >> 0u) & 1u) << 1u) |
                                  (((b0 >> 0u) & 1u) << 2u) |
                                  (((r1 >> 0u) & 1u) << 3u) |
                                  (((g1 >> 0u) & 1u) << 4u) |
                                  (((b1 >> 0u) & 1u) << 5u));

            plane1[x] = (uint8_t)(((r0 >> 1u) & 1u) |
                                  (((g0 >> 1u) & 1u) << 1u) |
                                  (((b0 >> 1u) & 1u) << 2u) |
                                  (((r1 >> 1u) & 1u) << 3u) |
                                  (((g1 >> 1u) & 1u) << 4u) |
                                  (((b1 >> 1u) & 1u) << 5u));

            plane2[x] = (uint8_t)(((r0 >> 2u) & 1u) |
                                  (((g0 >> 2u) & 1u) << 1u) |
                                  (((b0 >> 2u) & 1u) << 2u) |
                                  (((r1 >> 2u) & 1u) << 3u) |
                                  (((g1 >> 2u) & 1u) << 4u) |
                                  (((b1 >> 2u) & 1u) << 5u));

            plane3[x] = (uint8_t)(((r0 >> 3u) & 1u) |
                                  (((g0 >> 3u) & 1u) << 1u) |
                                  (((b0 >> 3u) & 1u) << 2u) |
                                  (((r1 >> 3u) & 1u) << 3u) |
                                  (((g1 >> 3u) & 1u) << 4u) |
                                  (((b1 >> 3u) & 1u) << 5u));

            plane4[x] = (uint8_t)(((r0 >> 4u) & 1u) |
                                  (((g0 >> 4u) & 1u) << 1u) |
                                  (((b0 >> 4u) & 1u) << 2u) |
                                  (((r1 >> 4u) & 1u) << 3u) |
                                  (((g1 >> 4u) & 1u) << 4u) |
                                  (((b1 >> 4u) & 1u) << 5u));

            plane5[x] = (uint8_t)(((r0 >> 5u) & 1u) |
                                  (((g0 >> 5u) & 1u) << 1u) |
                                  (((b0 >> 5u) & 1u) << 2u) |
                                  (((r1 >> 5u) & 1u) << 3u) |
                                  (((g1 >> 5u) & 1u) << 4u) |
                                  (((b1 >> 5u) & 1u) << 5u));
        }
    }
}

static uint8_t select_build_buffer(void) {
    bool used[SCAN_BUFFER_COUNT] = {false, false, false};

    __dmb();
    used[g_display_idx] = true;
    if (g_swap_pending) {
        used[g_pending_idx] = true;
    }

    for (uint8_t i = 0; i < SCAN_BUFFER_COUNT; ++i) {
        if (!used[i]) {
            return i;
        }
    }

    return (uint8_t)((g_display_idx + 1u) % SCAN_BUFFER_COUNT);
}

static void core1_process_one_packet(void) {
    packet_slot_t *slot = &g_packet_slots[g_pkt_cons_idx];
    if (!slot->ready) {
        return;
    }

    __dmb();
    uint16_t encoded_len = slot->len;

    if (encoded_len == 0 || encoded_len > RECV_BUFFER_SIZE) {
        g_dropped_frames_core1++;
        g_cobs_errors_core1++;
        packet_release_slot(slot);
        g_pkt_cons_idx = (uint8_t)((g_pkt_cons_idx + 1u) % PACKET_QUEUE_COUNT);
        return;
    }

    uint32_t t_decode = time_us_32();
    size_t decoded_len = cobs_decode(slot->data, encoded_len, g_decode_buf, FRAME_SIZE_RGB565);
    uint32_t decode_us = (uint32_t)(time_us_32() - t_decode);
    update_max_u32(&g_decode_us_max, decode_us);

    packet_release_slot(slot);
    g_pkt_cons_idx = (uint8_t)((g_pkt_cons_idx + 1u) % PACKET_QUEUE_COUNT);

    if (decoded_len != FRAME_SIZE_RGB565) {
        g_dropped_frames_core1++;
        g_cobs_errors_core1++;
        return;
    }

    if (g_swap_pending) {
        g_dropped_frames_core1++;
        g_drop_swap_pending++;
        return;
    }

    uint8_t build_idx = select_build_buffer();
    uint32_t t_convert = time_us_32();
    convert_rgb565_to_bcm(g_decode_buf, build_idx);
    uint32_t convert_us = (uint32_t)(time_us_32() - t_convert);
    update_max_u32(&g_convert_us_max, convert_us);

    __dmb();
    g_pending_idx = build_idx;
    g_swap_pending = true;
    g_decoded_frames++;
}

static void ingest_usb_blocks(void) {
    static uint8_t trash[256];

    while (tud_cdc_available()) {
        uint32_t avail = tud_cdc_available();
        if (avail == 0) {
            break;
        }

        if (g_usb_resync) {
            uint32_t req = min_u32(avail, (uint32_t)sizeof(trash));
            uint32_t n = tud_cdc_read(trash, req);
            if (n == 0) {
                break;
            }
            g_rx_total_bytes += n;

            uint32_t idx = 0;
            sync_drop_until_delimiter(trash, &idx, n);
            continue;
        }

        usb_block_t *blk = &g_usb_blocks[g_usb_prod_idx];
        if (blk->ready) {
            uint32_t req = min_u32(avail, (uint32_t)sizeof(trash));
            uint32_t n = tud_cdc_read(trash, req);
            if (n == 0) {
                break;
            }
            g_rx_total_bytes += n;

            g_usb_resync = true;
            g_dropped_frames_core0++;
            g_drop_packet_q_overrun++;

            for (uint32_t i = 0; i < USB_BLOCK_COUNT; ++i) {
                g_usb_blocks[i].len = 0;
                g_usb_blocks[i].ready = 0u;
            }
            g_usb_prod_idx = 0;
            g_usb_cons_idx = 0;
            g_usb_cons_pos = 0;
            g_parser_pos = 0;
            g_parser_discard = false;

            uint32_t idx = 0;
            sync_drop_until_delimiter(trash, &idx, n);
            continue;
        }

        uint32_t space = (uint32_t)USB_BLOCK_SIZE - blk->len;
        if (space == 0) {
            usb_publish_block(blk);
            continue;
        }

        uint32_t req = min_u32(avail, space);
        uint32_t n = tud_cdc_read(&blk->data[blk->len], req);
        if (n == 0) {
            break;
        }

        g_rx_total_bytes += n;
        blk->len = (uint16_t)(blk->len + n);

        if (blk->len == USB_BLOCK_SIZE) {
            usb_publish_block(blk);
        }
    }

    if (!tud_cdc_available()) {
        usb_publish_block(&g_usb_blocks[g_usb_prod_idx]);
    }
}

static void parse_usb_stream_to_packets(void) {
    uint32_t budget = compute_parse_budget();
    uint32_t processed = 0;
    uint8_t b = 0;

    while (processed < budget && stream_pop_usb_byte(&b)) {
        processed++;

        if (b == 0x00u) {
            if (!g_parser_discard && g_parser_pos > 0) {
                packet_slot_t *slot = &g_packet_slots[g_pkt_prod_idx];
                if (slot->ready) {
                    g_dropped_frames_core0++;
                    g_drop_packet_q_overrun++;
                } else {
                    slot->len = g_parser_pos;
                    packet_publish_slot(slot);
                }
            }

            g_parser_pos = 0;
            g_parser_discard = false;
            continue;
        }

        if (g_parser_discard) {
            continue;
        }

        packet_slot_t *slot = &g_packet_slots[g_pkt_prod_idx];
        if (slot->ready) {
            g_parser_discard = true;
            g_parser_pos = 0;
            g_dropped_frames_core0++;
            g_drop_packet_q_overrun++;
            continue;
        }

        if (g_parser_pos < RECV_BUFFER_SIZE) {
            slot->data[g_parser_pos++] = b;
        } else {
            g_parser_discard = true;
            g_parser_pos = 0;
            g_dropped_frames_core0++;
            g_drop_packet_overflow++;
        }
    }
}

static void emit_stats(void) {
    uint64_t now = time_us_64();
    if ((now - g_stats_last_us) < STATS_INTERVAL_US) {
        return;
    }
    g_stats_last_us = now;

    uint32_t rx = g_rx_total_bytes;
    uint32_t dec = g_decoded_frames;
    uint32_t disp = g_displayed_frames;
    uint32_t scan = g_scan_frames;
    uint32_t drop = g_dropped_frames_core0 + g_dropped_frames_core1;
    uint32_t cobs = g_cobs_errors_core1;
    uint32_t usb_drop = g_usb_drop_bytes;
    uint32_t dma_to = g_dma_wait_timeouts;

    uint32_t rx_bps = rx - g_stats_last_rx;
    uint32_t usb_drop_bps = usb_drop - g_stats_last_usb_drop;
    uint32_t dec_fps = dec - g_stats_last_dec;
    uint32_t disp_fps = disp - g_stats_last_disp;
    uint32_t scan_fps = scan - g_stats_last_scan;
    uint32_t drop_ps = drop - g_stats_last_drop;
    uint32_t dma_to_ps = dma_to - g_stats_last_dma_to;

    uint32_t tud_gap_us_max = g_tud_gap_us_max;
    uint32_t decode_us_max = g_decode_us_max;
    uint32_t convert_us_max = g_convert_us_max;
    uint32_t usb_hw = g_usb_block_highwater;
    uint32_t pkt_hw = g_packet_queue_highwater;

    g_stats_last_rx = rx;
    g_stats_last_dec = dec;
    g_stats_last_disp = disp;
    g_stats_last_scan = scan;
    g_stats_last_drop = drop;
    g_stats_last_usb_drop = usb_drop;
    g_stats_last_dma_to = dma_to;

    g_tud_gap_us_max = 0;
    g_decode_us_max = 0;
    g_convert_us_max = 0;
    g_usb_block_highwater = usb_pending_bytes();
    g_packet_queue_highwater = packet_queue_depth();

    if (tud_cdc_n_connected(0) && tud_cdc_n_write_available(0) > 320) {
        char line[384];
        int n = snprintf(line, sizeof(line),
                         "STAT clk_khz=%lu rx_Bps=%lu usb_drop_Bps=%lu dec_fps=%lu disp_fps=%lu scan_fps=%lu drop_ps=%lu drop=%lu swap_drop=%lu ovf_drop=%lu qovf_drop=%lu cobs=%lu usb_hw=%lu pkt_hw=%lu tud_gap_us=%lu dec_us=%lu conv_us=%lu dma_to_ps=%lu dma_to=%lu\r\n",
                         (unsigned long)(clock_get_hz(clk_sys) / 1000u),
                         (unsigned long)rx_bps,
                         (unsigned long)usb_drop_bps,
                         (unsigned long)dec_fps,
                         (unsigned long)disp_fps,
                         (unsigned long)scan_fps,
                         (unsigned long)drop_ps,
                         (unsigned long)drop,
                         (unsigned long)g_drop_swap_pending,
                         (unsigned long)g_drop_packet_overflow,
                         (unsigned long)g_drop_packet_q_overrun,
                         (unsigned long)cobs,
                         (unsigned long)usb_hw,
                         (unsigned long)pkt_hw,
                         (unsigned long)tud_gap_us_max,
                         (unsigned long)decode_us_max,
                         (unsigned long)convert_us_max,
                         (unsigned long)dma_to_ps,
                         (unsigned long)dma_to);
        if (n > 0) {
            tud_cdc_n_write(0, line, (uint32_t)n);
            tud_cdc_n_write_flush(0);
        }
    }
}

static inline void service_usb(void) {
    uint32_t now = time_us_32();
    if (g_last_tud_us != 0) {
        update_max_u32(&g_tud_gap_us_max, (uint32_t)(now - g_last_tud_us));
    }
    g_last_tud_us = now;

    tud_task();
    ingest_usb_blocks();
}

static void core1_entry(void) {
    hub75_gpio_init();
    hub75_pio_init();
    hub75_dma_init();

    g_next_swap_allowed_us = time_us_64();

    dma_hw->ints0 = 1u << (uint32_t)g_dma_chan;
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    while (true) {
        hub75_refresh_once();
        core1_process_one_packet();
    }
}

int main(void) {
    init_system_clock();

    memset(g_bcm_planes, 0, sizeof(g_bcm_planes));
    memset(g_dma_buffer, 0, sizeof(g_dma_buffer));
    memset(g_usb_blocks, 0, sizeof(g_usb_blocks));
    memset(g_packet_slots, 0, sizeof(g_packet_slots));
    memset(g_decode_buf, 0, sizeof(g_decode_buf));

    init_gamma_and_color_luts();

    multicore_launch_core1(core1_entry);

    tusb_init();
    g_stats_last_us = time_us_64();

    while (true) {
        service_usb();
        parse_usb_stream_to_packets();

        if (usb_pending_bytes() > USB_BLOCK_SIZE) {
            service_usb();
            parse_usb_stream_to_packets();
        }

        emit_stats();
        tight_loop_contents();
    }
}
