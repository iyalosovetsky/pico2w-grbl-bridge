#include "grbl_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/flash.h"
#include "pico/time.h"

#include "shared_state.h"
#include "usb_host_cdc.h"

#define STATUS_POLL_INTERVAL_MS 500 // this rig doesn't need finer-grained polling than this
// Generous on purpose: a blocking command (homing, a long dwell) legitimately holds
// off 'ok' until it's done, and we'd rather wait than desync the queue.
#define LINE_TIMEOUT_MS 30000
// How often to remind a live console that nothing has ever attached, in case the
// one-shot message from usb_host_cdc_init() was printed before a terminal connected.
#define NO_DEVICE_REMINDER_MS 10000

static bool waiting_ok;
static absolute_time_t waiting_deadline;
static absolute_time_t next_poll;
static absolute_time_t next_no_device_reminder;
static char last_status_word[GRBL_STATUS_LEN]; // for the filtered log's "Old -> New" lines
// grblHAL (like grbl) doesn't repeat WCO in every status report, only when it changes —
// a report without one still means "same offset as before", not "no offset known", so
// we cache the last one seen and keep computing WPos from it in the meantime.
static float last_wco[GRBL_MAX_AXES];
static int last_wco_count;
static bool have_last_wco;
// Set whenever a command is actually sent (queued G-code line, or a user-triggered
// realtime hold/resume/reset); makes the *next* status report show up in the filtered
// log even if the status word itself didn't change, as a "here's where things stand
// right after that" confirmation.
static bool force_next_status_log;

static int parse_csv_floats(char *s, float *out, int max) {
    int n = 0;
    char *save = NULL;
    char *tok = strtok_r(s, ",", &save);
    while (tok && n < max) {
        out[n++] = strtof(tok, NULL);
        tok = strtok_r(NULL, ",", &save);
    }
    return n;
}

static void parse_status_report(const char *raw) {
    char buf[256];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '>') {
        buf[len - 1] = '\0';
    }

    char *save = NULL;
    char *tok = strtok_r(buf, "|", &save);
    if (!tok) {
        return;
    }

    machine_state_t ns;
    memset(&ns, 0, sizeof(ns));
    ns.connected = true;

    const char *status_name = (tok[0] == '<') ? tok + 1 : tok;
    strncpy(ns.status, status_name, sizeof(ns.status) - 1);

    float mpos[GRBL_MAX_AXES] = {0};
    float wpos[GRBL_MAX_AXES] = {0};
    float wco[GRBL_MAX_AXES] = {0};
    int n_mpos = 0, n_wpos = 0, n_wco = 0;
    bool have_mpos = false, have_wpos = false, have_wco = false;
    float table_deg = 0, table_abs_deg = 0, servo_deg = 0;
    bool have_table = false, have_table_abs = false, have_servo = false;

    tok = strtok_r(NULL, "|", &save);
    while (tok) {
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            const char *key = tok;
            char *val = colon + 1;
            if (strcmp(key, "MPos") == 0) {
                n_mpos = parse_csv_floats(val, mpos, GRBL_MAX_AXES);
                have_mpos = n_mpos > 0;
            } else if (strcmp(key, "WPos") == 0) {
                n_wpos = parse_csv_floats(val, wpos, GRBL_MAX_AXES);
                have_wpos = n_wpos > 0;
            } else if (strcmp(key, "WCO") == 0) {
                n_wco = parse_csv_floats(val, wco, GRBL_MAX_AXES);
                have_wco = n_wco > 0;
            } else if (strcmp(key, "FS") == 0) {
                float fs[2] = {0};
                int nfs = parse_csv_floats(val, fs, 2);
                if (nfs >= 1) ns.feed = fs[0];
                if (nfs >= 2) ns.speed = fs[1];
            } else if (strcmp(key, "TBL") == 0) {
                have_table = parse_csv_floats(val, &table_deg, 1) > 0;
            } else if (strcmp(key, "TBLABS") == 0) {
                have_table_abs = parse_csv_floats(val, &table_abs_deg, 1) > 0;
            } else if (strcmp(key, "ST3215") == 0) {
                have_servo = parse_csv_floats(val, &servo_deg, 1) > 0;
            }
            // Ov/Bf/Pn/Ln and other optional fields are left for a later iteration.
        }
        tok = strtok_r(NULL, "|", &save);
    }

    if (have_mpos) {
        ns.naxes = (uint8_t) n_mpos;
        memcpy(ns.mpos, mpos, sizeof(mpos));
    }
    if (have_wco) {
        memcpy(last_wco, wco, sizeof(wco));
        last_wco_count = n_wco;
        have_last_wco = true;
    }

    if (have_wpos) {
        ns.has_wpos = true;
        memcpy(ns.wpos, wpos, sizeof(wpos));
        if (n_wpos > ns.naxes) ns.naxes = (uint8_t) n_wpos;
    } else if (have_mpos && have_last_wco) {
        // This report may not have repeated WCO — use the last one we saw, per grbl's
        // own protocol (WCO is only sent when it changes, not on every report).
        ns.has_wpos = true;
        int n = n_mpos < last_wco_count ? n_mpos : last_wco_count;
        for (int i = 0; i < n; i++) {
            ns.wpos[i] = mpos[i] - last_wco[i];
        }
    }

    // TBL/TBLABS normally arrive together; still show a wrapped-only reading if TBLABS
    // is ever missing rather than dropping the whole thing.
    if (have_table || have_table_abs) {
        ns.has_table = true;
        ns.table_deg = table_deg;
        ns.table_abs_deg = have_table_abs ? table_abs_deg : table_deg;
    }
    if (have_servo) {
        ns.has_servo = true;
        ns.servo_deg = servo_deg;
    }

    // Filtered log: skip the (frequent) raw status report *most* of the time, but let
    // one through verbatim — not paraphrased, so e.g. a manually sent '?' still shows
    // its real MPos/TBL/ST3215/... — on an actual state transition (Idle -> Run,
    // Run/Jog -> Idle, ...) or, even without one, right after a user command, as a
    // "here's where things stand now" confirmation.
    bool changed = last_status_word[0] && strcmp(last_status_word, ns.status) != 0;
    if (changed || force_next_status_log) {
        shared_state_filtered_push(raw);
    }
    force_next_status_log = false;
    strncpy(last_status_word, ns.status, sizeof(last_status_word) - 1);
    last_status_word[sizeof(last_status_word) - 1] = '\0';

    shared_state_set_status(&ns);
}

static void on_line(const char *line) {
    // Full log: absolutely everything, unfiltered.
    shared_state_console_push(line);

    if (line[0] == '<') {
        // Raw status reports are the one thing the filtered log doesn't want verbatim —
        // parse_status_report() pushes a synthetic "Old -> New" line there on an actual
        // state change instead.
        parse_status_report(line);
        shared_state_notify_status_line(); // drives the onboard LED heartbeat (led.c)
        return;
    }

    // Filtered log: everything else — ok/error/ALARM/messages/banners/$$ dumps.
    shared_state_filtered_push(line);

    if (strcmp(line, "ok") == 0) {
        waiting_ok = false;
        return;
    }
    if (strncmp(line, "error:", 6) == 0) {
        waiting_ok = false;
        shared_state_set_alarm(line);
        return;
    }
    if (strncmp(line, "ALARM:", 6) == 0) {
        shared_state_set_alarm(line);
        return;
    }
    if (strncmp(line, "Grbl", 4) == 0 || strncmp(line, "GrblHAL", 7) == 0) {
        // Startup banner: the controller just (re)booted, nothing is in flight anymore.
        waiting_ok = false;
        return;
    }
}

static void on_mount(bool mounted) {
    shared_state_set_connected(mounted);
    waiting_ok = false;
    shared_state_console_push(mounted ? "[link] grblHAL connected" : "[link] grblHAL disconnected");
}

void grbl_link_core1_main(void) {
    flash_safe_execute_core_init();

    usb_host_cdc_set_line_callback(on_line);
    usb_host_cdc_set_mount_callback(on_mount);
    usb_host_cdc_init();

    next_poll = make_timeout_time_ms(STATUS_POLL_INTERVAL_MS);
    next_no_device_reminder = make_timeout_time_ms(NO_DEVICE_REMINDER_MS);

    while (true) {
        shared_state_core1_tick(); // watchdog liveness signal (main.c) — every pass, no exceptions

        usb_host_cdc_task();

        bool mounted = usb_host_cdc_is_mounted();

        if (!mounted && time_reached(next_no_device_reminder)) {
            printf("[usb] still no device on the USB host port — check the OTG adapter/VBUS wiring\r\n");
            next_no_device_reminder = make_timeout_time_ms(NO_DEVICE_REMINDER_MS);
        }

        char rt;
        if (mounted && shared_state_take_realtime(&rt)) {
            usb_host_cdc_write((const uint8_t *) &rt, 1);
            const char *label = rt == '!' ? "> ! (hold)" : rt == '~' ? "> ~ (resume)" : "> ^X (reset)";
            shared_state_console_push(label);
            shared_state_filtered_push(label);
            force_next_status_log = true;
        }

        if (mounted && time_reached(next_poll)) {
            uint8_t q = '?';
            usb_host_cdc_write(&q, 1);
            next_poll = make_timeout_time_ms(STATUS_POLL_INTERVAL_MS);
        }

        if (waiting_ok && time_reached(waiting_deadline)) {
            shared_state_console_push("[link] timed out waiting for ok, resuming queue");
            waiting_ok = false;
        }

        if (mounted && !waiting_ok) {
            char line[GCODE_LINE_LEN];
            if (gcode_queue_pop(line, sizeof(line))) {
                char out[GCODE_LINE_LEN + 1];
                size_t n = strlen(line);
                memcpy(out, line, n);
                out[n] = '\n';
                if (usb_host_cdc_write((const uint8_t *) out, n + 1)) {
                    char tagged[GCODE_LINE_LEN + 4];
                    snprintf(tagged, sizeof(tagged), "> %s", line);
                    shared_state_console_push(tagged);
                    shared_state_filtered_push(tagged);
                    force_next_status_log = true;
                    waiting_ok = true;
                    waiting_deadline = make_timeout_time_ms(LINE_TIMEOUT_MS);
                }
            }
        }
    }
}
