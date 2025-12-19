/**
 * Custom TinyUSB Configuration for HUB75 LED Controller
 *
 * Optimized for high-throughput USB CDC serial reception
 * Frame size: 128x64 RGB565 = 16KB per frame
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// defined by compiler flags for flexibility
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS             OPT_OS_PICO
#endif

// CFG_TUSB_DEBUG is defined by compiler in DEBUG builds
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

// Enable device stack
#define CFG_TUD_ENABLED         1

// Default is max speed that hardware controller could support with on-chip PHY
#define CFG_TUD_MAX_SPEED       OPT_MODE_DEFAULT_SPEED

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE  64
#endif

//--------------------------------------------------------------------
// CLASS CONFIGURATION
//--------------------------------------------------------------------

// CDC FIFO sizes - optimized for high throughput frame reception
// RP2040 has 264KB SRAM, we can afford large buffers

// Number of CDC interfaces
#define CFG_TUD_CDC             1

// CDC Endpoint buffer size (hardware packet size)
// USB Full Speed max is 64 bytes per packet
// Larger values here don't help much for Full Speed
#define CFG_TUD_CDC_EP_BUFSIZE  64

// CDC RX FIFO size - SOFTWARE buffer for received data
// This is the key setting for high throughput!
// 16KB = one full 128x64 RGB565 frame
// Set to 8KB for 128x32 or 16KB for 128x64
#define CFG_TUD_CDC_RX_BUFSIZE  16384

// CDC TX FIFO size - for sending data back to host
// We don't send much data, keep it small
#define CFG_TUD_CDC_TX_BUFSIZE  256

// Other device classes (disabled)
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

//--------------------------------------------------------------------
// MEMORY CONFIGURATION
//--------------------------------------------------------------------

// Use static memory allocation (no malloc)
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__ ((aligned(4)))

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
