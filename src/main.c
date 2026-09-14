// Entry point. Core1 owns the USB-host link to grblHAL exclusively (grbl_link.c);
// core0 owns WiFi + the HTTP/REST server. They only talk through shared_state.c.
#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "grbl_link.h"
#include "http_server.h"
#include "shared_state.h"
#include "wifi_config.h"

int main(void) {
    stdio_init_all();
    shared_state_init();

    multicore_launch_core1(grbl_link_core1_main);

    if (cyw43_arch_init()) {
        printf("[main] cyw43_arch_init failed\n");
        return 1;
    }

    wifi_config_bringup();

    if (!http_server_start()) {
        printf("[main] failed to start HTTP server\n");
    } else {
        printf("[main] HTTP server listening on port 80\n");
    }

    while (true) {
        cyw43_arch_poll();
        wifi_config_poll_pending();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(50));
    }
}
