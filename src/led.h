// Onboard-LED status signaling — the only feedback available without a console attached.
// All calls must come from core0 (the LED is wired to the CYW43 chip on Pico W/2 W, not
// a plain GPIO, so it shares state with the WiFi driver core0 already owns).
//
//   - Booting / no IP yet: fast blink.
//   - Just got an IP (STA connected, or the AP came up): three short flashes, once.
//   - Afterwards: off, with a brief pulse every LED_HEARTBEAT_EVERY_N_LINES-th grblHAL
//     status report (shared_state_notify_status_line(), called from grbl_link.c) — a
//     "still talking to grblHAL" heartbeat.
#ifndef LED_H
#define LED_H

// Call once, right after cyw43_arch_init() succeeds.
void led_init(void);

// Call once, after WiFi bring-up (STA connected or AP started) — starts the 3-flash cue.
void led_notify_got_ip(void);

// Call on every core0 main-loop iteration; non-blocking.
void led_task(void);

#endif
