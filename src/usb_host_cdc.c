#include "usb_host_cdc.h"

#include <stdio.h>
#include <string.h>

#include "pio_usb.h"
#include "tusb.h"

#define LINE_BUF_LEN 256

// D+ pin for the PIO-USB host port; D- is D+ + 1 (PIO_USB_PINOUT_DPDM, the library
// default). Wire a full USB-A host connector here (D+, D-, 5V, GND) to the SKR-Pico's
// USB port — see README.md. Deliberately NOT the native USB port, which stays free for
// flashing/debug (stdio-over-CDC, driven by main.c's tud_init/tud_task).
// Overridable without editing source: cmake -DPIO_USB_HOST_DP_PIN=<gpio> (see
// CMakeLists.txt / tools/build.sh --dp-pin).
#ifndef PIO_USB_HOST_DP_PIN
#define PIO_USB_HOST_DP_PIN 0
#endif

// Which PIO block (0/1/2) the PIO-USB host claims (both its TX and RX state machines),
// away from whatever block the CYW43 WiFi driver's SPI-over-PIO claims dynamically at
// cyw43_arch_init() — without this both would default to PIO0 and could collide.
// Overridable: cmake -DPIO_USB_HOST_PIO_INDEX=<0|1|2> (see tools/build.sh --pio).
#ifndef PIO_USB_HOST_PIO_INDEX
#define PIO_USB_HOST_PIO_INDEX 1
#endif

static uint8_t mounted_idx = 0xFF; // 0xFF = none mounted
static char line_buf[LINE_BUF_LEN];
static size_t line_len;

static usb_host_cdc_line_cb_t line_cb;
static usb_host_cdc_mount_cb_t mount_cb;

void usb_host_cdc_init(void) {
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIO_USB_HOST_DP_PIN;
    pio_cfg.pio_tx_num = PIO_USB_HOST_PIO_INDEX;
    pio_cfg.pio_rx_num = PIO_USB_HOST_PIO_INDEX;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(BOARD_TUH_RHPORT);
    printf("[usb] PIO-USB host ready: D+=GP%d D-=GP%d PIO%d — waiting for a device"
           " (nothing here ever means check VBUS wiring, see README)\r\n",
           PIO_USB_HOST_DP_PIN, PIO_USB_HOST_DP_PIN + 1, PIO_USB_HOST_PIO_INDEX);
}

void usb_host_cdc_task(void) {
    tuh_task();
}

bool usb_host_cdc_is_mounted(void) {
    return mounted_idx != 0xFF && tuh_cdc_mounted(mounted_idx);
}

bool usb_host_cdc_write(const uint8_t *data, size_t len) {
    if (!usb_host_cdc_is_mounted()) {
        return false;
    }
    uint32_t written = tuh_cdc_write(mounted_idx, data, len);
    tuh_cdc_write_flush(mounted_idx);
    return written == len;
}

void usb_host_cdc_set_line_callback(usb_host_cdc_line_cb_t cb) {
    line_cb = cb;
}

void usb_host_cdc_set_mount_callback(usb_host_cdc_mount_cb_t cb) {
    mount_cb = cb;
}

// ---- Generic TinyUSB host callbacks (any device, before/regardless of class) ----
// Firing here but never followed by "[usb] CDC mounted" below means something enumerated
// but wasn't recognized as a CDC-ACM device; never firing at all means the PIO-USB host
// never saw anything electrically attach — almost always the VBUS wiring, not this.

void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    printf("[usb] device attached: addr=%u vid=%04x pid=%04x\r\n", daddr, vid, pid);
}

void tuh_umount_cb(uint8_t daddr) {
    printf("[usb] device detached: addr=%u\r\n", daddr);
}

// ---- TinyUSB host CDC callbacks ----

void tuh_cdc_mount_cb(uint8_t idx) {
    tuh_itf_info_t itf_info = {0};
    tuh_cdc_itf_get_info(idx, &itf_info);
    printf("[usb] CDC mounted: idx=%u addr=%u itf=%u\r\n", idx, itf_info.daddr,
           itf_info.desc.bInterfaceNumber);

    // Only track the first CDC interface we see; a single grblHAL board is expected.
    if (mounted_idx == 0xFF) {
        mounted_idx = idx;
        line_len = 0;
        if (mount_cb) mount_cb(true);
    }
}

void tuh_cdc_umount_cb(uint8_t idx) {
    printf("[usb] CDC unmounted: idx=%u\r\n", idx);
    if (idx == mounted_idx) {
        mounted_idx = 0xFF;
        line_len = 0;
        if (mount_cb) mount_cb(false);
    }
}

void tuh_cdc_rx_cb(uint8_t idx) {
    if (idx != mounted_idx) {
        return;
    }
    uint8_t buf[64];
    uint32_t count;
    while ((count = tuh_cdc_read(idx, buf, sizeof(buf))) > 0) {
        for (uint32_t i = 0; i < count; i++) {
            char c = (char) buf[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line_buf[line_len] = '\0';
                if (line_len > 0 && line_cb) {
                    line_cb(line_buf);
                }
                line_len = 0;
                continue;
            }
            if (line_len < LINE_BUF_LEN - 1) {
                line_buf[line_len++] = c;
            } else {
                // Line too long (e.g. a $$ settings dump line) — flush what we have
                // rather than silently dropping it.
                line_buf[line_len] = '\0';
                if (line_cb) line_cb(line_buf);
                line_len = 0;
            }
        }
    }
}
