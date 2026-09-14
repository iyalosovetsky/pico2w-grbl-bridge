#include "wifi_config.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "dhcpserver.h"
#include "dnsserver.h"

#define WIFI_CFG_MAGIC 0x57494669u // "WiFi"
#define WIFI_CFG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define STA_CONNECT_TIMEOUT_MS 15000

typedef struct {
    uint32_t magic;
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASS_MAX_LEN + 1];
    uint32_t crc32;
} wifi_cfg_record_t;

static wifi_mode_t current_mode = WIFI_MODE_AP;
static char ap_ssid[32];
static dhcp_server_t dhcp_server;
static dns_server_t dns_server;

static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool wifi_config_load(wifi_credentials_t *out) {
    const wifi_cfg_record_t *rec = (const wifi_cfg_record_t *) (XIP_BASE + WIFI_CFG_FLASH_OFFSET);
    if (rec->magic != WIFI_CFG_MAGIC) {
        return false;
    }
    uint32_t crc = crc32_calc((const uint8_t *) rec->ssid, sizeof(rec->ssid) + sizeof(rec->password));
    if (crc != rec->crc32) {
        return false;
    }
    strncpy(out->ssid, rec->ssid, sizeof(out->ssid) - 1);
    out->ssid[sizeof(out->ssid) - 1] = '\0';
    strncpy(out->password, rec->password, sizeof(out->password) - 1);
    out->password[sizeof(out->password) - 1] = '\0';
    return out->ssid[0] != '\0';
}

static void do_flash_write(void *param) {
    const uint8_t *page = (const uint8_t *) param;
    flash_range_erase(WIFI_CFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WIFI_CFG_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
}

static bool wifi_config_save(const wifi_credentials_t *creds) {
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0, sizeof(page));

    wifi_cfg_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = WIFI_CFG_MAGIC;
    strncpy(rec.ssid, creds->ssid, sizeof(rec.ssid) - 1);
    strncpy(rec.password, creds->password, sizeof(rec.password) - 1);
    rec.crc32 = crc32_calc((const uint8_t *) rec.ssid, sizeof(rec.ssid) + sizeof(rec.password));

    _Static_assert(sizeof(wifi_cfg_record_t) <= FLASH_PAGE_SIZE, "wifi cfg record must fit in one flash page");
    memcpy(page, &rec, sizeof(rec));

    int rc = flash_safe_execute(do_flash_write, page, 1000);
    return rc == PICO_OK;
}

void wifi_config_apply_and_reboot(const wifi_credentials_t *creds) {
    wifi_config_save(creds);
    sleep_ms(200);
    watchdog_reboot(0, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}

static void start_ap_mode(void) {
    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
    size_t idlen = strlen(id);
    snprintf(ap_ssid, sizeof(ap_ssid), "ScannerRig-%s", idlen >= 4 ? id + idlen - 4 : id);

    cyw43_arch_enable_ap_mode(ap_ssid, NULL, CYW43_AUTH_OPEN);

    ip4_addr_t gw, mask;
    IP4_ADDR(&gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    dhcp_server_init(&dhcp_server, &gw, &mask);
    dns_server_init(&dns_server, &gw);

    current_mode = WIFI_MODE_AP;
    printf("[wifi] AP mode: SSID '%s' (open), http://192.168.4.1/\n", ap_ssid);
}

wifi_mode_t wifi_config_bringup(void) {
    wifi_credentials_t creds;
    if (wifi_config_load(&creds)) {
        cyw43_arch_enable_sta_mode();
        printf("[wifi] Connecting to '%s'...\n", creds.ssid);
        int rc = cyw43_arch_wifi_connect_timeout_ms(creds.ssid, creds.password,
                                                      CYW43_AUTH_WPA2_AES_PSK, STA_CONNECT_TIMEOUT_MS);
        if (rc == 0) {
            current_mode = WIFI_MODE_STA;
            printf("[wifi] Connected, IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
            return current_mode;
        }
        printf("[wifi] STA connect to '%s' failed (%d), falling back to AP mode\n", creds.ssid, rc);
    } else {
        printf("[wifi] No saved credentials, starting in AP mode\n");
    }
    start_ap_mode();
    return current_mode;
}

wifi_mode_t wifi_config_current_mode(void) {
    return current_mode;
}

const char *wifi_config_ap_ssid(void) {
    return ap_ssid;
}

static bool pending_apply;
static wifi_credentials_t pending_creds;
static absolute_time_t pending_deadline;

void wifi_config_schedule_apply(const wifi_credentials_t *creds, uint32_t delay_ms) {
    pending_creds = *creds;
    pending_deadline = make_timeout_time_ms(delay_ms);
    pending_apply = true;
}

void wifi_config_poll_pending(void) {
    if (pending_apply && time_reached(pending_deadline)) {
        pending_apply = false;
        wifi_config_apply_and_reboot(&pending_creds);
    }
}
