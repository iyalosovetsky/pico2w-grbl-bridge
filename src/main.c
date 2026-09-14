// Entry point. Core1 owns the USB-host link to grblHAL exclusively (grbl_link.c),
// here over the board's own native USB controller (see usb_host_cdc.c) via a USB-A OTG
// adapter on its built-in micro-USB port. Core0 owns WiFi + the HTTP/REST server. Debug
// output goes out UART0 since the native USB port is occupied by the host role — see
// CMakeLists.txt's pico_enable_stdio_uart. They only talk through shared_state.c.
#include <stdio.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "generated/build_info.h"
#include "grbl_link.h"
#include "http_server.h"
#include "led.h"
#include "shared_state.h"
#include "wifi_config.h"

// RP2040's hardware watchdog maxes out at ~8388ms (RP2350: ~16777ms) — 8000 is safe on
// both boards this firmware targets.
#define WATCHDOG_TIMEOUT_MS 8000
// If core1 hasn't ticked in this long, treat it as stalled (wedged USB/TinyUSB state
// from e.g. a bad cable) and stop feeding the watchdog on its behalf.
#define CORE1_STALL_TIMEOUT_MS 3000

static void print_banner(void) {
    printf("\n========================================\n");
    printf(" Scanner Rig Bridge\n");
    printf(" Built: %s (build #%d, %s%s)\n", BUILD_TIMESTAMP, BUILD_NUMBER, BUILD_GIT_HASH,
           BUILD_GIT_DIRTY ? "-dirty" : "");
    printf("========================================\n");
}

int main(void) {
    stdio_init_all();
    print_banner();

    if (watchdog_enable_caused_reboot()) {
        printf("[main] recovered from a watchdog reset (core1/USB link had stalled)\n");
    }

    shared_state_init();

    multicore_launch_core1(grbl_link_core1_main);

    if (cyw43_arch_init()) {
        printf("[main] cyw43_arch_init failed\n");
        return 1;
    }
    led_init(); // onboard LED lives on the CYW43 chip — only controllable after this

    wifi_config_bringup();
    led_notify_got_ip(); // both STA and AP mode have a usable IP by the time this returns

    if (!http_server_start()) {
        printf("[main] failed to start HTTP server\n");
    } else {
        printf("[main] HTTP server listening on port 80\n");
    }

    printf("[main] Ready — mode: %s  ip: %s  http://%s/\n",
           wifi_config_current_mode() == WIFI_MODE_STA ? "STA" : "AP",
           wifi_config_ip_str(), wifi_config_ip_str());

    // Armed here, not earlier: wifi_config_bringup() above can legitimately block for
    // up to 15s trying to join a network, far longer than the watchdog's hardware max.
    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    uint32_t last_core1_tick = shared_state_core1_tick_count();
    absolute_time_t last_core1_progress = get_absolute_time();
    bool core1_flagged_stalled = false;

    while (true) {
        cyw43_arch_poll();
        wifi_config_poll_pending();
        led_task();

        uint32_t tick = shared_state_core1_tick_count();
        if (tick != last_core1_tick) {
            last_core1_tick = tick;
            last_core1_progress = get_absolute_time();
            core1_flagged_stalled = false;
        }

        if (absolute_time_diff_us(last_core1_progress, get_absolute_time()) <
            (int64_t) CORE1_STALL_TIMEOUT_MS * 1000) {
            // Both cores are making progress — safe to feed. If core0 itself wedges
            // somewhere, this call simply stops happening and the watchdog resets the
            // board on its own, same as the deliberate core1-stall case below.
            watchdog_update();
        } else if (!core1_flagged_stalled) {
            core1_flagged_stalled = true;
            shared_state_set_connected(false);
            printf("[main] core1 (USB/grblHAL link) appears stalled — no longer feeding"
                   " the watchdog, expecting a reset\n");
        }

        cyw43_arch_wait_for_work_until(make_timeout_time_ms(20));
    }
}
