/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * HTTP API, version 1. Wire contract: docs/api.md.
 *   GET  /api/v1/status    cache-only snapshot, never waits on the panel
 *   POST /api/v1/frame     one 960,000 B pre-dithered frame (frame.h layout)
 *   GET/PUT /api/v1/config panel name (identity.c)
 *   POST /api/v1/firmware  signed app image into the other OTA slot (ota.c)
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
