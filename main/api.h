/*
 * HTTP API, version 1. Wire contract: docs/api.md.
 *   GET  /api/v1/status  cache-only snapshot, never waits on the panel
 *   POST /api/v1/frame   one 960,000 B pre-dithered frame (frame.h layout)
 * Plain HTTP: the dedicated VLAN is the security boundary (README).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t boot_count;
    uint32_t brownouts;
    const char *reset_reason;  /* this boot's esp_reset_reason(), as text */
    const char *restart_cause; /* why the firmware itself restarted last time, or "" */
} api_boot_info_t;

/* `boot` must outlive the server. */
esp_err_t api_start(const api_boot_info_t *boot);
