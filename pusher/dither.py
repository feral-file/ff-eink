#!/usr/bin/env python3
"""Turn images into 960,000-byte GDEP133C02 frames (see docs/api.md).

    python3 dither.py photo.jpg other.png -o out/ [--preview] [--palette aitjcize]

Pipeline: EXIF rotate -> RGB -> portrait 3:4 cover-crop -> 1200x1600 ->
Floyd-Steinberg onto the six panel inks -> 4 bpp, even-x pixel in the high
nibble. Needs Pillow >= 9.1 (Image.Dither).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageOps

W, H = 1200, 1600
FRAME_BYTES = W * H // 2

# Palette order is K W Y R B G; CODES maps palette index -> panel nibble.
CODES = (0x0, 0x1, 0x2, 0x3, 0x5, 0x6)
K, WHITE, Y, R, B, G = CODES

# Ink colours, K W Y R B G order. Numbers only, no code, from the projects
# below; provenance and licences in THIRD_PARTY_NOTICES.md.
#
#   bpc80        Frans-Willem/epd-dither src/palette.rs @ 9b461577
#                (SPECTRA6_D65_BPC80_ADJUSTED; GPL-3.0 at that commit,
#                AGPL-3.0-or-later since eccead50, values unchanged), the
#                only set taken from that repository: spectrophotometer measurement of a
#                reTerminal E1004 / T133A01 panel (same panel class) under
#                D65 light, with 80 % black-point compensation.
#   aitjcize     aitjcize/esp32-photoframe main/color_palette.c @ 54d87a36
#                (MIT): the default 6-colour palette of that photo frame.
#                Its own pipeline also applies an S-curve and extra
#                saturation before dithering; only the palette is here.
#   epdoptimize  paperlesspaper/epdoptimize src/dither/data/
#                default-palettes.json "spectra6" @ 25f42661 (Apache-2.0).
#
# Which palette, judged on the panel itself (docs/bench.md, 2026-11-10): a
# dark, high-contrast artwork looked clearly best with "bpc80": clean, deep
# blacks. The uncompensated measurement (epd-dither's D65_ADJUSTED) flattened
# everything darker than the black ink into one colour, and a per-image tone
# mapping onto it put coloured noise in the shadows; neither is shipped. Screen previews exaggerate palettes with brighter inks:
# judge on the panel.
PALETTES = {
    "bpc80": ((22, 29, 49), (206, 227, 234), (234, 212, 0), (159, 0, 0), (0, 96, 186), (81, 155, 99)),
    "aitjcize": ((2, 2, 2), (190, 200, 200), (205, 202, 0), (135, 19, 0), (5, 64, 158), (39, 102, 60)),
    "epdoptimize": ((31, 34, 38), (185, 199, 201), (193, 187, 30), (98, 32, 30), (35, 63, 142), (53, 86, 58)),
}
DEFAULT_PALETTE = "bpc80"

# idx -> code in the high / low nibble, as bytes.translate tables.
_HI = bytes((CODES[i] << 4) if i < 6 else 0xFF for i in range(256))
_LO = bytes(CODES[i] if i < 6 else 0xFF for i in range(256))


def fit(img: Image.Image) -> Image.Image:
    """EXIF-rotate, drop alpha, cover-crop to 3:4 portrait, resize to 1200x1600."""
    img = ImageOps.exif_transpose(img).convert("RGB")
    return ImageOps.fit(img, (W, H), method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))


def _palette_image(palette: str) -> Image.Image:
    pal = Image.new("P", (1, 1))
    # Exactly 18 values: padding to 256 entries would add (0,0,0) entries the
    # quantizer may pick, producing indices with no panel colour.
    pal.putpalette([v for rgb in PALETTES[palette] for v in rgb])
    return pal


def pack(indices: bytes) -> bytes:
    """Palette indices (one byte per pixel, 0..5, row-major) -> frame bytes."""
    if len(indices) != W * H:
        raise ValueError(f"expected {W * H} pixels, got {len(indices)}")
    if max(indices) > 5:
        raise ValueError(f"palette index {max(indices)} has no panel colour")
    hi = indices[0::2].translate(_HI)
    lo = indices[1::2].translate(_LO)
    return bytes(a | b for a, b in zip(hi, lo))


def unpack(frame: bytes) -> bytes:
    """Frame bytes -> palette indices (inverse of pack)."""
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"expected {FRAME_BYTES} bytes, got {len(frame)}")
    code_to_idx = {c: i for i, c in enumerate(CODES)}
    out = bytearray(W * H)
    for i, b in enumerate(frame):
        out[2 * i] = code_to_idx[b >> 4]
        out[2 * i + 1] = code_to_idx[b & 0x0F]
    return bytes(out)


def dither(img: Image.Image, palette: str = DEFAULT_PALETTE) -> bytes:
    """Any image -> frame bytes."""
    q = fit(img).quantize(palette=_palette_image(palette), dither=Image.Dither.FLOYDSTEINBERG)
    return pack(q.tobytes())


def preview(frame: bytes, palette: str = DEFAULT_PALETTE) -> Image.Image:
    """Frame bytes -> RGB image in the measured inks, for a look before pushing."""
    p = Image.frombytes("P", (W, H), unpack(frame))
    p.putpalette([v for rgb in PALETTES[palette] for v in rgb])
    return p.convert("RGB")


def chart() -> bytes:
    """The asymmetric six-colour chart, same layout as main/patterns.c.

    Pushed as the first and last frame of a soak so the two photos can be
    compared for ghosting. Pure inks, so no dithering is involved.
    """
    px = bytearray([1]) * (W * H)  # palette index 1 = white

    def rect(x0: int, y0: int, w: int, h: int, idx: int) -> None:
        for y in range(max(0, y0), min(H, y0 + h)):
            px[y * W + max(0, x0) : y * W + min(W, x0 + w)] = bytes([idx]) * (min(W, x0 + w) - max(0, x0))

    for i in range(6):
        rect(i * 200, 400, 200, 800, i)
    rect(0, 0, 300, 300, 0)  # TL black
    rect(W - 300, 0, 300, 300, 3)  # TR red
    rect(0, H - 300, 300, 300, 4)  # BL blue
    rect(0, 0, W, 2, 0)
    rect(0, H - 2, W, 2, 0)
    rect(0, 0, 2, H, 0)
    rect(W - 2, 0, 2, H, 0)
    return pack(bytes(px))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="*", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True, help="output directory")
    ap.add_argument("--palette", choices=sorted(PALETTES), default=DEFAULT_PALETTE,
                    help="bpc80 (default), aitjcize, epdoptimize; see docs/pusher.md")
    ap.add_argument("--preview", action="store_true", help="also write <name>.preview.png")
    ap.add_argument("--chart", action="store_true", help="also write chart.bin")
    args = ap.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    jobs = [(p.stem, lambda p=p: dither(Image.open(p), args.palette)) for p in args.images]
    if args.chart:
        jobs.append(("chart", chart))
    for name, make in jobs:
        frame = make()
        (args.out / f"{name}.bin").write_bytes(frame)
        if args.preview:
            preview(frame, args.palette).save(args.out / f"{name}.preview.png")
        print(f"{args.out / name}.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
