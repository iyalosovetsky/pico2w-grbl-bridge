#include "api_handlers.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "generated/web_page.h"
#include "grbl_link.h"
#include "shared_state.h"
#include "wifi_config.h"

// Only the newest few console lines go into every /api/status poll — keep the response
// small since the web page re-fetches it twice a second over WiFi.
#define STATUS_CONSOLE_LINES 12

static size_t json_escape_append(char *out, size_t out_cap, size_t pos, const char *s) {
    for (; *s && pos + 2 < out_cap; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') {
            out[pos++] = '\\';
            out[pos++] = (char) c;
        } else if (c < 0x20) {
            // Drop stray control characters rather than emit invalid JSON.
        } else {
            out[pos++] = (char) c;
        }
    }
    return pos;
}

static void handle_status(char *out, size_t cap, http_response_t *resp) {
    machine_state_t st;
    shared_state_get(&st);

    char console[STATUS_CONSOLE_LINES][CONSOLE_LOG_LINE_LEN];
    size_t n_console = shared_state_console_snapshot(console, STATUS_CONSOLE_LINES);

    size_t pos = 0;
    pos += snprintf(out + pos, cap - pos,
                     "{\"connected\":%s,\"status\":\"%s\",\"naxes\":%u,\"mpos\":[",
                     st.connected ? "true" : "false", st.status, st.naxes);
    for (uint8_t i = 0; i < st.naxes && i < GRBL_MAX_AXES; i++) {
        pos += snprintf(out + pos, cap - pos, "%s%.3f", i ? "," : "", (double) st.mpos[i]);
    }
    pos += snprintf(out + pos, cap - pos, "],\"has_wpos\":%s,\"wpos\":[",
                     st.has_wpos ? "true" : "false");
    for (uint8_t i = 0; i < st.naxes && i < GRBL_MAX_AXES; i++) {
        pos += snprintf(out + pos, cap - pos, "%s%.3f", i ? "," : "", (double) st.wpos[i]);
    }
    pos += snprintf(out + pos, cap - pos, "],\"feed\":%.3f,\"speed\":%.3f,\"alarm\":",
                     (double) st.feed, (double) st.speed);
    if (st.alarm[0]) {
        pos += snprintf(out + pos, cap - pos, "\"");
        pos = json_escape_append(out, cap, pos, st.alarm);
        pos += snprintf(out + pos, cap - pos, "\"");
    } else {
        pos += snprintf(out + pos, cap - pos, "null");
    }

    wifi_mode_t mode = wifi_config_current_mode();
    pos += snprintf(out + pos, cap - pos, ",\"wifi_mode\":\"%s\",\"ap_ssid\":\"%s\",\"console\":[",
                     mode == WIFI_MODE_AP ? "ap" : "sta", wifi_config_ap_ssid());
    for (size_t i = 0; i < n_console; i++) {
        pos += snprintf(out + pos, cap - pos, "%s\"", i ? "," : "");
        pos = json_escape_append(out, cap, pos, console[i]);
        pos += snprintf(out + pos, cap - pos, "\"");
    }
    pos += snprintf(out + pos, cap - pos, "]}");

    resp->status = 200;
    resp->status_text = "OK";
    resp->content_type = "application/json";
    resp->body = out;
    resp->body_len = pos < cap ? pos : cap - 1;
}

// Body is plain text, one G-code/console command per line (blank lines ignored) —
// simplest thing a curl one-liner or the bundled web page can produce.
static void handle_gcode_post(const char *body, size_t body_len, char *out, size_t cap,
                               http_response_t *resp) {
    int queued = 0, rejected = 0, total = 0;
    size_t i = 0;
    while (i < body_len) {
        size_t start = i;
        while (i < body_len && body[i] != '\n') i++;
        size_t end = i;
        while (end > start && (body[end - 1] == '\r' || isspace((unsigned char) body[end - 1]))) end--;
        while (start < end && isspace((unsigned char) body[start])) start++;
        if (i < body_len) i++; // skip the '\n'

        if (end > start) {
            total++;
            char line[GCODE_LINE_LEN];
            size_t len = end - start;
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, body + start, len);
            line[len] = '\0';
            if (gcode_queue_push(line)) queued++; else rejected++;
        }
    }

    size_t pos = snprintf(out, cap, "{\"total\":%d,\"queued\":%d,\"rejected\":%d}", total, queued, rejected);
    resp->status = (total > 0 && queued == 0) ? 503 : 200;
    resp->status_text = resp->status == 200 ? "OK" : "Service Unavailable";
    resp->content_type = "application/json";
    resp->body = out;
    resp->body_len = pos < cap ? pos : cap - 1;
}

static void handle_realtime(char which, char *out, size_t cap, http_response_t *resp) {
    shared_state_request_realtime(which);
    size_t pos = snprintf(out, cap, "{\"ok\":true}");
    resp->status = 200;
    resp->status_text = "OK";
    resp->content_type = "application/json";
    resp->body = out;
    resp->body_len = pos;
}

// Body: SSID on the first line, password (if any) on the second — simple and easy to
// produce from the AP-mode config page's plain <form>, no JSON parser needed.
static void handle_wifi_post(const char *body, size_t body_len, char *out, size_t cap,
                              http_response_t *resp) {
    wifi_credentials_t creds;
    memset(&creds, 0, sizeof(creds));

    size_t nl = 0;
    while (nl < body_len && body[nl] != '\n') nl++;
    size_t ssid_len = nl;
    while (ssid_len > 0 && (body[ssid_len - 1] == '\r' || isspace((unsigned char) body[ssid_len - 1]))) ssid_len--;
    if (ssid_len > WIFI_SSID_MAX_LEN) ssid_len = WIFI_SSID_MAX_LEN;
    memcpy(creds.ssid, body, ssid_len);

    if (nl < body_len) {
        size_t pw_start = nl + 1;
        size_t pw_end = body_len;
        while (pw_end > pw_start && (body[pw_end - 1] == '\r' || body[pw_end - 1] == '\n')) pw_end--;
        size_t pw_len = pw_end - pw_start;
        if (pw_len > WIFI_PASS_MAX_LEN) pw_len = WIFI_PASS_MAX_LEN;
        memcpy(creds.password, body + pw_start, pw_len);
    }

    if (creds.ssid[0] == '\0') {
        resp->status = 400;
        resp->status_text = "Bad Request";
        resp->content_type = "application/json";
        resp->body = out;
        resp->body_len = snprintf(out, cap, "{\"error\":\"ssid required\"}");
        return;
    }

    wifi_config_schedule_apply(&creds, 800);

    resp->status = 200;
    resp->status_text = "OK";
    resp->content_type = "application/json";
    resp->body = out;
    resp->body_len = snprintf(out, cap, "{\"ok\":true,\"rebooting\":true}");
}

static void handle_wifi_get(char *out, size_t cap, http_response_t *resp) {
    wifi_mode_t mode = wifi_config_current_mode();
    size_t pos = snprintf(out, cap, "{\"mode\":\"%s\",\"ap_ssid\":\"%s\"}",
                           mode == WIFI_MODE_AP ? "ap" : "sta", wifi_config_ap_ssid());
    resp->status = 200;
    resp->status_text = "OK";
    resp->content_type = "application/json";
    resp->body = out;
    resp->body_len = pos;
}

static void not_found(char *out, size_t cap, http_response_t *resp) {
    resp->status = 404;
    resp->status_text = "Not Found";
    resp->content_type = "application/json";
    resp->body = out;
    resp->body_len = snprintf(out, cap, "{\"error\":\"not found\"}");
}

void api_dispatch(const char *method, const char *path, const char *body, size_t body_len,
                   char *out_buf, size_t out_cap, http_response_t *resp) {
    bool is_get = strcmp(method, "GET") == 0;
    bool is_post = strcmp(method, "POST") == 0;

    if (is_get && strcmp(path, "/") == 0) {
        resp->status = 200;
        resp->status_text = "OK";
        resp->content_type = "text/html; charset=utf-8";
        resp->body = (const char *) index_html;
        resp->body_len = index_html_len;
        return;
    }
    if (is_get && strcmp(path, "/api/status") == 0) {
        handle_status(out_buf, out_cap, resp);
        return;
    }
    if (is_post && strcmp(path, "/api/gcode") == 0) {
        handle_gcode_post(body, body_len, out_buf, out_cap, resp);
        return;
    }
    if (is_post && strcmp(path, "/api/hold") == 0) {
        handle_realtime('!', out_buf, out_cap, resp);
        return;
    }
    if (is_post && strcmp(path, "/api/resume") == 0) {
        handle_realtime('~', out_buf, out_cap, resp);
        return;
    }
    if (is_post && strcmp(path, "/api/reset") == 0) {
        handle_realtime(0x18, out_buf, out_cap, resp);
        return;
    }
    if (is_get && strcmp(path, "/api/wifi") == 0) {
        handle_wifi_get(out_buf, out_cap, resp);
        return;
    }
    if (is_post && strcmp(path, "/api/wifi") == 0) {
        handle_wifi_post(body, body_len, out_buf, out_cap, resp);
        return;
    }

    not_found(out_buf, out_cap, resp);
}
