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

/* ------------------------------------------------------------------------ */
/*  GPIO bit masks                                                          */
/* ------------------------------------------------------------------------ */
#define LAT_MASK  (1u << PIN_LAT)
#define OE_MASK   (1u << PIN_OE)
#define ADDR_MASK ((1u << PIN_ADDR_A) | (1u << PIN_ADDR_B) | \
                   (1u << PIN_ADDR_C) | (1u << PIN_ADDR_D))

/* ------------------------------------------------------------------------ */
/*  USB / packet queue constants                                            */
/* ------------------------------------------------------------------------ */
#define USB_BLOCK_COUNT     2u
#define USB_BLOCK_SIZE      2048u
#define PACKET_QUEUE_COUNT  3u
#define SCAN_BUFFER_COUNT   3u
#define PARSE_BUDGET_MIN    512u
#define PARSE_BUDGET_MAX    4096u
#define TUD_GAP_SHRINK_US   700u
#define DMA_WAIT_TIMEOUT_US 3000u

#define FRAME_UPDATE_PERIOD_US (1000000u / MAX_FRAME_UPDATE_FPS)

/* ------------------------------------------------------------------------ */
/*  Type definitions                                                        */
/* ------------------------------------------------------------------------ */
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
/*  Build-time assertions                                                   */
/* ------------------------------------------------------------------------ */
_Static_assert(MAX_FRAME_UPDATE_FPS > 0, "MAX_FRAME_UPDATE_FPS must be > 0");
_Static_assert(USB_BLOCK_COUNT == 2u, "USB_BLOCK_COUNT must be 2 for SPSC");
_Static_assert(RECV_BUFFER_SIZE <= 65535u, "RECV_BUFFER_SIZE must fit 16-bit");
_Static_assert(COLOR_DEPTH == 9u, "COLOR_DEPTH must be 9 for v3 native");
_Static_assert((DISPLAY_WIDTH % 4u) == 0u, "DISPLAY_WIDTH must be multiple of 4");

/* ------------------------------------------------------------------------ */
/*  HUB75 / PIO / DMA globals  (Core1 primary)                              */
/* ------------------------------------------------------------------------ */
static PIO g_pio = pio0;
static uint g_sm_data = 0;       /* SM0: data shifter     */
static uint g_sm_latoe = 1;      /* SM1: LAT+OE generator */
static uint g_sm_addr = 2;       /* SM2: address output   (future) */
static uint g_sm_seq = 3;        /* SM3: sequencer        (future) */
static int g_dma_ch_data = -1;   /* CH0: row data -> SM0  */
static int g_dma_ch_oe = -1;     /* CH1: OE time  -> SM1  */

/* 9-bit BCM planes: packed 5 pixels / 32-bit word                        */
static uint32_t __attribute__((aligned(4)))
    g_bcm_planes[SCAN_BUFFER_COUNT][SCAN_ROWS][COLOR_DEPTH][WORDS_PER_ROW];

/* ------------------------------------------------------------------------ */
/*  USB stream / packet queue  (Core0 primary)                              */
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
/*  Gamma / colour LUTs  (9-bit direct: 0..511)                             */
/* ------------------------------------------------------------------------ */
static uint16_t g_gamma9_lut[256];
static uint16_t g_r5_to_cd[32];
static uint16_t g_g6_to_cd[64];
static uint16_t g_b5_to_cd[32];

/* ------------------------------------------------------------------------ */
/*  Frame-swap state                                                        */
/* ------------------------------------------------------------------------ */
static uint8_t g_display_idx = 0;
static uint8_t g_pending_idx = 0;
static bool g_swap_pending = false;
static uint64_t g_next_swap_allowed_us = 0;

/* ------------------------------------------------------------------------ */
/*  Core1 display state  (written only by Core1)                            */
/* ------------------------------------------------------------------------ */
static volatile uint32_t g_core1_scan_frames = 0;
static volatile uint32_t g_core1_displayed_frames = 0;
static volatile uint32_t g_core1_dma_timeouts = 0;

/* ------------------------------------------------------------------------ */
/*  Core0 statistics                                                        */
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
/*  Stats snapshot state (Core0 only)                                       */
/* ------------------------------------------------------------------------ */
static uint32_t g_stats_last_rx = 0;
static uint32_t g_stats_last_scan = 0;
static uint32_t g_stats_last_disp = 0;
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

static inline uint8_t expand5(uint8_t v) {
    return (uint8_t)(((uint16_t)v * 255u + 15u) / 31u);
}

static inline uint8_t expand6(uint8_t v) {
    return (uint8_t)(((uint16_t)v * 255u + 31u) / 63u);
}

/* Pre-computed gamma 2.2 table (0..255 -> 0..255) */
static inline uint8_t gamma8_raw(uint8_t v) {
    static const uint8_t lut[256] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
        1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,
        3,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,
        6,7,7,7,8,8,8,9,9,9,10,10,11,11,11,12,
        12,13,13,13,14,14,15,15,16,16,17,17,18,18,19,19,
        20,20,21,22,22,23,23,24,25,25,26,26,27,28,28,29,
        30,30,31,32,33,33,34,35,35,36,37,38,39,39,40,41,
        42,43,43,44,45,46,47,48,49,49,50,51,52,53,54,55,
        56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,
        73,74,75,76,77,78,79,81,82,83,84,85,87,88,89,90,
        91,93,94,95,97,98,99,100,102,103,105,106,107,109,110,111,
        113,114,116,117,119,120,121,123,124,126,127,129,130,132,133,135,
        137,138,140,141,143,145,146,148,149,151,153,154,156,158,159,161,
        163,165,166,168,170,172,173,175,177,179,181,182,184,186,188,190,
        192,194,196,197,199,201,203,205,207,209,211,213,215,217,219,221,
        223,225,227,229,231,234,236,238,240,242,244,246,248,251,253,255
    };
    return lut[v];
}

/* Core1 atomic helpers (single-threaded, no lock needed) */
static inline void core1_inc(volatile uint32_t *ctr) {
    *ctr = *ctr + 1u;
}

static inline bool packet_slot_ready(packet_slot_t *slot) {
    if (!slot->ready) return false;
    __dmb();
    return true;
}

/* ------------------------------------------------------------------------ */
/*  USB helpers                                                             */
/* ------------------------------------------------------------------------ */
static uint32_t usb_pending_bytes(void) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < USB_BLOCK_COUNT; ++i) {
        const usb_block_t *blk = &g_usb_blocks[i];
        if (!blk->ready) continue;
        if (i == g_usb_cons_idx) {
            if (blk->len > g_usb_cons_pos)
                total += (uint32_t)(blk->len - g_usb_cons_pos);
        } else {
            total += blk->len;
        }
    }
    return total;
}

static uint32_t packet_queue_depth(void) {
    uint32_t depth = 0;
    for (uint32_t i = 0; i < PACKET_QUEUE_COUNT; ++i)
        if (packet_slot_ready(&g_packet_slots[i])) depth++;
    return depth;
}

static inline uint32_t compute_parse_budget(void) {
    uint32_t pending = usb_pending_bytes();
    if (pending == 0) return 0;
    uint32_t budget = pending;
    if (budget > PARSE_BUDGET_MAX) budget = PARSE_BUDGET_MAX;
    if (budget < PARSE_BUDGET_MIN) budget = PARSE_BUDGET_MIN;
    if (g_tud_gap_us_max > TUD_GAP_SHRINK_US && budget > PARSE_BUDGET_MIN)
        budget >>= 1u;
    if (budget < PARSE_BUDGET_MIN) budget = PARSE_BUDGET_MIN;
    return budget;
}

static inline void usb_publish_block(usb_block_t *blk) {
    if (blk->len == 0 || blk->ready) return;
    blk->ready = 1u;
    uint32_t pb = usb_pending_bytes();
    if (pb > g_usb_block_highwater) g_usb_block_highwater = pb;
    uint8_t next = g_usb_prod_idx ^ 1u;
    g_usb_prod_idx = next;
    if (!g_usb_blocks[next].ready) g_usb_blocks[next].len = 0;
}

static inline void packet_publish_slot(packet_slot_t *slot) {
    __dmb();
    slot->ready = 1u;
    uint32_t pd = packet_queue_depth();
    if (pd > g_packet_queue_highwater) g_packet_queue_highwater = pd;
    uint8_t next = (uint8_t)((g_pkt_prod_idx + 1u) % PACKET_QUEUE_COUNT);
    g_pkt_prod_idx = next;
    if (!packet_slot_ready(&g_packet_slots[next])) g_packet_slots[next].len = 0;
}

static inline void packet_release_slot(packet_slot_t *slot) {
    slot->len = 0;
    __dmb();
    slot->ready = 0u;
}

static inline bool stream_pop_usb_byte(uint8_t *out) {
    usb_block_t *blk = &g_usb_blocks[g_usb_cons_idx];
    if (!blk->ready) return false;
    if (g_usb_cons_pos >= blk->len) {
        blk->len = 0; blk->ready = 0u; g_usb_cons_pos = 0;
        g_usb_cons_idx ^= 1u;
        blk = &g_usb_blocks[g_usb_cons_idx];
        if (!blk->ready) return false;
    }
    *out = blk->data[g_usb_cons_pos++];
    if (g_usb_cons_pos >= blk->len) {
        blk->len = 0; blk->ready = 0u; g_usb_cons_pos = 0;
        g_usb_cons_idx ^= 1u;
    }
    return true;
}

static void sync_drop_until_delimiter(const uint8_t *buf, uint32_t *idx, uint32_t size) {
    uint32_t start = *idx;
    while (*idx < size) {
        if (buf[*idx] == 0x00u) {
            (*idx)++;
            g_usb_resync = false;
            g_parser_pos = 0;
            g_parser_discard = false;
            break;
        }
        (*idx)++;
    }
    g_usb_drop_bytes += (*idx - start);
}

/* ------------------------------------------------------------------------ */
/*  Clock / LUT initialisation                                              */
/* ------------------------------------------------------------------------ */
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
    if (!ok) set_sys_clock_khz(133000u, true);
}

static void init_gamma_and_color_luts(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint8_t gv = gamma8_raw((uint8_t)i);
        /* Map 0..255 -> 0..511 accurately */
        g_gamma9_lut[i] = (uint16_t)(((uint32_t)gv * 511u + 127u) / 255u);
    }
    for (uint32_t i = 0; i < 32; ++i) {
        uint8_t v8 = expand5((uint8_t)i);
        g_r5_to_cd[i] = g_gamma9_lut[v8];
        g_b5_to_cd[i] = g_gamma9_lut[v8];
    }
    for (uint32_t i = 0; i < 64; ++i) {
        uint8_t v8 = expand6((uint8_t)i);
        g_g6_to_cd[i] = g_gamma9_lut[v8];
    }
}

/* ------------------------------------------------------------------------ */
/*  HUB75 GPIO helpers                                                      */
/* ------------------------------------------------------------------------ */
void __not_in_flash_func(set_row_address)(uint32_t row) {
    sio_hw->gpio_clr = ADDR_MASK;
    uint32_t bits = (((row >> 0u) & 1u) << PIN_ADDR_A) |
                    (((row >> 1u) & 1u) << PIN_ADDR_B) |
                    (((row >> 2u) & 1u) << PIN_ADDR_C) |
                    (((row >> 3u) & 1u) << PIN_ADDR_D);
    sio_hw->gpio_set = bits;
}

static void hub75_gpio_init(void) {
    for (uint32_t pin = PIN_R0; pin <= PIN_ADDR_D; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }
    gpio_put(PIN_OE, 1);
}

/* ------------------------------------------------------------------------ */
/*  PIO initialisation                                                      */
/* ------------------------------------------------------------------------ */
static void hub75_pio_init(void) {
    /* --- SM0: data shifter (5 pixels / word) --- */
    uint off0 = pio_add_program(g_pio, &hub75_data_v3_program);
    pio_sm_config c0 = hub75_data_v3_program_get_default_config(off0);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm_data, PIN_R0, 6, true);
    for (uint32_t pin = PIN_R0; pin < PIN_R0 + 6u; ++pin) pio_gpio_init(g_pio, pin);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm_data, PIN_CLK, 1, true);
    pio_gpio_init(g_pio, PIN_CLK);
    sm_config_set_out_pins(&c0, PIN_R0, 6);
    sm_config_set_sideset_pins(&c0, PIN_CLK);
    /* autopull at 30 bits -> 5 x out pins,6 triggers refill */
    sm_config_set_out_shift(&c0, true, true, 30);
    sm_config_set_fifo_join(&c0, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c0, 1.0f);
    pio_sm_init(g_pio, g_sm_data, off0, &c0);
    pio_sm_set_enabled(g_pio, g_sm_data, true);

    /* --- SM1: LAT + OE generator --- */
    uint off1 = pio_add_program(g_pio, &hub75_latoe_v3_program);
    pio_sm_config c1 = hub75_latoe_v3_program_get_default_config(off1);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm_latoe, PIN_LAT, 1, true);
    pio_gpio_init(g_pio, PIN_LAT);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm_latoe, PIN_OE, 1, true);
    pio_gpio_init(g_pio, PIN_OE);
    sm_config_set_sideset_pins(&c1, PIN_LAT);
    sm_config_set_set_pins(&c1, PIN_OE, 1);
    sm_config_set_fifo_join(&c1, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c1, 1.0f);
    pio_sm_init(g_pio, g_sm_latoe, off1, &c1);
    pio_sm_set_enabled(g_pio, g_sm_latoe, true);

    /* --- SM2: address output (future use) --- */
    uint off2 = pio_add_program(g_pio, &hub75_addr_v3_program);
    pio_sm_config c2 = hub75_addr_v3_program_get_default_config(off2);
    pio_sm_set_consecutive_pindirs(g_pio, g_sm_addr, PIN_ADDR_A, 4, true);
    for (uint32_t pin = PIN_ADDR_A; pin < PIN_ADDR_A + 4u; ++pin)
        pio_gpio_init(g_pio, pin);
    sm_config_set_out_pins(&c2, PIN_ADDR_A, 4);
    sm_config_set_clkdiv(&c2, 1.0f);
    pio_sm_init(g_pio, g_sm_addr, off2, &c2);
    /* Keep disabled until we switch to PIO-driven addressing */
    /* pio_sm_set_enabled(g_pio, g_sm_addr, true); */

    /* --- SM3: sequencer (future use) --- */
    uint off3 = pio_add_program(g_pio, &hub75_seq_v3_program);
    pio_sm_config c3 = hub75_seq_v3_program_get_default_config(off3);
    sm_config_set_clkdiv(&c3, 1.0f);
    pio_sm_init(g_pio, g_sm_seq, off3, &c3);
    /* pio_sm_set_enabled(g_pio, g_sm_seq, true); */
}

/* ------------------------------------------------------------------------ */
/*  DMA initialisation                                                      */
/* ------------------------------------------------------------------------ */
static void hub75_dma_init(void) {
    /* CH0: row data -> PIO0 SM0 TX FIFO */
    g_dma_ch_data = dma_claim_unused_channel(true);
    dma_channel_config c0 = dma_channel_get_default_config((uint)g_dma_ch_data);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);
    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);
    channel_config_set_dreq(&c0, pio_get_dreq(g_pio, g_sm_data, true));
    dma_channel_set_config((uint)g_dma_ch_data, &c0, false);
    dma_channel_set_write_addr((uint)g_dma_ch_data, &g_pio->txf[g_sm_data], false);
    dma_channel_set_irq0_enabled((uint)g_dma_ch_data, true);

    /* CH1: OE duration -> PIO0 SM1 TX FIFO */
    g_dma_ch_oe = dma_claim_unused_channel(true);
    dma_channel_config c1 = dma_channel_get_default_config((uint)g_dma_ch_oe);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);
    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_dreq(&c1, pio_get_dreq(g_pio, g_sm_latoe, true));
    dma_channel_set_config((uint)g_dma_ch_oe, &c1, false);
    dma_channel_set_write_addr((uint)g_dma_ch_oe, &g_pio->txf[g_sm_latoe], false);
    dma_channel_set_irq0_enabled((uint)g_dma_ch_oe, true);
}

/* ------------------------------------------------------------------------ */
/*  DMA IRQ handlers                                                        */
/* ------------------------------------------------------------------------ */
static volatile bool g_dma_data_done = false;
static volatile bool g_dma_oe_done = false;

void __not_in_flash_func(dma_irq0_handler)(void) {
    uint32_t mask_data = 1u << (uint32_t)g_dma_ch_data;
    uint32_t mask_oe = 1u << (uint32_t)g_dma_ch_oe;
    if (dma_hw->ints0 & mask_data) {
        dma_hw->ints0 = mask_data;
        g_dma_data_done = true;
    }
    if (dma_hw->ints0 & mask_oe) {
        dma_hw->ints0 = mask_oe;
        g_dma_oe_done = true;
    }
}

/* ------------------------------------------------------------------------ */
/*  Display refresh  (Core1, timing-critical)                               */
/* ------------------------------------------------------------------------ */
/* OE duration table: bit n needs (1<<n) PIO cycles (5ns @ 200MHz)          */
static const uint32_t g_oe_cycles_table[COLOR_DEPTH] = {
    200u,   /* bit0: 1us  */
    400u,   /* bit1: 2us  */
    800u,   /* bit2: 4us  */
    1600u,  /* bit3: 8us  */
    3200u,  /* bit4: 16us */
    6400u,  /* bit5: 32us */
    12800u, /* bit6: 64us */
    25600u, /* bit7: 128us*/
    51200u  /* bit8: 256us*/
};

void __not_in_flash_func(hub75_refresh_once)(void) {
    uint8_t display_idx = g_display_idx;

    for (uint32_t bit = 0; bit < COLOR_DEPTH; ++bit) {
        uint32_t oe_cycles = g_oe_cycles_table[bit];

        for (uint32_t row = 0; row < SCAN_ROWS; ++row) {
            /* 1. Blank display */
            sio_hw->gpio_set = OE_MASK;

            /* 2. Start DMA data transfer */
            uint32_t *src = g_bcm_planes[display_idx][row][bit];
            dma_channel_set_read_addr((uint)g_dma_ch_data, src, false);
            g_dma_data_done = false;
            dma_channel_set_trans_count((uint)g_dma_ch_data, WORDS_PER_ROW, true);

            /* 3. Wait for data DMA complete */
            uint32_t wait_start = time_us_32();
            bool data_ok = true;
            while (!g_dma_data_done) {
                __wfe();
                if ((uint32_t)(time_us_32() - wait_start) > DMA_WAIT_TIMEOUT_US) {
                    if (!dma_channel_is_busy((uint)g_dma_ch_data)) {
                        g_dma_data_done = true;
                    } else {
                        data_ok = false;
                        break;
                    }
                }
            }

            if (!data_ok) {
                core1_inc(&g_core1_dma_timeouts);
                dma_channel_abort((uint)g_dma_ch_data);
                dma_hw->ints0 = 1u << (uint32_t)g_dma_ch_data;
                continue;
            }

            /* 4. SM0 has shifted data. Now trigger SM1 LAT+OE sequence.
             *    Send OE duration via DMA to SM1 FIFO. */
            g_dma_oe_done = false;
            dma_channel_set_read_addr((uint)g_dma_ch_oe, &oe_cycles, false);
            dma_channel_set_trans_count((uint)g_dma_ch_oe, 1, true);

            /* 5. Wait for OE DMA (and thus SM1 sequence) to complete.
             *    SM1 raises IRQ0 at end of OE window. */
            wait_start = time_us_32();
            bool oe_ok = true;
            while (!g_dma_oe_done) {
                __wfe();
                if ((uint32_t)(time_us_32() - wait_start) > DMA_WAIT_TIMEOUT_US) {
                    if (!dma_channel_is_busy((uint)g_dma_ch_oe)) {
                        g_dma_oe_done = true;
                    } else {
                        oe_ok = false;
                        break;
                    }
                }
            }

            if (!oe_ok) {
                core1_inc(&g_core1_dma_timeouts);
                dma_channel_abort((uint)g_dma_ch_oe);
                dma_hw->ints0 = 1u << (uint32_t)g_dma_ch_oe;
                continue;
            }

            /* 6. Update row address for next cycle */
            set_row_address(row);
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
/*  RGB565 -> 9-bit BCM conversion  (Core0)                                 */
/* ------------------------------------------------------------------------ */
void __not_in_flash_func(convert_rgb565_to_bcm)(const uint8_t *src, uint8_t plane_idx) {
    /* Temporary packed bit-plane buffer for one row */
    uint8_t packed[COLOR_DEPTH][DISPLAY_WIDTH];

    for (uint32_t row = 0; row < SCAN_ROWS; ++row) {
        uint32_t y_upper = row;
        uint32_t y_lower = row + SCAN_ROWS;
        const uint8_t *up = src + (y_upper * DISPLAY_WIDTH * 2u);
        const uint8_t *lo = src + (y_lower * DISPLAY_WIDTH * 2u);

        /* Convert each column to 9-bit colour, then pack into bit-planes */
        for (uint32_t x = 0; x < DISPLAY_WIDTH; ++x) {
            uint32_t rev_x = DISPLAY_WIDTH - 1u - x;
            uint16_t p_up = (uint16_t)up[0] | ((uint16_t)up[1] << 8u);
            uint16_t p_lo = (uint16_t)lo[0] | ((uint16_t)lo[1] << 8u);
            up += 2;
            lo += 2;

            uint16_t r0 = g_r5_to_cd[(p_up >> 11u) & 0x1Fu];
            uint16_t g0 = g_g6_to_cd[(p_up >> 5u) & 0x3Fu];
            uint16_t b0 = g_b5_to_cd[p_up & 0x1Fu];
            uint16_t r1 = g_r5_to_cd[(p_lo >> 11u) & 0x1Fu];
            uint16_t g1 = g_g6_to_cd[(p_lo >> 5u) & 0x3Fu];
            uint16_t b1 = g_b5_to_cd[p_lo & 0x1Fu];

            for (uint32_t bit = 0; bit < COLOR_DEPTH; ++bit) {
                uint8_t v = (uint8_t)(((r0 >> bit) & 1u) |
                                      (((g0 >> bit) & 1u) << 1u) |
                                      (((b0 >> bit) & 1u) << 2u) |
                                      (((r1 >> bit) & 1u) << 3u) |
                                      (((g1 >> bit) & 1u) << 4u) |
                                      (((b1 >> bit) & 1u) << 5u));
                packed[bit][rev_x] = v;
            }
        }

        /* Pack 5 pixels into each 32-bit word */
        for (uint32_t bit = 0; bit < COLOR_DEPTH; ++bit) {
            uint32_t *dst = g_bcm_planes[plane_idx][row][bit];
            for (uint32_t wx = 0; wx < WORDS_PER_ROW; ++wx) {
                uint32_t base = wx * PIXELS_PER_WORD;
                uint32_t word = 0;
                for (uint32_t i = 0; i < PIXELS_PER_WORD && (base + i) < DISPLAY_WIDTH; ++i) {
                    word |= (uint32_t)packed[bit][base + i] << (i * 6u);
                }
                dst[wx] = word;
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  Frame buffer management                                                 */
/* ------------------------------------------------------------------------ */
static uint8_t select_build_buffer(void) {
    bool used[SCAN_BUFFER_COUNT] = {false, false, false};
    used[g_display_idx] = true;
    if (g_swap_pending) used[g_pending_idx] = true;
    for (uint8_t i = 0; i < SCAN_BUFFER_COUNT; ++i)
        if (!used[i]) return i;
    return (uint8_t)((g_display_idx + 1u) % SCAN_BUFFER_COUNT);
}

/* ------------------------------------------------------------------------ */
/*  Core1 packet processing                                                 */
/* ------------------------------------------------------------------------ */
static void core1_process_one_packet(void) {
    packet_slot_t *slot = &g_packet_slots[g_pkt_cons_idx];
    if (!packet_slot_ready(slot)) return;

    uint16_t encoded_len = slot->len;
    if (encoded_len == 0 || encoded_len > RECV_BUFFER_SIZE) {
        packet_release_slot(slot);
        g_pkt_cons_idx = (uint8_t)((g_pkt_cons_idx + 1u) % PACKET_QUEUE_COUNT);
        return;
    }

    size_t decoded_len = cobs_decode(slot->data, encoded_len, g_decode_buf, FRAME_SIZE_RGB565);
    packet_release_slot(slot);
    g_pkt_cons_idx = (uint8_t)((g_pkt_cons_idx + 1u) % PACKET_QUEUE_COUNT);

    if (decoded_len != FRAME_SIZE_RGB565) return;

    uint8_t build_idx = select_build_buffer();
    convert_rgb565_to_bcm(g_decode_buf, build_idx);

    g_pending_idx = build_idx;
    g_swap_pending = true;
}

/* ------------------------------------------------------------------------ */
/*  Core0 USB ingestion                                                     */
/* ------------------------------------------------------------------------ */
static void ingest_usb_blocks(void) {
    static uint8_t trash[256];
    while (tud_cdc_available()) {
        uint32_t avail = tud_cdc_available();
        if (avail == 0) break;

        if (g_usb_resync) {
            uint32_t req = min_u32(avail, (uint32_t)sizeof(trash));
            uint32_t n = tud_cdc_read(trash, req);
            if (n == 0) break;
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
            if (n == 0) break;
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
        if (n == 0) break;
        g_rx_total_bytes += n;
        blk->len = (uint16_t)(blk->len + n);
        if (blk->len == USB_BLOCK_SIZE) usb_publish_block(blk);
    }
    if (!tud_cdc_available())
        usb_publish_block(&g_usb_blocks[g_usb_prod_idx]);
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
        if (g_parser_discard) continue;
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

/* ------------------------------------------------------------------------ */
/*  Statistics emission                                                     */
/* ------------------------------------------------------------------------ */
static void emit_stats(void) {
    uint64_t now = time_us_64();
    if ((now - g_stats_last_us) < STATS_INTERVAL_US) return;
    g_stats_last_us = now;

    uint32_t scan = g_core1_scan_frames;
    uint32_t disp = g_core1_displayed_frames;
    uint32_t dma_to = g_core1_dma_timeouts;

    uint32_t rx = g_rx_total_bytes;
    uint32_t drop = g_dropped_frames_core0;
    uint32_t usb_drop = g_usb_drop_bytes;

    uint32_t rx_bps = rx - g_stats_last_rx;
    uint32_t usb_drop_bps = usb_drop - g_stats_last_usb_drop;
    uint32_t scan_fps = scan - g_stats_last_scan;
    uint32_t disp_fps = disp - g_stats_last_disp;
    uint32_t drop_ps = drop - g_stats_last_drop;
    uint32_t dma_to_ps = dma_to - g_stats_last_dma_to;

    uint32_t tud_gap = g_tud_gap_us_max;
    uint32_t usb_hw = g_usb_block_highwater;
    uint32_t pkt_hw = g_packet_queue_highwater;

    g_stats_last_rx = rx;
    g_stats_last_scan = scan;
    g_stats_last_disp = disp;
    g_stats_last_drop = drop;
    g_stats_last_usb_drop = usb_drop;
    g_stats_last_dma_to = dma_to;

    g_tud_gap_us_max = 0;
    g_usb_block_highwater = usb_pending_bytes();
    g_packet_queue_highwater = packet_queue_depth();

    if (tud_cdc_n_connected(0) && tud_cdc_n_write_available(0) > 320) {
        char line[480];
        int n = snprintf(line, sizeof(line),
            "STAT clk_khz=%lu rx_Bps=%lu usb_drop_Bps=%lu scan_fps=%lu disp_fps=%lu drop_ps=%lu drop=%lu ovf=%lu qovf=%lu qfull_d=%lu qfull_m=%lu usb_blk_ovr=%lu usb_hw=%lu pkt_hw=%lu tud_gap_us=%lu dma_to_ps=%lu dma_to=%lu\r\n",
            (unsigned long)(clock_get_hz(clk_sys) / 1000u),
            (unsigned long)rx_bps,
            (unsigned long)usb_drop_bps,
            (unsigned long)scan_fps,
            (unsigned long)disp_fps,
            (unsigned long)drop_ps,
            (unsigned long)drop,
            (unsigned long)g_drop_packet_overflow,
            (unsigned long)g_drop_packet_q_overrun,
            (unsigned long)g_queue_full_on_delim,
            (unsigned long)g_queue_full_mid_packet,
            (unsigned long)g_usb_block_ready_overrun,
            (unsigned long)usb_hw,
            (unsigned long)pkt_hw,
            (unsigned long)tud_gap,
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
        uint32_t gap = now - g_last_tud_us;
        if (gap > g_tud_gap_us_max) g_tud_gap_us_max = gap;
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

    dma_hw->ints0 = (1u << (uint32_t)g_dma_ch_data) | (1u << (uint32_t)g_dma_ch_oe);
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
