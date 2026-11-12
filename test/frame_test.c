/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Host unit tests for the pure frame helpers (main/frame.h) and the test
 * patterns. Run: make -C test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "name_rules.h"
#include "patterns.h"

static int failures;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                            \
        }                                                                          \
    } while (0)

static uint8_t px(const uint8_t *f, unsigned x, unsigned y)
{
    uint8_t b = f[(size_t)y * FRAME_ROW_BYTES + x / 2];
    return (x & 1) ? (b & 0x0F) : (b >> 4);
}

static void test_nibbles(void)
{
    const int valid[16] = {1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int n = 0; n < 16; n++) {
        CHECK(frame_nibble_valid((uint8_t)n) == valid[n]);
    }
    for (int b = 0; b < 256; b++) {
        CHECK(frame_byte_valid((uint8_t)b) == (valid[b >> 4] && valid[b & 15]));
    }
    uint8_t buf[8] = {0x01, 0x23, 0x56, 0x65, 0x11, 0x00, 0x33, 0x22};
    CHECK(frame_first_invalid(buf, sizeof(buf)) == sizeof(buf));
    buf[5] = 0x14; /* low nibble 4 */
    CHECK(frame_first_invalid(buf, sizeof(buf)) == 5);
    buf[2] = 0x71; /* high nibble 7 */
    CHECK(frame_first_invalid(buf, sizeof(buf)) == 2);
}

static void test_gather(uint8_t *f)
{
    /* Byte value encodes (row, column-byte) so any misplacement shows. */
    for (unsigned y = 0; y < FRAME_H; y++) {
        for (unsigned c = 0; c < FRAME_ROW_BYTES; c++) {
            f[y * FRAME_ROW_BYTES + c] = (uint8_t)(y * 7 + c);
        }
    }
    static uint8_t out[3 * FRAME_HALF_ROW_BYTES];
    frame_gather_half(out, f, 1598, 2, FRAME_HALF_SLAVE);
    for (unsigned r = 0; r < 2; r++) {
        for (unsigned c = 0; c < FRAME_HALF_ROW_BYTES; c++) {
            CHECK(out[r * FRAME_HALF_ROW_BYTES + c] == (uint8_t)((1598 + r) * 7 + 300 + c));
        }
    }
    frame_gather_half(out, f, 0, 3, FRAME_HALF_MASTER);
    for (unsigned r = 0; r < 3; r++) {
        for (unsigned c = 0; c < FRAME_HALF_ROW_BYTES; c++) {
            CHECK(out[r * FRAME_HALF_ROW_BYTES + c] == (uint8_t)(r * 7 + c));
        }
    }
}

static void test_fill_rect(uint8_t *f)
{
    /* Odd/even start and end columns, clipping on every side. */
    const int rects[][4] = {{1, 1, 3, 2}, {2, 5, 4, 1}, {7, 0, 1, 1}, {-5, -5, 10, 10},
                            {1195, 1595, 20, 20}, {0, 0, 0, 5}};
    for (size_t i = 0; i < sizeof(rects) / sizeof(rects[0]); i++) {
        memset(f, 0x11, FRAME_BYTES);
        const int *r = rects[i];
        frame_fill_rect(f, r[0], r[1], r[2], r[3], PX_R);
        for (int y = 0; y < 12; y++) {
            for (int x = 0; x < 12; x++) {
                int in = x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
                CHECK(px(f, (unsigned)x, (unsigned)y) == (in ? PX_R : PX_W));
            }
        }
        for (int y = FRAME_H - 8; y < FRAME_H; y++) {
            for (int x = FRAME_W - 8; x < FRAME_W; x++) {
                int in = x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
                CHECK(px(f, (unsigned)x, (unsigned)y) == (in ? PX_R : PX_W));
            }
        }
    }
}

static void test_patterns(uint8_t *f)
{
    for (int p = 0; p < PATTERN_COUNT; p++) {
        memset(f, 0x44, FRAME_BYTES); /* invalid filler: patterns must cover it all */
        pattern_draw(f, (pattern_t)p);
        CHECK(frame_first_invalid(f, FRAME_BYTES) == FRAME_BYTES);
    }
    pattern_draw(f, PATTERN_CHART);
    CHECK(px(f, 599, 800) == PX_Y); /* bar 2 (Y) ends at x = 599 */
    CHECK(px(f, 600, 800) == PX_R); /* red starts exactly at the M/S seam */
    CHECK(px(f, 10, 10) == PX_K);   /* TL */
    CHECK(px(f, 1190, 10) == PX_R); /* TR */
    CHECK(px(f, 10, 1590) == PX_B); /* BL */
    CHECK(px(f, 1190, 1590) == PX_W); /* BR empty */
    CHECK(px(f, 0, 800) == PX_K && px(f, 1199, 800) == PX_K); /* border */
}

static void test_names(void)
{
    const char *ok[] = {"a", "lobby-r03-c07", "ffeink-cb0cb4", "0", "abcdefghijklmnopqrstuvwxyz012345"};
    const char *bad[] = {"", "-a", "a-", "Lobby", "a_b", "a.b", "a b", "é",
                         "abcdefghijklmnopqrstuvwxyz0123456" /* 33 */, NULL};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        CHECK(name_valid(ok[i]));
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!name_valid(bad[i]));
    }
}

int main(void)
{
    uint8_t *f = malloc(FRAME_BYTES);
    if (!f) {
        return 2;
    }
    test_nibbles();
    test_gather(f);
    test_fill_rect(f);
    test_patterns(f);
    test_names();
    free(f);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("frame_test: all checks passed\n");
    return 0;
}
