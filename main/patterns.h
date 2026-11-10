/*
 * Generated test frames for gate G1 (panel bring-up without a network).
 * Generated, not embedded: a 960 KB array would not fit the app partition.
 */
#pragma once

#include <stdint.h>

typedef enum {
    PATTERN_CHART = 0, /* asymmetric six-colour chart: orientation + M/S split */
    PATTERN_BARS,      /* six horizontal bands, K W Y R B G top to bottom */
    PATTERN_CHECKER,   /* 200 px cells cycling the six colours */
    PATTERN_COUNT,
} pattern_t;

const char *pattern_name(pattern_t p);

/* Overwrite the whole frame buffer with pattern p. */
void pattern_draw(uint8_t *frame, pattern_t p);
