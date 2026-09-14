// WiFi bring-up (STA with saved credentials, AP fallback) and flash-backed credential
// storage, so the bridge is always reachable even before it's ever been configured.
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASS_MAX_LEN + 1];
} wifi_credentials_t;

typedef enum {
    WIFI_MODE_STA,
    WIFI_MODE_AP,
} wifi_mode_t;

// Loads saved STA credentials from flash. Returns false if none are stored (blank/corrupt).
bool wifi_config_load(wifi_credentials_t *out);

// Saves credentials to flash then reboots so the board comes back up in STA mode with
// them (switching a live cyw43 interface from AP to STA is far more fragile than just
// restarting). Does not return.
void wifi_config_apply_and_reboot(const wifi_credentials_t *creds);

// Schedules wifi_config_apply_and_reboot() to run ~delay_ms from now, so an HTTP
// response can actually reach the client before the board restarts. Must be paired
// with periodic wifi_config_poll_pending() calls from the core0 main loop.
void wifi_config_schedule_apply(const wifi_credentials_t *creds, uint32_t delay_ms);
void wifi_config_poll_pending(void);

// Tries STA with saved credentials for a few seconds; on failure or if none are saved,
// falls back to a local open AP ("ScannerRig-xxxx") with a DHCP+DNS server so the device
// is always reachable at http://192.168.4.1/ to configure WiFi via /api/wifi. Call once
// from core0 before starting the HTTP server.
wifi_mode_t wifi_config_bringup(void);

wifi_mode_t wifi_config_current_mode(void);
const char *wifi_config_ap_ssid(void); // valid once bringup() has run in AP mode

#endif
