/*
 * Frame format of the GDEP133C02 (13.3" Spectra 6, 1200x1600).
 *
 * Pure helpers with no ESP-IDF dependency, so test/frame_test.c can run them
 * on the dev machine. The wire format (docs/api.md) is exactly this layout:
 *
 *   - 1600 rows x 600 bytes = 960,000 bytes, row-major, top row first.
 *   - 4 bpp, two pixels per byte, the even-x pixel in the high nibble.
 *   - Nibble codes: K 0x0, W 0x1, Y 0x2, R 0x3, B 0x5, G 0x6. 0x4 and 0x7 are
 *     not colours (the vendor code never sends them), nor is anything in
 *     0x8-0xF, so a frame carrying them is rejected rather than shown as
 *     undefined ink.
 *   - Two controller ICs: bytes 0..299 of each row (left 600 px) go to the
 *     master (CS_M), bytes 300..599 (right 600 px) to the slave (CS_S).
 *
 * Source: GDEP133C02 datasheet + Good Display example GDEP133C02.c (2026-04-20).
 * Which physical edge is "top/left" is settled on hardware with the
 * asymmetric chart (patterns.c); this header only fixes the byte layout.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FRAME_W 1200
#define FRAME_H 1600
#define FRAME_ROW_BYTES (FRAME_W / 2)            /* 600 */
#define FRAME_HALF_ROW_BYTES (FRAME_ROW_BYTES / 2) /* 300 per controller IC */
#define FRAME_BYTES (FRAME_ROW_BYTES * FRAME_H)  /* 960,000 */

enum frame_colour {
    PX_K = 0x0,
    PX_W = 0x1,
    PX_Y = 0x2,
    PX_R = 0x3,
    PX_B = 0x5,
    PX_G = 0x6,
};

enum frame_half {
    FRAME_HALF_MASTER = 0, /* left 600 px, CS_M */
    FRAME_HALF_SLAVE = 1,  /* right 600 px, CS_S */
};

/* Bit n set <=> nibble value n is a colour: {0,1,2,3,5,6} = 0b01101111. */
#define FRAME_VALID_NIBBLES 0x6Fu

static inline bool frame_nibble_valid(uint8_t n)
{
    return n < 8 && ((FRAME_VALID_NIBBLES >> n) & 1u);
}

static inline bool frame_byte_valid(uint8_t b)
{
    return frame_nibble_valid(b >> 4) && frame_nibble_valid(b & 0x0F);
}

/* Index of the first byte holding a non-colour nibble, or len if all valid. */
static inline size_t frame_first_invalid(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!frame_byte_valid(buf[i])) {
            return i;
        }
    }
    return len;
}

/*
 * Copy one controller's half of `rows` consecutive rows starting at `row0`
 * into `dst` (rows * 300 bytes, contiguous). This is the gather the panel
 * driver does from the PSRAM frame into its DMA bounce buffer: each IC
 * receives its 300 B of every row back to back after one DTM command.
 */
static inline void frame_gather_half(uint8_t *dst, const uint8_t *frame,
                                     unsigned row0, unsigned rows, enum frame_half half)
{
    const uint8_t *src = frame + (size_t)row0 * FRAME_ROW_BYTES
                         + (half == FRAME_HALF_SLAVE ? FRAME_HALF_ROW_BYTES : 0);
    for (unsigned r = 0; r < rows; r++) {
        memcpy(dst + (size_t)r * FRAME_HALF_ROW_BYTES, src + (size_t)r * FRAME_ROW_BYTES,
               FRAME_HALF_ROW_BYTES);
    }
}

/* Set one pixel. x in [0,1200), y in [0,1600), c a frame_colour. */
static inline void frame_set_pixel(uint8_t *frame, unsigned x, unsigned y, uint8_t c)
{
    uint8_t *p = frame + (size_t)y * FRAME_ROW_BYTES + x / 2;
    *p = (x & 1u) ? (uint8_t)((*p & 0xF0) | c) : (uint8_t)((*p & 0x0F) | (c << 4));
}

/* Fill a clipped rectangle; whole bytes are memset, odd edges per pixel. */
static inline void frame_fill_rect(uint8_t *frame, int x0, int y0, int w, int h, uint8_t c)
{
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FRAME_W) x1 = FRAME_W;
    if (y1 > FRAME_H) y1 = FRAME_H;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    const uint8_t pair = (uint8_t)((c << 4) | c);
    for (int y = y0; y < y1; y++) {
        int x = x0;
        if (x & 1) {
            frame_set_pixel(frame, (unsigned)x++, (unsigned)y, c);
        }
        int even_end = x + ((x1 - x) & ~1);
        if (even_end > x) {
            memset(frame + (size_t)y * FRAME_ROW_BYTES + x / 2, pair, (size_t)(even_end - x) / 2);
            x = even_end;
        }
        if (x < x1) {
            frame_set_pixel(frame, (unsigned)x, (unsigned)y, c);
        }
    }
}
