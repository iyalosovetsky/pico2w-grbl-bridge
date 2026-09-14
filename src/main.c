// Entry point. Core1 owns the USB-host link to grblHAL exclusively (grbl_link.c);
// core0 owns WiFi + the HTTP/REST server, plus the native USB device stack (used only
// for stdio-over-CDC debugging — see usb_host_cdc.c for why grblHAL itself is talked to
// over a separate PIO-USB host port instead). They only talk through shared_state.c.
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "grbl_link.h"
#include "http_server.h"
#include "shared_state.h"
#include "wifi_config.h"

int main(void) {
    // Pico-PIO-USB requires the system clock to be an exact multiple of 12 MHz; the
    // default (125/150 MHz depending on chip) isn't. Must happen before any USB/clock
    // dependent init.
    set_sys_clock_khz(120000, true);

    // Native USB device stack (stdio-over-CDC). Since we also link tinyusb_host (for
    // the PIO-USB link on core1), pico-sdk's stdio_usb no longer auto-inits or
    // auto-pumps TinyUSB for us — we own tud_init()/tud_task() explicitly.
    tud_init(0);
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
        tud_task();
        cyw43_arch_poll();
        wifi_config_poll_pending();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(20));
    }
}
