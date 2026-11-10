/*
 * Panel name rules (pure; host-tested in test/frame_test.c).
 *
 * The name is the panel's wall-position label, e.g. "lobby-r03-c07". It is
 * also sent as the DHCP hostname, so it follows the DNS label rules
 * (RFC 1123), restricted to lower case: 1-32 chars of [a-z0-9-], not
 * starting or ending with '-'. 32 keeps it inside lwIP's DHCP hostname
 * option and NVS string limits with room to spare.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define NAME_MAX_LEN 32

static inline bool name_valid(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > NAME_MAX_LEN || s[0] == '-' || s[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}
