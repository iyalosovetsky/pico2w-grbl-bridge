#include "led.h"

#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "shared_state.h"

#define BOOT_BLINK_MS 150
#define GOT_IP_FLASH_ON_MS 80
#define GOT_IP_FLASH_OFF_MS 80
#define GOT_IP_FLASH_COUNT 3
#define HEARTBEAT_ON_MS 60

typedef enum {
    PHASE_BOOT,   // fast blink until we have an IP
    PHASE_GOT_IP, // playing the one-shot 3-flash cue
    PHASE_NORMAL, // off, pulses on the grblHAL heartbeat
} led_phase_t;

static led_phase_t phase = PHASE_BOOT;
static bool led_on;
static absolute_time_t next_deadline;
static int got_ip_flashes_done;

static bool heartbeat_active;
static absolute_time_t heartbeat_off_at;

static void set_led(bool on) {
    led_on = on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

void led_init(void) {
    phase = PHASE_BOOT;
    next_deadline = get_absolute_time();
    set_led(false);
}

void led_notify_got_ip(void) {
    if (phase == PHASE_BOOT) {
        phase = PHASE_GOT_IP;
        got_ip_flashes_done = 0;
        set_led(false);
        next_deadline = get_absolute_time();
    }
}

static void task_boot(void) {
    if (time_reached(next_deadline)) {
        set_led(!led_on);
        next_deadline = make_timeout_time_ms(BOOT_BLINK_MS);
    }
}

static void task_got_ip(void) {
    if (!time_reached(next_deadline)) {
        return;
    }
    if (led_on) {
        set_led(false);
        got_ip_flashes_done++;
        if (got_ip_flashes_done >= GOT_IP_FLASH_COUNT) {
            phase = PHASE_NORMAL;
            heartbeat_active = false;
            return;
        }
        next_deadline = make_timeout_time_ms(GOT_IP_FLASH_OFF_MS);
    } else {
        set_led(true);
        next_deadline = make_timeout_time_ms(GOT_IP_FLASH_ON_MS);
    }
}

static void task_normal(void) {
    if (heartbeat_active) {
        if (time_reached(heartbeat_off_at)) {
            set_led(false);
            heartbeat_active = false;
        }
        return;
    }
    if (shared_state_take_led_pulse()) {
        set_led(true);
        heartbeat_active = true;
        heartbeat_off_at = make_timeout_time_ms(HEARTBEAT_ON_MS);
    }
}

void led_task(void) {
    switch (phase) {
        case PHASE_BOOT: task_boot(); break;
        case PHASE_GOT_IP: task_got_ip(); break;
        case PHASE_NORMAL: task_normal(); break;
    }
}
