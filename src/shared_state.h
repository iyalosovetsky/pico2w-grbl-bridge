// Thread-safe state shared between core0 (WiFi/HTTP) and core1 (USB-host/grblHAL link).
//
// Axis count is NOT hardcoded to this rig's specific motors: the turntable (M102-M104)
// and the ST3215 tilt servo (M101) are driven independently of grbl's motion planner in
// this custom grblHAL build (see rotary_table.c / st3215.c) and never appear in the
// standard '<...>' status report. Only whatever grbl axes actually exist (typically the
// scanner's X/Y carriage) show up in MPos/WPos, so we just parse however many the report
// contains, up to GRBL_MAX_AXES.
#ifndef SHARED_STATE_H
#define SHARED_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GRBL_MAX_AXES 6
#define GRBL_STATUS_LEN 16
#define GRBL_ALARM_LEN 80
#define CONSOLE_LOG_LINES 32
#define CONSOLE_LOG_LINE_LEN 96
#define GCODE_QUEUE_DEPTH 32
#define GCODE_LINE_LEN 96

typedef struct {
    bool connected; // USB CDC link to grblHAL currently mounted and enumerated
    char status[GRBL_STATUS_LEN]; // "Idle", "Run", "Hold", "Alarm", "Door", ...
    uint8_t naxes;
    float mpos[GRBL_MAX_AXES];
    float wpos[GRBL_MAX_AXES];
    bool has_wpos;
    float feed;
    float speed;
    // Turntable (M102-M104) and ST3215 tilt servo (M101) state, reported by this rig's
    // grblHAL fork as extra '|TBL:'/'|TBLABS:'/'|ST3215:' status fields since they're
    // driven independently of grbl's own axes (see the naxes comment above).
    bool has_table;
    float table_deg;     // TBL: turntable angle, wrapped to [0,360)
    float table_abs_deg; // TBLABS: turntable angle, unwrapped total rotation
    bool has_servo;
    float servo_deg;     // ST3215: tilt servo angle
    char alarm[GRBL_ALARM_LEN]; // last ALARM:/error: text; cleared by the next user command
    uint32_t last_status_ms;    // board uptime (ms) of the last parsed '?' report
} machine_state_t;

void shared_state_init(void);

// Thread-safe snapshot / update of the machine state.
void shared_state_get(machine_state_t *out);
void shared_state_set_status(const machine_state_t *in);
void shared_state_set_connected(bool connected);
void shared_state_set_alarm(const char *text);  // sets the banner text (ALARM: or error: line)
void shared_state_clear_alarm(void);             // called on any user-initiated command

// Full console log: ring buffer of every raw line sent to and received from grblHAL,
// for the web UI's "full" view.
void shared_state_console_push(const char *line);
// Copies up to max_lines into out[][CONSOLE_LOG_LINE_LEN], oldest first. Returns count copied.
size_t shared_state_console_snapshot(char out[][CONSOLE_LOG_LINE_LEN], size_t max_lines);

// Filtered console log: sent commands and everything received *except* raw '<...>'
// status reports, which only appear here as a synthetic "Old -> New" line when the
// parsed status word actually changes (grbl_link.c's parse_status_report()). Same
// ring-buffer shape/API as the full log, for the web UI's "filtered" view.
void shared_state_filtered_push(const char *line);
size_t shared_state_filtered_snapshot(char out[][CONSOLE_LOG_LINE_LEN], size_t max_lines);

// Outbound G-code line queue: HTTP handlers (core0) push, grbl_link (core1) pops.
bool gcode_queue_push(const char *line);         // false if the queue is full
bool gcode_queue_pop(char *out, size_t out_cap); // false if the queue is empty
size_t gcode_queue_len(void);

// Real-time single-byte requests (hold/resume/soft-reset), coalesced: a second request of
// the same kind before core1 drains the first is a no-op, which matches grbl's real-time
// semantics (each is idempotent, not a counted command).
void shared_state_request_realtime(char which); // '!' | '~' | 0x18
bool shared_state_take_realtime(char *out);      // core1 side: pop one pending request

// LED heartbeat: core1 (grbl_link.c) calls this once per parsed '<...>' status report;
// every 20th call arms a pending pulse that core0 (led.c) consumes to blink the onboard
// LED, as a simple "still talking to grblHAL" indicator.
#define LED_HEARTBEAT_EVERY_N_LINES 20
void shared_state_notify_status_line(void);
bool shared_state_take_led_pulse(void); // core0 side: true once, then clears

#endif
