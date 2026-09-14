#include "shared_state.h"

#include <string.h>

#include "pico/sync.h"
#include "pico/time.h"

static mutex_t state_mutex;
static machine_state_t state;

static mutex_t console_mutex;
static char console_lines[CONSOLE_LOG_LINES][CONSOLE_LOG_LINE_LEN];
static size_t console_head; // index of the oldest line
static size_t console_count;

static mutex_t filtered_mutex;
static char filtered_lines[CONSOLE_LOG_LINES][CONSOLE_LOG_LINE_LEN];
static size_t filtered_head;
static size_t filtered_count;

static mutex_t queue_mutex;
static char gcode_lines[GCODE_QUEUE_DEPTH][GCODE_LINE_LEN];
static size_t queue_head;
static size_t queue_count;

static mutex_t rt_mutex;
static volatile bool rt_hold, rt_resume, rt_reset;

static mutex_t led_mutex;
static uint32_t led_line_count;
static bool led_pulse_pending;

void shared_state_init(void) {
    mutex_init(&state_mutex);
    mutex_init(&console_mutex);
    mutex_init(&filtered_mutex);
    mutex_init(&queue_mutex);
    mutex_init(&rt_mutex);
    mutex_init(&led_mutex);
    memset(&state, 0, sizeof(state));
    strcpy(state.status, "Unknown");
}

void shared_state_get(machine_state_t *out) {
    mutex_enter_blocking(&state_mutex);
    *out = state;
    mutex_exit(&state_mutex);
}

void shared_state_set_status(const machine_state_t *in) {
    mutex_enter_blocking(&state_mutex);
    char alarm_keep[GRBL_ALARM_LEN];
    memcpy(alarm_keep, state.alarm, sizeof(alarm_keep));
    state = *in;
    // The alarm/error banner is cleared by the next user command (shared_state_clear_alarm),
    // not automatically when the machine's status word moves on — grblHAL can leave the
    // Alarm state on its own before the user has actually seen/acted on why it alarmed.
    memcpy(state.alarm, alarm_keep, sizeof(state.alarm));
    state.last_status_ms = to_ms_since_boot(get_absolute_time());
    mutex_exit(&state_mutex);
}

void shared_state_set_connected(bool connected) {
    mutex_enter_blocking(&state_mutex);
    state.connected = connected;
    if (!connected) {
        strcpy(state.status, "Disconnected");
    }
    mutex_exit(&state_mutex);
}

void shared_state_set_alarm(const char *text) {
    mutex_enter_blocking(&state_mutex);
    strncpy(state.alarm, text, sizeof(state.alarm) - 1);
    state.alarm[sizeof(state.alarm) - 1] = '\0';
    mutex_exit(&state_mutex);
}

void shared_state_clear_alarm(void) {
    mutex_enter_blocking(&state_mutex);
    state.alarm[0] = '\0';
    mutex_exit(&state_mutex);
}

void shared_state_console_push(const char *line) {
    mutex_enter_blocking(&console_mutex);
    size_t write_idx = (console_head + console_count) % CONSOLE_LOG_LINES;
    strncpy(console_lines[write_idx], line, CONSOLE_LOG_LINE_LEN - 1);
    console_lines[write_idx][CONSOLE_LOG_LINE_LEN - 1] = '\0';
    if (console_count < CONSOLE_LOG_LINES) {
        console_count++;
    } else {
        console_head = (console_head + 1) % CONSOLE_LOG_LINES;
    }
    mutex_exit(&console_mutex);
}

size_t shared_state_console_snapshot(char out[][CONSOLE_LOG_LINE_LEN], size_t max_lines) {
    mutex_enter_blocking(&console_mutex);
    size_t n = console_count < max_lines ? console_count : max_lines;
    // Return the *newest* n lines, oldest-first within that window.
    size_t skip = console_count - n;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (console_head + skip + i) % CONSOLE_LOG_LINES;
        memcpy(out[i], console_lines[idx], CONSOLE_LOG_LINE_LEN);
    }
    mutex_exit(&console_mutex);
    return n;
}

void shared_state_filtered_push(const char *line) {
    mutex_enter_blocking(&filtered_mutex);
    size_t write_idx = (filtered_head + filtered_count) % CONSOLE_LOG_LINES;
    strncpy(filtered_lines[write_idx], line, CONSOLE_LOG_LINE_LEN - 1);
    filtered_lines[write_idx][CONSOLE_LOG_LINE_LEN - 1] = '\0';
    if (filtered_count < CONSOLE_LOG_LINES) {
        filtered_count++;
    } else {
        filtered_head = (filtered_head + 1) % CONSOLE_LOG_LINES;
    }
    mutex_exit(&filtered_mutex);
}

size_t shared_state_filtered_snapshot(char out[][CONSOLE_LOG_LINE_LEN], size_t max_lines) {
    mutex_enter_blocking(&filtered_mutex);
    size_t n = filtered_count < max_lines ? filtered_count : max_lines;
    size_t skip = filtered_count - n;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (filtered_head + skip + i) % CONSOLE_LOG_LINES;
        memcpy(out[i], filtered_lines[idx], CONSOLE_LOG_LINE_LEN);
    }
    mutex_exit(&filtered_mutex);
    return n;
}

bool gcode_queue_push(const char *line) {
    mutex_enter_blocking(&queue_mutex);
    bool ok = queue_count < GCODE_QUEUE_DEPTH;
    if (ok) {
        size_t write_idx = (queue_head + queue_count) % GCODE_QUEUE_DEPTH;
        strncpy(gcode_lines[write_idx], line, GCODE_LINE_LEN - 1);
        gcode_lines[write_idx][GCODE_LINE_LEN - 1] = '\0';
        queue_count++;
    }
    mutex_exit(&queue_mutex);
    return ok;
}

bool gcode_queue_pop(char *out, size_t out_cap) {
    mutex_enter_blocking(&queue_mutex);
    bool ok = queue_count > 0;
    if (ok) {
        strncpy(out, gcode_lines[queue_head], out_cap - 1);
        out[out_cap - 1] = '\0';
        queue_head = (queue_head + 1) % GCODE_QUEUE_DEPTH;
        queue_count--;
    }
    mutex_exit(&queue_mutex);
    return ok;
}

size_t gcode_queue_len(void) {
    mutex_enter_blocking(&queue_mutex);
    size_t n = queue_count;
    mutex_exit(&queue_mutex);
    return n;
}

void shared_state_request_realtime(char which) {
    mutex_enter_blocking(&rt_mutex);
    if (which == '!') rt_hold = true;
    else if (which == '~') rt_resume = true;
    else if (which == 0x18) rt_reset = true;
    mutex_exit(&rt_mutex);
}

bool shared_state_take_realtime(char *out) {
    mutex_enter_blocking(&rt_mutex);
    bool got = true;
    if (rt_hold) { *out = '!'; rt_hold = false; }
    else if (rt_resume) { *out = '~'; rt_resume = false; }
    else if (rt_reset) { *out = 0x18; rt_reset = false; }
    else got = false;
    mutex_exit(&rt_mutex);
    return got;
}

void shared_state_notify_status_line(void) {
    mutex_enter_blocking(&led_mutex);
    led_line_count++;
    if (led_line_count % LED_HEARTBEAT_EVERY_N_LINES == 0) {
        led_pulse_pending = true;
    }
    mutex_exit(&led_mutex);
}

bool shared_state_take_led_pulse(void) {
    mutex_enter_blocking(&led_mutex);
    bool pending = led_pulse_pending;
    led_pulse_pending = false;
    mutex_exit(&led_mutex);
    return pending;
}
