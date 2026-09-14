#include "http_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/time.h"

#include "api_handlers.h"

#define REQ_BUF_SIZE 4096
#define RESP_BUF_SIZE 6144
#define POLL_INTERVAL_S 5
#define HEADERS_TIMEOUT_US (10 * 1000 * 1000)

typedef struct {
    struct tcp_pcb *pcb;
    absolute_time_t created;

    char req[REQ_BUF_SIZE];
    size_t req_len;
    bool headers_complete;
    size_t header_end;  // offset of the first body byte
    int content_length; // -1 until parsed from headers
    bool oversized;

    char resp_buf[RESP_BUF_SIZE]; // scratch JSON/body buffer, owned by this connection
    const char *resp_body;        // points into resp_buf, or into static flash data
    char resp_head[192];
    size_t head_len, body_len, sent_len;
    bool response_ready;
} http_conn_t;

static err_t close_conn(http_conn_t *c, struct tcp_pcb *pcb, err_t err) {
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_err(pcb, NULL);
        err_t e = tcp_close(pcb);
        if (e != ERR_OK) {
            tcp_abort(pcb);
            err = ERR_ABRT;
        }
    }
    free(c);
    return err;
}

// Case-insensitive substring search over the first hay_len bytes of hay.
static size_t case_find(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) return (size_t) -1;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            if (tolower((unsigned char) hay[i + j]) != tolower((unsigned char) needle[j])) break;
        }
        if (j == nlen) return i;
    }
    return (size_t) -1;
}

static void send_more(http_conn_t *c) {
    size_t total = c->head_len + c->body_len;
    while (c->sent_len < total) {
        u16_t avail = tcp_sndbuf(c->pcb);
        if (avail == 0) break;
        size_t remaining = total - c->sent_len;
        size_t chunk = remaining < avail ? remaining : avail;

        const char *src;
        if (c->sent_len < c->head_len) {
            size_t head_remaining = c->head_len - c->sent_len;
            if (chunk > head_remaining) chunk = head_remaining;
            src = c->resp_head + c->sent_len;
        } else {
            src = c->resp_body + (c->sent_len - c->head_len);
        }

        if (tcp_write(c->pcb, src, (u16_t) chunk, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        c->sent_len += chunk;
    }
    tcp_output(c->pcb);
}

static void begin_response(http_conn_t *c, int status, const char *status_text,
                            const char *content_type, const char *body, size_t body_len) {
    c->resp_body = body;
    c->body_len = body_len;
    c->head_len = snprintf(c->resp_head, sizeof(c->resp_head),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
        status, status_text, content_type, (unsigned) body_len);
    c->response_ready = true;
    c->sent_len = 0;
    send_more(c);
}

static void build_and_send(http_conn_t *c) {
    char method[8] = {0};
    char path[160] = {0};

    size_t i = 0;
    while (i < c->header_end && c->req[i] != ' ' && i < sizeof(method) - 1) {
        method[i] = c->req[i];
        i++;
    }
    while (i < c->header_end && c->req[i] != ' ') i++;
    while (i < c->header_end && c->req[i] == ' ') i++;
    size_t pstart = i;
    while (i < c->header_end && c->req[i] != ' ' && (i - pstart) < sizeof(path) - 1) {
        path[i - pstart] = c->req[i];
        i++;
    }
    char *qmark = strchr(path, '?');
    if (qmark) *qmark = '\0';

    const char *body = c->req + c->header_end;
    size_t body_len = c->content_length > 0 ? (size_t) c->content_length : 0;

    http_response_t resp = {0};
    api_dispatch(method, path, body, body_len, c->resp_buf, sizeof(c->resp_buf), &resp);
    begin_response(c, resp.status, resp.status_text, resp.content_type, resp.body, resp.body_len);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void) len;
    http_conn_t *c = (http_conn_t *) arg;
    if (c->sent_len >= c->head_len + c->body_len) {
        return close_conn(c, pcb, ERR_OK);
    }
    send_more(c);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void) err;
    http_conn_t *c = (http_conn_t *) arg;
    if (!p) {
        return close_conn(c, pcb, ERR_OK);
    }

    if (p->tot_len > 0) {
        size_t space = REQ_BUF_SIZE - 1 - c->req_len;
        size_t copy_len = p->tot_len < space ? p->tot_len : space;
        if (copy_len > 0) {
            pbuf_copy_partial(p, c->req + c->req_len, (u16_t) copy_len, 0);
            c->req_len += copy_len;
            c->req[c->req_len] = '\0';
        }
        if (p->tot_len > space) {
            c->oversized = true;
        }
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);

    if (!c->headers_complete) {
        size_t hdr_pos = case_find(c->req, c->req_len, "\r\n\r\n");
        if (hdr_pos != (size_t) -1) {
            c->headers_complete = true;
            c->header_end = hdr_pos + 4;
            c->content_length = 0;
            size_t cl_pos = case_find(c->req, c->header_end, "\r\nContent-Length:");
            if (cl_pos != (size_t) -1) {
                c->content_length = atoi(c->req + cl_pos + strlen("\r\nContent-Length:"));
                if (c->content_length < 0) c->content_length = 0;
            }
            if (c->header_end + (size_t) c->content_length > REQ_BUF_SIZE - 1) {
                c->oversized = true;
            }
        }
    }

    if (!c->response_ready && c->oversized) {
        begin_response(c, 413, "Payload Too Large", "text/plain", "request too large", 17);
        return ERR_OK;
    }

    if (!c->response_ready && c->headers_complete) {
        size_t have = c->req_len > c->header_end ? c->req_len - c->header_end : 0;
        if (have >= (size_t) c->content_length) {
            build_and_send(c);
        }
    }

    return ERR_OK;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    http_conn_t *c = (http_conn_t *) arg;
    if (c->response_ready) {
        send_more(c); // recover from an earlier ERR_MEM on tcp_write
        return ERR_OK;
    }
    if (absolute_time_diff_us(c->created, get_absolute_time()) > HEADERS_TIMEOUT_US) {
        return close_conn(c, pcb, ERR_OK);
    }
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void) err;
    // Per lwIP's contract the pcb is already invalid/freed by the time this fires.
    free(arg);
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void) arg;
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    http_conn_t *c = calloc(1, sizeof(http_conn_t));
    if (!c) {
        return ERR_MEM;
    }
    c->pcb = pcb;
    c->created = get_absolute_time();
    c->content_length = -1;

    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_poll(pcb, on_poll, POLL_INTERVAL_S * 2);
    tcp_err(pcb, on_err);
    return ERR_OK;
}

bool http_server_start(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, 80) != ERR_OK) {
        tcp_close(pcb);
        return false;
    }
    struct tcp_pcb *listen_pcb = tcp_listen_with_backlog(pcb, 8);
    if (!listen_pcb) {
        tcp_close(pcb);
        return false;
    }
    tcp_accept(listen_pcb, on_accept);
    return true;
}
