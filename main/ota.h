/*
 * OTA bookkeeping (docs/ota.md). The upload itself is POST /api/v1/firmware
 * in api.c; this module owns what happens after a new image boots.
 */
#pragma once

#include <stdbool.h>

/* Normal build only, after the network and HTTP server were started
 * (api_ok tells whether the server is up). If this image is pending
 * verification, a task marks it valid once an IP has been held for 60 s
 * with the server up, or rolls back if that does not happen within 10 min
 * (or at once if the server failed to start). */
void ota_start_validation(bool api_ok);

const char *ota_state(void);  /* "valid", "pending" or "unknown" */
bool ota_rolled_back(void);   /* an image was rejected and sits in the other slot */
const char *ota_version(void); /* this image's version (git describe at build) */
