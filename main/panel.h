/*
 * GDEP133C02 panel driver and the single frame buffer.
 *
 * Ownership: one 960,000 B frame buffer in PSRAM, allocated once at boot.
 * Exactly one party owns it at a time, tracked by the panel state:
 *
 *   IDLE --panel_claim()--> RECEIVING --panel_submit()--> REFRESHING --(panel task)--> IDLE
 *                               \--panel_abort()--> IDLE
 *
 * Writers (HTTP handler, bench-build test pattern) claim it, fill it, then submit.
 * The panel task is the only reader and returns the state to IDLE when the
 * refresh ends, successfully or not. A claim while not IDLE fails: that is
 * the API's 429.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PANEL_IDLE = 0,
    PANEL_RECEIVING,
    PANEL_REFRESHING,
} panel_state_t;

typedef struct {
    panel_state_t state;
    uint32_t last_seq;        /* seq of the last frame shown successfully */
    uint32_t frames_shown;
    uint32_t panel_errors;
    const char *last_error;   /* static string, "" if none */
    uint32_t last_error_seq;
    uint32_t last_refresh_ms; /* DRF until BUSY released, last successful frame */
    uint32_t last_frame_ms;   /* reset until deep sleep, last successful frame */
    int64_t last_shown_us;    /* esp_timer time of the last successful frame, 0 if none */
} panel_status_t;

/* GPIO, SPI, PSRAM frame buffer, panel power and the panel task. Fatal-or-OK. */
esp_err_t panel_init(void);

/* IDLE -> RECEIVING. Returns the frame buffer, or NULL when busy. */
uint8_t *panel_claim(void);

/* RECEIVING -> IDLE without a refresh (bad or incomplete upload). */
void panel_abort(void);

/* RECEIVING -> REFRESHING; the panel task shows the buffer and records seq. */
void panel_submit(uint32_t seq);

/* Lock-free snapshot for the status endpoint; never blocks on the panel. */
void panel_get_status(panel_status_t *out);
