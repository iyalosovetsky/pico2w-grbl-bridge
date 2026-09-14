// Entry point. Core1 owns the USB-host link to grblHAL exclusively (grbl_link.c),
// here over the board's own native USB controller (see usb_host_cdc.c) via a USB-A OTG
// adapter on its built-in micro-USB port. Core0 owns WiFi + the HTTP/REST server. Debug
// output goes out UART0 since the native USB port is occupied by the host role — see
// CMakeLists.txt's pico_enable_stdio_uart. They only talk through shared_state.c.
#include <stdio.h>

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

    while (true) {
        cyw43_arch_poll();
        wifi_config_poll_pending();
        led_task();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(20));
    }
}
