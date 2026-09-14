// REST endpoint implementations, called by http_server.c once a full request has been
// received. Builds JSON responses into a buffer owned by the caller (the connection's
// own buffer, so concurrent connections never share mutable state).
#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include <stddef.h>

#include "http_server.h"

void api_dispatch(const char *method, const char *path, const char *body, size_t body_len,
                   char *out_buf, size_t out_cap, http_response_t *resp);

#endif
