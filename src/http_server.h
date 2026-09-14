// Minimal non-blocking HTTP/1.0 server on top of lwIP's raw TCP API (no RTOS, no
// sockets — matches the pico-examples pico_w/wifi pattern). One-shot request/response
// per connection (Connection: close), which is all a small local control UI needs.
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int status;              // e.g. 200, 404
    const char *status_text; // e.g. "OK"
    const char *content_type;
    const char *body;        // must stay valid until the response is fully sent
    size_t body_len;
} http_response_t;

// Starts listening on port 80. Call once from core0 after WiFi is up.
bool http_server_start(void);

#endif
