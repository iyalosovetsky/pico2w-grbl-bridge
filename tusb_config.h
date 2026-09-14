// TinyUSB configuration: host-only, on the board's native RP2040/RP2350 USB controller
// (rhport 0, via a USB-A OTG adapter on the board's own micro-USB port) — talks to
// grblHAL's console on the SKR Pico. Debug output goes out UART0 instead (see
// CMakeLists.txt), since the native port is occupied by the host role here.
//
// native-usb-host branch: trying the built-in USB port before finishing the separate
// PIO-USB debug wiring (see main for that approach).
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif
#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

// Host stack only — we never present a USB device on this port.
#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 0

// Native silicon USB controller acting as host (no pico-pio-usb, no MAX3421).
#define BOARD_TUH_RHPORT 0
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
