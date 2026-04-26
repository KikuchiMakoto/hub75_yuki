#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hub75_config.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/pio.h"
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

/* Validate configuration invariants */
_Static_assert(MAX_FRAME_UPDATE_FPS > 0, "MAX_FRAME_UPDATE_FPS must be > 0");
_Static_assert(USB_BLOCK_COUNT == 2u, "USB_BLOCK_COUNT must be 2 for SPSC");
_Static_assert(RECV_BUFFER_SIZE <= 65535u, "RECV_BUFFER_SIZE must fit packet slot length");
_Static_assert(COLOR_DEPTH == 6u, "COLOR_DEPTH must be 6 for current BCM conversion layout");
_Static_assert(COLOR_DEPTH >= 1u && COLOR_DEPTH <= 8u, "COLOR_DEPTH must be in range [1, 8]");
_Static_assert((DISPLAY_WIDTH % 4u) == 0u, "DISPLAY_WIDTH must be multiple of 4 for DMA 32-bit");

typedef struct {
    uint8_t data[USB_BLOCK_SIZE];
    uint16_t len;
    uint8_t ready;
} usb_block_t;

typedef struct {
    uint8_t data[RECV_BUFFER_SIZE];
    uint16_t len;
    volatile uint8_t ready;
} packet_slot_t;

/* ------------------------------------------------------------------------ */
/*  HUB75 / PIO / DMA globals                                               */
/* ------------------------------------------------------------------------ */
static PIO g_pio = pio0;
static uint g_sm_data = 0;
static int g_dma_chan = -1;
static uint g_pio_prog_offset = 0;

/* BCM frame buffers: aligned to 4 bytes for safe 32-bit DMA source access   */
static uint8_t __attribute__((aligned(4))) g_bcm_planes[SCAN_BUFFER_COUNT][SCAN_ROWS][COLOR_DEPTH][DISPLAY_WIDTH];

/* ------------------------------------------------------------------------ */
/*  USB stream / packet queue  (Core0 primary, Core1 consumes packets)      */
/* ------------------------------------------------------------------------ */
static usb_block_t g_usb_blocks[USB_BLOCK_COUNT];
static uint8_t g_usb_prod_idx = 0;
static uint8_t g_usb_cons_idx = 0;
static uint16_t g_usb_cons_pos = 0;

static packet_slot_t g_packet_slots[PACKET_QUEUE_COUNT];
static uint8_t g_pkt_prod_idx = 0;
static uint8_t g_pkt_cons_idx = 0;
static uint16_t g_parser_pos = 0;
static bool g_parser_discard = false;
static bool g_usb_resync = false;

static uint8_t __attribute__((aligned(4))) g_decode_buf[FRAME_SIZE_RGB565];

/* ------------------------------------------------------------------------ */
/*  Gamma / colour-depth LUTs                                               */
/* ------------------------------------------------------------------------ */
static uint8_t g_gamma_lut[256];
static uint8_t g_r5_to_cd[32];
static uint8_t g_g6_to_cd[64];
static uint8_t g_b5_to_cd[32];

/* ------------------------------------------------------------------------ */
/*  Frame-swap state (Core1 only writes, Core0 reads via atomics/volatile)  */
/* ------------------------------------------------------------------------ */
static uint8_t g_display_idx = 0;
static uint8_t g_pending_idx = 0;
static bool g_swap_pending = false;
static volatile bool g_dma_done = false;
static volatile uint32_t g_dma_wait_timeouts = 0;
static uint64_t g_next_swap_allowed_us = 0;

/* ------------------------------------------------------------------------ */
/*  Statistics – Core0 local                                                */
/* ------------------------------------------------------------------------ */
static uint32_t g_rx_total_bytes = 0;
static uint32_t g_dropped_frames_core0 = 0;
static uint32_t g_usb_drop_bytes = 0;
static uint32_t g_drop_packet_overflow = 0;
static uint32_t g_drop_packet_q_overrun = 0;
static uint32_t g_queue_full_on_delim = 0;
static uint32_t g_queue_full_mid_packet = 0;
static uint32_t g_usb_block_ready_overrun = 0;
static uint32_t g_tud_gap_us_max = 0;
static uint32_t g_usb_block_highwater = 0;
static uint32_t g_packet_queue_highwater = 0;
static uint32_t g_last_tud_us = 0;

/* ------------------------------------------------------------------------ */
/*  Statistics – Core1 local (written ONLY by Core1, read by Core0)         */
/*  No critical_section / lock required on Core1 because Core1 is           */
/*  single-threaded.  Core0 reads atomically thanks to 32-bit aligned       */
/*  volatile accesses on RP2040.                                            */
/* ------------------------------------------------------------------------ */
static volatile uint32_t g_core1_decoded_frames = 0;
static volatile uint32_t g_core1_displayed_frames = 0;
static volatile uint32_t g_core1_scan_frames = 0;
static volatile uint32_t g_core1_dropped_frames = 0;
static volatile uint32_t g_core1_cobs_errors = 0;
static volatile uint32_t g_core1_dma_timeouts = 0;
static volatile uint32_t g_core1_swap_replaced_frames = 0;
static volatile uint32_t g_core1_slot_corruption_events = 0;
static volatile uint32_t g_core1_ready_len_zero_events = 0;
static volatile uint32_t g_core1_packet_gap_us_max = 0;
static volatile uint32_t g_core1_decode_us_max = 0;
static volatile uint32_t g_core1_convert_us_max = 0;

/* ------------------------------------------------------------------------ */
/*  Statistics snapshot / diff state (Core0 only)                           */
/* ------------------------------------------------------------------------ */
static uint32_t g_stats_last_rx = 0;
static uint32_t g_stats_last_dec = 0;
static uint32_t g_stats_last_disp = 0;
static uint32_t g_stats_last_scan = 0;
static uint32_t g_stats_last_drop = 0;
static uint32_t g_stats_last_usb_drop = 0;
static uint32_t g_stats_last_dma_to = 0;
static uint64_t g_stats_last_us = 0;

/* ------------------------------------------------------------------------ */
/*  Helpers                                                                 */
/* ------------------------------------------------------------------------ */
static inline uint32_t min_u32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

/* Fast, accurate 5-bit to 8-bit expansion: (v * 255 + 15) / 31 */
static inline uint8_t expand5(uint8_t v) {
    return (uint8_t)(((uint16_t)v * 255u + 15u) / 31u);
}

/* Fast, accurate 6-bit to 8-bit expansion: (v * 255 + 31) / 63 */
static inline uint8_t expand6(uint8_t v) {
    return (uint8_t)(((uint16_t)v * 255u + 31u) / 63u);
}

/* Integer gamma approximation: gamma 2.2 using fixed-point table.
 * Pre-computed at init time; no runtime float. */
static inline uint8_t gamma8(uint8_t v) {
    /* Table generated as round(pow(v/255.0, 2.2) * 255.0) */
    static const uint8_t lut[256] = {
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   1,   1,   1,   1,
        1,   1,   1,   1,   1,   1,   1,   1,
        1,   2,   2,   2,   2,   2,   2,   2,
        2,   3,   3,   3,   3,   3,   3,   3,
        4,   4,   4,   4,   4,   5,   5,   5,
        5,   5,   6,   6,   6,   6,   6,   7,
        7,   7,   7,   8,   8,   8,   8,   9,
        9,   9,   9,   10,  10,  10,  11,  11,
        11,  11,  12,  12,  12,  13,  13,  13,
        14,  14,  14,  15,  15,  15,  16,  16,
        16,  17,  17,  17,  18,  18,  18,  19,
        19,  19,  20,  20,  21,  21,  21,  22,
        22,  22,  23,  23,  24,  24,  24,  25,
        25,  26,  26,  26,  27,  27,  28,  28,
        29,  29,  29,  30,  30,  31,  31,  32,
        32,  32,  33,  33,  34,  34,  35,  35,
        36,  36,  37,  37,  38,  38,  39,  39,
        40,  40,  41,  41,  42,  42,  43,  43,
        44,  44,  45,  45,  46,  46,  47,  47,
        48,  48,  49,  50,  50,  51,  51,  52,
        52,  53,  53,  54,  55,  55,  56,  56,
        57,  57,  58,  59,  59,  60,  60,  61,
        62,  62,  63,  63,  64,  65,  65,  66,
        66,  67,  68,  68,  69,  70,  70,  71,
        72,  72,  73,  73,  74,  75,  75,  76,
        77,  77,  78,  79,  79,  80,  81,  81,
        82,  83,  83,  84,  85,  85,  86,  87,
        87,  88,  89,  90,  90,  91,  92,  92,
        93,  94,  94,  95,  96,  97,  97,  98,
        99,  99,  100, 101, 102, 102, 103, 104,
        105, 105, 106, 107, 108, 108, 109, 110,
        111, 111, 112, 113, 114, 114, 115, 116,
        117, 117, 118, 119, 120, 121, 121, 122,
        123, 124, 125, 125, 126, 127, 128, 128,
        129, 130, 131, 132, 132, 133, 134, 135,
        136, 136, 137, 138, 139, 140, 140, 141,
        142, 143, 144, 145, 145, 146, 147, 148,
        149, 150, 150, 151, 152, 153, 154, 155,
        155, 156, 157, 158, 159, 160, 161, 161,
        162, 163, 164, 165, 166, 167, 167, 168,
        169, 170, 171, 172, 173, 174, 175, 175,
        176, 177, 178, 179, 180, 181, 182, 183,
        183, 184, 185, 186, 187, 188, 189, 190,
        191, 192, 193, 193, 194, 195, 196, 197,
        198, 199, 200, 201, 202, 203, 204, 205,
        205, 206, 207, 208, 209, 210, 211, 212,
        213, 214, 215, 216, 217, 218, 219, 220,
        221, 222, 223, 224, 225, 226, 227, 228,
        229, 230, 231, 232, 233, 234, 235, 236,
        237, 238, 239, 240, 241, 242, 243, 244,
        245, 246, 247, 248, 249, 250, 251, 252,
        253, 254, 255
    };
    return lut[v];
}

/* Core1 helpers – no locks, only Core1 touches these */
static inline void core1_inc(volatile uint32_t *ctr) {
    *ctr = *ctr + 1u;
}

static inline void core1_update_max(volatile uint32_t *mx, uint32_t v) {
    if (v > *mx) {
        *mx = v;
    }
}

static inline bool packet_slot_ready(packet_slot_t *slot) {
    if (!slot->ready) {
        return false;
    }
    __dmb();
    return true;
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
        if (packet_slot_ready(&g_packet_slots[i])) {
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

    blk->ready = 1u;
    if (usb_pending_bytes() > g_usb_block_highwater) {
        g_usb_block_highwater = usb_pending_bytes();
    }

    uint8_t next = g_usb_prod_idx ^ 1u;
    g_usb_prod_idx = next;
    if (!g_usb_blocks[next].ready) {
        g_usb_blocks[next].len = 0;
    }
}

static inline void packet_publish_slot(packet_slot_t *slot) {
    __dmb();
    slot->ready = 1u;
    if (packet_queue_depth() > g_packet_queue_highwater) {
        g_packet_queue_highwater = packet_queue_depth();
    }

    uint8_t next = (uint8_t)((g_pkt_prod_idx + 1u) % PACKET_QUEUE_COUNT);
    g_pkt_prod_idx = next;
    if (!packet_slot_ready(&g_packet_slots[next])) {
        g_packet_slots[next].len = 0;
    }
}

static inline void packet_release_slot(packet_slot_t *slot) {
    slot->len = 0;
    __dmb();
    slot->ready = 0u;
}

static inline bool stream_pop_usb_byte(uint8_t *out) {
    usb_block_t *blk = &g_usb_blocks[g_usb_cons_idx];

    if (!blk->ready) {
        return false;
    }

    if (g_usb_cons_pos >= blk->len) {
        blk->len = 0;
        blk->ready = 0u;
        g_usb_cons_pos = 0;
        g_usb_cons_idx ^= 1u;
        blk = &g_usb_blocks[g_usb_cons_idx];
        if (!blk->ready) {
            return false;
        }
    }

    *out = blk->data[g_usb_cons_pos++];

    if (g_usb_cons_pos >= blk->len) {
        blk->len = 0;
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
        g_gamma_lut[i] = gamma8((uint8_t)i);
    }

    for (uint32_t i = 0; i < 32; ++i) {
        uint8_t v8 = expand5((uint8_t)i);
        g_r5_to_cd[i] = (uint8_t)(g_gamma_lut[v8] >> (8 - COLOR_DEPTH));
        g_b5_to_cd[i] = (uint8_t)(g_gamma_lut[v8] >> (8 - COLOR_DEPTH));
    }

    for (uint32_t i = 0; i < 64; ++i) {
        uint8_t v8 = expand6((uint8_t)i);
        g_g6_to_cd[i] = (uint8_t)(g_gamma_lut[v8] >> (8 - COLOR_DEPTH));
    }
}

/* ------------------------------------------------------------------------ */
/*  HUB75 GPIO / PIO / DMA  (timing-critical, all in SRAM)                 */
/* ------------------------------------------------------------------------ */

void __not_in_flash_func(set_row_address)(uint32_t row) {
    sio_hw->gpio_clr = ADDR_MASK;
    uint32_t bits = (((row >> 0u) & 1u) << PIN_ADDR_A) |
                    (((row >> 1u) & 1u) << PIN_ADDR_B) |
                    (((row >> 2u) & 1u) << PIN_ADDR_C) |
                    (((row >> 3u) & 1u) << PIN_ADDR_D);
    sio_hw->gpio_set = bits;
}

/* Direct PIO register access for minimal latency */
static inline uint32_t __not_in_flash_func(pio_sm_pc)(PIO pio, uint sm) {
    return pio->sm[sm].addr;
}

static inline bool __not_in_flash_func(pio_sm_tx_fifo_empty_raw)(PIO pio, uint sm) {
    /* TX FIFO level is bits [3:0] for SM0, [11:8] for SM1, etc. */
    return (pio->flevel & (0xFu << (sm * 8u))) == 0u;
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
    g_pio_prog_offset = pio_add_program(g_pio, &hub75_data_program);
    pio_sm_config c = hub75_data_program_get_default_config(g_pio_prog_offset);

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

    pio_sm_init(g_pio, g_sm_data, g_pio_prog_offset, &c);
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

static bool __not_in_flash_func(wait_for_row_shift_complete)(void) {
    uint32_t wait_start = time_us_32();
    uint target_pc = g_pio_prog_offset + hub75_data_wrap_target;

    while (true) {
        if (pio_sm_tx_fifo_empty_raw(g_pio, g_sm_data) && pio_sm_pc(g_pio, g_sm_data) == target_pc) {
            return true;
        }

        if ((uint32_t)(time_us_32() - wait_start) > DMA_WAIT_TIMEOUT_US) {
            return false;
        }

        tight_loop_contents();
    }
}

static void __not_in_flash_func(recover_scan_pipeline)(void) {
    uint chan = (uint)g_dma_chan;
    uint32_t mask = 1u << (uint32_t)g_dma_chan;

    dma_channel_abort(chan);
    dma_hw->ints0 = mask;
    pio_sm_set_enabled(g_pio, g_sm_data, false);
    pio_sm_clear_fifos(g_pio, g_sm_data);
    pio_sm_restart(g_pio, g_sm_data);
    pio_sm_set_enabled(g_pio, g_sm_data, true);
    g_dma_done = true;
}

/* ------------------------------------------------------------------------ */
/*  Display refresh – direct DMA from g_bcm_planes, no CPU copy             */
/* ------------------------------------------------------------------------ */
void __not_in_flash_func(hub75_refresh_once)(void) {
    uint8_t display_idx = g_display_idx;
    uint32_t buf_idx = 0;

    for (uint32_t bit = 0; bit < COLOR_DEPTH; ++bit) {
        uint32_t delay_us = 1u << bit;

        for (uint32_t row = 0; row < SCAN_ROWS; ++row) {
            sio_hw->gpio_set = OE_MASK;

            /* Point DMA directly at the BCM row buffer (32-bit aligned) */
            uint8_t *src = g_bcm_planes[display_idx][row][bit];
            dma_channel_set_read_addr((uint)g_dma_chan, src, false);
            g_dma_done = false;
            dma_channel_set_trans_count((uint)g_dma_chan, DISPLAY_WIDTH / 4u, true);

            /* Pre-compute next DMA source while current one is shifting */
            uint32_t next_row = row + 1u;
            uint32_t next_bit = bit;
            if (next_row >= SCAN_ROWS) {
                next_row = 0;
                next_bit = bit + 1u;
            }

            uint32_t wait_start = time_us_32();
            bool row_transfer_ok = true;
            while (!g_dma_done) {
                __wfe();
                if ((uint32_t)(time_us_32() - wait_start) > DMA_WAIT_TIMEOUT_US) {
                    if (!dma_channel_is_busy((uint)g_dma_chan)) {
                        g_dma_done = true;
                    } else {
                        row_transfer_ok = false;
                        break;
                    }
                }
            }

            if (!row_transfer_ok || !wait_for_row_shift_complete()) {
                core1_inc(&g_core1_dma_timeouts);
                recover_scan_pipeline();
                continue;
            }

            __asm volatile("nop\nnop\nnop\nnop");

            set_row_address(row);
            sio_hw->gpio_set = LAT_MASK;
            __asm volatile("nop\nnop\nnop\nnop");
            sio_hw->gpio_clr = LAT_MASK;

            sio_hw->gpio_clr = OE_MASK;
            busy_wait_us_32(delay_us);
            sio_hw->gpio_set = OE_MASK;

            (void)next_row;
            (void)next_bit;
            (void)buf_idx;
        }
    }

    core1_inc(&g_core1_scan_frames);

    if (g_swap_pending) {
        uint64_t now = time_us_64();
        if (now >= g_next_swap_allowed_us) {
            g_display_idx = g_pending_idx;
            g_swap_pending = false;
            core1_inc(&g_core1_displayed_frames);
            g_next_swap_allowed_us = now + FRAME_UPDATE_PERIOD_US;
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  RGB565 -> BCM conversion (Core1)                                        */
/* ------------------------------------------------------------------------ */
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
            uint32_t dst_x = DISPLAY_WIDTH - 1u - x;
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

            plane0[dst_x] = (uint8_t)(((r0 >> 0u) & 1u) |
                                      (((g0 >> 0u) & 1u) << 1u) |
                                      (((b0 >> 0u) & 1u) << 2u) |
                                      (((r1 >> 0u) & 1u) << 3u) |
                                      (((g1 >> 0u) & 1u) << 4u) |
                                      (((b1 >> 0u) & 1u) << 5u));

            plane1[dst_x] = (uint8_t)(((r0 >> 1u) & 1u) |
                                      (((g0 >> 1u) & 1u) << 1u) |
                                      (((b0 >> 1u) & 1u) << 2u) |
                                      (((r1 >> 1u) & 1u) << 3u) |
                                      (((g1 >> 1u) & 1u) << 4u) |
                                      (((b1 >> 1u) & 1u) << 5u));

            plane2[dst_x] = (uint8_t)(((r0 >> 2u) & 1u) |
                                      (((g0 >> 2u) & 1u) << 1u) |
                                      (((b0 >> 2u) & 1u) << 2u) |
                                      (((r1 >> 2u) & 1u) << 3u) |
                                      (((g1 >> 2u) & 1u) << 4u) |
                                      (((b1 >> 2u) & 1u) << 5u));

            plane3[dst_x] = (uint8_t)(((r0 >> 3u) & 1u) |
                                      (((g0 >> 3u) & 1u) << 1u) |
                                      (((b0 >> 3u) & 1u) << 2u) |
                                      (((r1 >> 3u) & 1u) << 3u) |
                                      (((g1 >> 3u) & 1u) << 4u) |
                                      (((b1 >> 3u) & 1u) << 5u));

            plane4[dst_x] = (uint8_t)(((r0 >> 4u) & 1u) |
                                      (((g0 >> 4u) & 1u) << 1u) |
                                      (((b0 >> 4u) & 1u) << 2u) |
                                      (((r1 >> 4u) & 1u) << 3u) |
                                      (((g1 >> 4u) & 1u) << 4u) |
                                      (((b1 >> 4u) & 1u) << 5u));

            plane5[dst_x] = (uint8_t)(((r0 >> 5u) & 1u) |
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
    if (!packet_slot_ready(slot)) {
        return;
    }

    uint32_t packet_start_us = time_us_32();
    static uint32_t s_core1_last_packet_us = 0;
    if (s_core1_last_packet_us != 0u) {
        core1_update_max(&g_core1_packet_gap_us_max, packet_start_us - s_core1_last_packet_us);
    }
    s_core1_last_packet_us = packet_start_us;

    uint16_t encoded_len = slot->len;

    if (encoded_len == 0u) {
        core1_inc(&g_core1_ready_len_zero_events);
    }

    if (encoded_len == 0 || encoded_len > RECV_BUFFER_SIZE) {
        core1_inc(&g_core1_dropped_frames);
        core1_inc(&g_core1_cobs_errors);
        packet_release_slot(slot);
        g_pkt_cons_idx = (uint8_t)((g_pkt_cons_idx + 1u) % PACKET_QUEUE_COUNT);
        return;
    }

    uint32_t t_decode = time_us_32();
    size_t decoded_len = cobs_decode(slot->data, encoded_len, g_decode_buf, FRAME_SIZE_RGB565);
    uint32_t decode_us = (uint32_t)(time_us_32() - t_decode);
    core1_update_max(&g_core1_decode_us_max, decode_us);

    packet_release_slot(slot);
    g_pkt_cons_idx = (uint8_t)((g_pkt_cons_idx + 1u) % PACKET_QUEUE_COUNT);

    if (decoded_len != FRAME_SIZE_RGB565) {
        core1_inc(&g_core1_dropped_frames);
        core1_inc(&g_core1_cobs_errors);
        return;
    }

    if (g_swap_pending) {
        core1_inc(&g_core1_swap_replaced_frames);
    }

    uint8_t build_idx = select_build_buffer();
    uint32_t t_convert = time_us_32();
    convert_rgb565_to_bcm(g_decode_buf, build_idx);
    uint32_t convert_us = (uint32_t)(time_us_32() - t_convert);
    core1_update_max(&g_core1_convert_us_max, convert_us);

    g_pending_idx = build_idx;
    g_swap_pending = true;
    core1_inc(&g_core1_decoded_frames);
}

/* ------------------------------------------------------------------------ */
/*  Core0 USB / parsing                                                     */
/* ------------------------------------------------------------------------ */
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
            g_usb_block_ready_overrun++;
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
                if (packet_slot_ready(slot)) {
                    g_dropped_frames_core0++;
                    g_drop_packet_q_overrun++;
                    g_queue_full_on_delim++;
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
        if (packet_slot_ready(slot)) {
            g_parser_discard = true;
            g_parser_pos = 0;
            g_dropped_frames_core0++;
            g_drop_packet_q_overrun++;
            g_queue_full_mid_packet++;
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

    /* Snapshot Core1 counters (32-bit aligned volatile reads are atomic) */
    uint32_t dec = g_core1_decoded_frames;
    uint32_t disp = g_core1_displayed_frames;
    uint32_t scan = g_core1_scan_frames;
    uint32_t drop1 = g_core1_dropped_frames;
    uint32_t cobs = g_core1_cobs_errors;
    uint32_t dma_to = g_core1_dma_timeouts;
    uint32_t swap_rep = g_core1_swap_replaced_frames;
    uint32_t slot_corrupt = g_core1_slot_corruption_events;
    uint32_t ready_len0 = g_core1_ready_len_zero_events;
    uint32_t pkt_gap_max = g_core1_packet_gap_us_max;
    uint32_t dec_us_max = g_core1_decode_us_max;
    uint32_t conv_us_max = g_core1_convert_us_max;

    /* Reset Core1 max counters after snapshot (best-effort) */
    g_core1_packet_gap_us_max = 0;
    g_core1_decode_us_max = 0;
    g_core1_convert_us_max = 0;

    uint32_t rx = g_rx_total_bytes;
    uint32_t drop = g_dropped_frames_core0 + drop1;
    uint32_t usb_drop = g_usb_drop_bytes;

    uint32_t rx_bps = rx - g_stats_last_rx;
    uint32_t usb_drop_bps = usb_drop - g_stats_last_usb_drop;
    uint32_t dec_fps = dec - g_stats_last_dec;
    uint32_t disp_fps = disp - g_stats_last_disp;
    uint32_t scan_fps = scan - g_stats_last_scan;
    uint32_t drop_ps = drop - g_stats_last_drop;
    uint32_t dma_to_ps = dma_to - g_stats_last_dma_to;

    uint32_t tud_gap_us_max = g_tud_gap_us_max;
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
    g_usb_block_highwater = usb_pending_bytes();
    g_packet_queue_highwater = packet_queue_depth();

    if (tud_cdc_n_connected(0) && tud_cdc_n_write_available(0) > 320) {
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "STAT clk_khz=%lu rx_Bps=%lu usb_drop_Bps=%lu dec_fps=%lu disp_fps=%lu scan_fps=%lu drop_ps=%lu drop=%lu swap_replace=%lu ovf_drop=%lu qovf_drop=%lu qfull_delim=%lu qfull_mid=%lu usb_blk_ovr=%lu slot_corrupt=%lu ready_len0=%lu cobs=%lu usb_hw=%lu pkt_hw=%lu tud_gap_us=%lu dec_us=%lu conv_us=%lu pkt_gap_us=%lu dma_to_ps=%lu dma_to=%lu\r\n",
                         (unsigned long)(clock_get_hz(clk_sys) / 1000u),
                         (unsigned long)rx_bps,
                         (unsigned long)usb_drop_bps,
                         (unsigned long)dec_fps,
                         (unsigned long)disp_fps,
                         (unsigned long)scan_fps,
                         (unsigned long)drop_ps,
                         (unsigned long)drop,
                         (unsigned long)swap_rep,
                         (unsigned long)g_drop_packet_overflow,
                         (unsigned long)g_drop_packet_q_overrun,
                         (unsigned long)g_queue_full_on_delim,
                         (unsigned long)g_queue_full_mid_packet,
                         (unsigned long)g_usb_block_ready_overrun,
                         (unsigned long)slot_corrupt,
                         (unsigned long)ready_len0,
                         (unsigned long)cobs,
                         (unsigned long)usb_hw,
                         (unsigned long)pkt_hw,
                         (unsigned long)tud_gap_us_max,
                         (unsigned long)dec_us_max,
                         (unsigned long)conv_us_max,
                         (unsigned long)pkt_gap_max,
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
        if ((now - g_last_tud_us) > g_tud_gap_us_max) {
            g_tud_gap_us_max = now - g_last_tud_us;
        }
    }
    g_last_tud_us = now;

    tud_task();
    ingest_usb_blocks();
}

/* ------------------------------------------------------------------------ */
/*  Core1 entry                                                             */
/* ------------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------------ */
/*  Main (Core0)                                                            */
/* ------------------------------------------------------------------------ */
int main(void) {
    init_system_clock();

    memset(g_bcm_planes, 0, sizeof(g_bcm_planes));
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
