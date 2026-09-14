// USB-host CDC-ACM link to grblHAL's console on the SKR-Pico.
// Runs entirely on core1 (tuh_task() must be pumped by usb_host_cdc_task()).
#ifndef USB_HOST_CDC_H
#define USB_HOST_CDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void usb_host_cdc_init(void);

// Drives tuh_task() and line assembly. Call in a tight loop from core1.
void usb_host_cdc_task(void);

bool usb_host_cdc_is_mounted(void);

// Writes raw bytes to grblHAL (a queued "line\n", or a single real-time byte).
// Returns false if no CDC device is currently mounted.
bool usb_host_cdc_write(const uint8_t *data, size_t len);

// Invoked (from usb_host_cdc_task()'s calling context) once per complete line
// ('\n'-terminated, CR/LF stripped) received from grblHAL.
typedef void (*usb_host_cdc_line_cb_t)(const char *line);
void usb_host_cdc_set_line_callback(usb_host_cdc_line_cb_t cb);

// Invoked when the CDC device mounts/unmounts (grblHAL board plugged in/out).
typedef void (*usb_host_cdc_mount_cb_t)(bool mounted);
void usb_host_cdc_set_mount_callback(usb_host_cdc_mount_cb_t cb);

#endif
