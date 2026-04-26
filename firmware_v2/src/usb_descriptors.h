#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

enum {
  VENDOR_REQUEST_WEBUSB = 1,
  VENDOR_REQUEST_MICROSOFT = 2
};

extern uint8_t const desc_ms_os_20[];

#endif /* USB_DESCRIPTORS_H_ */
