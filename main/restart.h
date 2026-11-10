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
