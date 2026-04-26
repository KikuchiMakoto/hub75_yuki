#include <stdint.h>
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_descriptors.h"

/* ----------------------------------------------------------------------
 * Interface numbers
 * ---------------------------------------------------------------------- */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_VENDOR,
    ITF_NUM_TOTAL
};

/* ----------------------------------------------------------------------
 * Endpoint addresses
 * ---------------------------------------------------------------------- */
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define EPNUM_VENDOR_OUT  0x03
#define EPNUM_VENDOR_IN   0x83

/* ----------------------------------------------------------------------
 * Configuration descriptor length
 * ---------------------------------------------------------------------- */
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN)

/* ----------------------------------------------------------------------
 * Device descriptor
 * ---------------------------------------------------------------------- */
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,          // USB 2.1 (needed for BOS)
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,
    .idProduct          = 0x0101,
    .bcdDevice          = 0x0200,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

/* ----------------------------------------------------------------------
 * Configuration descriptor (CDC ACM + Vendor)
 * ---------------------------------------------------------------------- */
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, CFG_TUD_CDC_EP_BUFSIZE),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 5, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, CFG_TUD_VENDOR_EP_BUFSIZE),
};

/* ----------------------------------------------------------------------
 * String descriptors
 * ---------------------------------------------------------------------- */
static const char *const string_desc_arr[] = {
    NULL,
    "kmakoto",
    "HUB75 Controller v2",
    NULL,               // serial number -> filled at runtime
    "HUB75 CDC",
    "HUB75 WebUSB"
};

static uint16_t _desc_str[32];

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;
    const char *str;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

    if (index == 0) {
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            str = serial;
        } else {
            if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
                return NULL;
            }
            str = string_desc_arr[index];
        }

        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; ++i) {
            _desc_str[1 + i] = (uint8_t)str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* ----------------------------------------------------------------------
 * BOS + WebUSB + Microsoft OS 2.0 Descriptors
 * ---------------------------------------------------------------------- */

#define MS_OS_20_DESC_LEN  0xb2

#define MS_OS_20_SET_HEADER_DESCRIPTOR       0x00
#define MS_OS_20_SUBSET_HEADER_CONFIGURATION 0x01
#define MS_OS_20_SUBSET_HEADER_FUNCTION      0x02
#define MS_OS_20_FEATURE_COMPATIBLE_ID       0x03
#define MS_OS_20_FEATURE_REG_PROPERTY        0x04

static const uint8_t desc_bos[] = {
    TUD_BOS_DESCRIPTOR(
        TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN,
        2),
    TUD_BOS_WEBUSB_DESCRIPTOR(VENDOR_REQUEST_WEBUSB, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT)
};

uint8_t const *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

/* ----------------------------------------------------------------------
 * WebUSB URL Descriptor
 * ---------------------------------------------------------------------- */
static const uint8_t desc_url[] = {
    3,                              // bLength
    3,                              // bDescriptorType = URL
    1,                              // bScheme = https
};

uint8_t const *tud_descriptor_url_cb(void) {
    return desc_url;
}

/* ----------------------------------------------------------------------
 * Microsoft OS 2.0 Descriptor
 * ---------------------------------------------------------------------- */
uint8_t const desc_ms_os_20[] = {
    // Set header: length, type, windows version, total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header: length, type, configuration index, reserved, configuration total length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    // Compatible ID descriptor: length, type, compatible ID, sub-compatible ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATIBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Registry property descriptor: length, type, data-type, name-length, name "DeviceInterfaceGUIDs\0"
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00,
    'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00,
    'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    // PropertyData: "{975F44D9-0D08-43FD-8B3E-127CA8AFFF9D}"
    '{', 0x00, '9', 0x00, '7', 0x00, '5', 0x00, 'F', 0x00, '4', 0x00,
    '4', 0x00, 'D', 0x00, '9', 0x00, '-', 0x00, '0', 0x00, 'D', 0x00,
    '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00, '3', 0x00, 'F', 0x00,
    'D', 0x00, '-', 0x00, '8', 0x00, 'B', 0x00, '3', 0x00, 'E', 0x00,
    '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00,
    '8', 0x00, 'A', 0x00, 'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00,
    'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "Incorrect size");

uint8_t const *tud_descriptor_ms_os_20_cb(void) {
    return desc_ms_os_20;
}

/* ----------------------------------------------------------------------
 * Vendor class request handler (WebUSB control requests)
 * ---------------------------------------------------------------------- */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    (void)rhport;

    if (stage != CONTROL_STAGE_SETUP) return true;

    switch (request->bRequest) {
        case VENDOR_REQUEST_WEBUSB: {
            if (request->wIndex == 0x02) { // WEBUSB_REQUEST_GET_URL
                uint16_t len = (uint16_t) sizeof(desc_url);
                return tud_control_xfer(rhport, request, (void *)(uintptr_t) desc_url, len);
            }
            break;
        }

        case VENDOR_REQUEST_MICROSOFT: {
            if (request->wIndex == 0x07) { // MS_OS_20_REQUEST_DESCRIPTOR
                uint16_t len = TU_MIN(request->wLength, MS_OS_20_DESC_LEN);
                return tud_control_xfer(rhport, request,
                                        (void *)(uintptr_t) desc_ms_os_20, len);
            }
            break;
        }
    }

    return false;
}
