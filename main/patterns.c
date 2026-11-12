/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Feral File
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "patterns.h"

#include <string.h>

#include "frame.h"

static const uint8_t k_six[6] = {PX_K, PX_W, PX_Y, PX_R, PX_B, PX_G};

const char *pattern_name(pattern_t p)
{
    static const char *const names[PATTERN_COUNT] = {"chart", "bars", "checker"};
    return p < PATTERN_COUNT ? names[p] : "?";
}

/*
 * What the chart proves when photographed (pusher/dither.py chart() draws
 * the identical frame, so the pushed chart and this one can be compared):
 *   - Corner squares differ (TL black, TR red, BL blue, BR empty), so any
 *     mirror or rotation of the scan shows up.
 *   - The six vertical bars run K W Y R B G left to right and red starts
 *     exactly at x = 600, the master/slave boundary: swapped halves put red
 *     on the left, a column offset shows a sliver at the seam.
 *   - A 2 px black border shows that the first/last rows and columns land.
 */
static void draw_chart(uint8_t *f)
{
    memset(f, (PX_W << 4) | PX_W, FRAME_BYTES);
    for (int i = 0; i < 6; i++) {
        frame_fill_rect(f, i * 200, 400, 200, 800, k_six[i]);
    }
    frame_fill_rect(f, 0, 0, 300, 300, PX_K);
    frame_fill_rect(f, FRAME_W - 300, 0, 300, 300, PX_R);
    frame_fill_rect(f, 0, FRAME_H - 300, 300, 300, PX_B);
    frame_fill_rect(f, 0, 0, FRAME_W, 2, PX_K);
    frame_fill_rect(f, 0, FRAME_H - 2, FRAME_W, 2, PX_K);
    frame_fill_rect(f, 0, 0, 2, FRAME_H, PX_K);
    frame_fill_rect(f, FRAME_W - 2, 0, 2, FRAME_H, PX_K);
}

static void draw_bars(uint8_t *f)
{
    for (int i = 0; i < 6; i++) {
        int y0 = i * FRAME_H / 6, y1 = (i + 1) * FRAME_H / 6;
        frame_fill_rect(f, 0, y0, FRAME_W, y1 - y0, k_six[i]);
    }
}

static void draw_checker(uint8_t *f)
{
    for (int gy = 0; gy < FRAME_H / 200; gy++) {
        for (int gx = 0; gx < FRAME_W / 200; gx++) {
            frame_fill_rect(f, gx * 200, gy * 200, 200, 200, k_six[(gx + gy) % 6]);
        }
    }
}

void pattern_draw(uint8_t *frame, pattern_t p)
{
    switch (p) {
    case PATTERN_BARS:
        draw_bars(frame);
        break;
    case PATTERN_CHECKER:
        draw_checker(frame);
        break;
    case PATTERN_CHART:
    default:
        draw_chart(frame);
        break;
    }
}
