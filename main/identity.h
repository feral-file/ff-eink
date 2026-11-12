/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Panel identity.
 *
 *   hw_id  the chip's factory MAC (eFuse), hex. Never changes for a board;
 *          use it to tell physical units apart (inventory, RMA). The USB
 *          NIC's MAC is not used: it changes when an adapter is swapped.
 *   name   the wall-position label set at provisioning (name_rules.h),
 *          kept in NVS; defaults to "ffeink-" + the last 6 hex of hw_id.
 *          Sent as the DHCP hostname and reported in the status.
 *
 * Swapping a spare in means setting the dead panel's name on the spare
 * (PUT /api/v1/config); see docs/api.md.
 */
#pragma once

#include "esp_err.h"
#include "name_rules.h"

/* Load the name from NVS (or the default). Call after nvs_flash_init. */
void identity_init(void);

const char *identity_hw_id(void); /* 12 lowercase hex chars */

/* Copy of the current name; safe from any task. */
void identity_name(char out[NAME_MAX_LEN + 1]);

/* Validate, persist, and apply (DHCP hostname). ESP_ERR_INVALID_ARG if the
 * name breaks name_rules.h; other errors from NVS. */
esp_err_t identity_set_name(const char *name);
