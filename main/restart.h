/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File
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

/* restart_when_idle from a new task, for callers that must return first
 * (an HTTP handler that still has to send its response). */
void restart_when_idle_async(const char *cause);
