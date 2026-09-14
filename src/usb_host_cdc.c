#include "usb_host_cdc.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"

#define LINE_BUF_LEN 256

static uint8_t mounted_idx = 0xFF; // 0xFF = none mounted
static char line_buf[LINE_BUF_LEN];
static size_t line_len;

static usb_host_cdc_line_cb_t line_cb;
static usb_host_cdc_mount_cb_t mount_cb;

void usb_host_cdc_init(void) {
    tuh_init(BOARD_TUH_RHPORT);
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
