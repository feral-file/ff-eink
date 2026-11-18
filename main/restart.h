/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Restarts that record why (NVS "rcause", reported next boot as
 * restart_cause) and that never cut a panel refresh short.
 */
#pragma once

/* Restart now. */
void restart_with_cause(const char *cause) __attribute__((noreturn));

/* Wait until the panel is idle (a refresh ends within ~3 min, panel.c's
 * caps), then restart. An interrupted waveform leaves the panel undefined. */
void restart_when_idle(const char *cause) __attribute__((noreturn));

/* Restart about 0.5 s later from a new task, so the caller (an HTTP handler)
 * can return and its response go out. Only for a caller that holds the
 * panel claim (panel_claim) and keeps it: then no refresh is running or can
 * start, so there is nothing to wait for. */
void restart_held_async(const char *cause);
