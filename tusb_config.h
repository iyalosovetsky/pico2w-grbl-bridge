// TinyUSB configuration: device stack on the native RP2040/RP2350 USB controller
// (rhport 0) for stdio-over-CDC only, so the board's own USB port stays free for
// flashing and debugging. The actual link to grblHAL's console runs on the Pico-PIO-USB
// host stack (rhport 1), on separate GPIO pins — see usb_host_cdc.c.
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

// OPT_OS_PICO (not OPT_OS_NONE): the device stack runs on core0, the host stack on
// core1, so TinyUSB needs the pico-sdk OS abstraction for its internal locking.
#define CFG_TUSB_OS OPT_OS_PICO

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

// ---- Device stack: native USB controller, CDC-ACM used for stdio only ----
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

// ---- Host stack: Pico-PIO-USB on rhport 1, talking to grblHAL's console ----
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 1
#define BOARD_TUH_RHPORT 1

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif
#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUH_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HUB 1 // some USB-A/OTG host adapters present an internal hub
#define CFG_TUH_DEVICE_MAX (3 * CFG_TUH_HUB + 1)

#define CFG_TUH_HID 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

// ---- CDC (grblHAL's console) ----
#define CFG_TUH_CDC 1
#define CFG_TUH_CDC_FTDI 0
#define CFG_TUH_CDC_CP210X 0
#define CFG_TUH_CDC_CH34X 0

#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM 0x03
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM \
    { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

#ifdef __cplusplus
}
#endif

#endif
