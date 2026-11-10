#include "api.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"

#include "frame.h"
#include "net.h"
#include "panel.h"

static const char *TAG = "api";

#define FW_VERSION "0.1.0-spike"

/* Whole-body deadline. A frame takes ~1 s at USB Full Speed; each recv may
 * wait recv_wait_timeout (2 s), so a stalled client costs at most ~12 s of
 * the single httpd task before the socket is closed. */
#define BODY_DEADLINE_US (10 * 1000 * 1000)
#define RECV_CHUNK 16384

static const api_boot_info_t *s_boot;
static _Atomic uint32_t s_frames_accepted;
static _Atomic uint32_t s_frames_rejected;
static _Atomic uint32_t s_accepted_seq;
static _Atomic uint32_t s_last_upload_ms;

static const char *state_name(panel_state_t s)
{
    switch (s) {
    case PANEL_RECEIVING: return "receiving";
    case PANEL_REFRESHING: return "refreshing";
    default: return "idle";
    }
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t reply_error(httpd_req_t *req, const char *status, const char *code)
{
    char body[48];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    atomic_fetch_add(&s_frames_rejected, 1);
    return reply(req, status, body);
}

static esp_err_t status_get(httpd_req_t *req)
{
    panel_status_t p;
    net_status_t n;
    panel_get_status(&p);
    net_get_status(&n);

    int64_t now = esp_timer_get_time();
    long shown_age_s = p.last_shown_us ? (long)((now - p.last_shown_us) / 1000000) : -1;
    esp_ip4_addr_t ip = {.addr = n.ip};

    char body[1024];
    snprintf(body, sizeof(body),
             "{\"fw\":\"%s\",\"uptime_s\":%" PRId64 ",\"boot_count\":%" PRIu32
             ",\"reset_reason\":\"%s\",\"restart_cause\":\"%s\",\"brownouts\":%" PRIu32
             ",\"state\":\"%s\",\"last_seq\":%" PRIu32 ",\"accepted_seq\":%" PRIu32
             ",\"frames_accepted\":%" PRIu32 ",\"frames_rejected\":%" PRIu32
             ",\"frames_shown\":%" PRIu32 ",\"panel_errors\":%" PRIu32
             ",\"last_error\":\"%s\",\"last_error_seq\":%" PRIu32 ",\"last_upload_ms\":%" PRIu32
             ",\"last_refresh_ms\":%" PRIu32 ",\"last_frame_ms\":%" PRIu32
             ",\"last_shown_age_s\":%ld,\"link_up\":%s,\"ip\":\"" IPSTR "\""
             ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"usb\":\"%04x:%04x\""
             ",\"usb_configs\":%u,\"link_downs\":%" PRIu32 ",\"ip_acquired\":%" PRIu32
             ",\"heap_internal_free\":%u,\"heap_internal_min\":%u,\"psram_free\":%u}",
             FW_VERSION, now / 1000000, s_boot->boot_count, s_boot->reset_reason,
             s_boot->restart_cause, s_boot->brownouts, state_name(p.state), p.last_seq,
             atomic_load(&s_accepted_seq), atomic_load(&s_frames_accepted),
             atomic_load(&s_frames_rejected), p.frames_shown, p.panel_errors, p.last_error,
             p.last_error_seq, atomic_load(&s_last_upload_ms), p.last_refresh_ms,
             p.last_frame_ms, shown_age_s, n.link_up ? "true" : "false", IP2STR(&ip), n.mac[0],
             n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5], n.usb_vid, n.usb_pid,
             n.usb_configs, n.link_downs, n.ip_acquired,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return reply(req, "200 OK", body);
}

static bool parse_seq(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*s < '0' || *s > '9' || *end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/*
 * Return values matter here (esp_http_server semantics):
 *   ESP_OK   -> httpd purges any unread body and keeps the connection, so the
 *               client reads our status cleanly. Used when Content-Length is
 *               the exact frame size (purge is bounded to one frame).
 *   ESP_FAIL -> httpd closes the socket. Used for a wrong Content-Length
 *               (purge would be unbounded) and for a broken/stalled body.
 */
static esp_err_t frame_post(httpd_req_t *req)
{
    if (req->content_len != FRAME_BYTES) { /* also rejects chunked (content_len 0) */
        reply_error(req, "400 Bad Request", "length");
        return ESP_FAIL;
    }
    char hdr[16];
    uint32_t seq;
    if (httpd_req_get_hdr_value_str(req, "X-Frame-Seq", hdr, sizeof(hdr)) != ESP_OK
        || !parse_seq(hdr, &seq)) {
        reply_error(req, "400 Bad Request", "seq");
        return ESP_OK;
    }
    uint8_t *fb = panel_claim();
    if (!fb) {
        reply_error(req, "429 Too Many Requests", "busy");
        return ESP_OK;
    }

    int64_t t0 = esp_timer_get_time();
    size_t got = 0;
    while (got < FRAME_BYTES) {
        if (esp_timer_get_time() - t0 > BODY_DEADLINE_US) {
            break;
        }
        int n = httpd_req_recv(req, (char *)fb + got, MIN(FRAME_BYTES - got, RECV_CHUNK));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; /* bounded by the deadline above */
        }
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    if (got < FRAME_BYTES) {
        panel_abort();
        ESP_LOGW(TAG, "seq=%" PRIu32 ": body short (%u of %d B)", seq, (unsigned)got, FRAME_BYTES);
        reply_error(req, "400 Bad Request", "body");
        return ESP_FAIL;
    }
    uint32_t upload_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    size_t bad = frame_first_invalid(fb, FRAME_BYTES);
    if (bad != FRAME_BYTES) {
        panel_abort();
        ESP_LOGW(TAG, "seq=%" PRIu32 ": invalid nibble at byte %u (0x%02x)", seq, (unsigned)bad,
                 fb[bad]);
        reply_error(req, "422 Unprocessable Entity", "pixels");
        return ESP_OK;
    }

    atomic_store(&s_last_upload_ms, upload_ms);
    atomic_store(&s_accepted_seq, seq);
    atomic_fetch_add(&s_frames_accepted, 1);
    panel_submit(seq); /* the refresh runs in the panel task; reply now */
    ESP_LOGI(TAG, "seq=%" PRIu32 " accepted, upload %" PRIu32 " ms", seq, upload_ms);

    char body[32];
    snprintf(body, sizeof(body), "{\"seq\":%" PRIu32 "}", seq);
    reply(req, "202 Accepted", body);
    return ESP_OK;
}

esp_err_t api_start(const api_boot_info_t *boot)
{
    s_boot = boot;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 5;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    static const httpd_uri_t status = {
        .uri = "/api/v1/status", .method = HTTP_GET, .handler = status_get};
    static const httpd_uri_t frame = {
        .uri = "/api/v1/frame", .method = HTTP_POST, .handler = frame_post};
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &frame);
    return ESP_OK;
}
