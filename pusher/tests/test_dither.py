import random
import sys
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dither  # noqa: E402

VALID = {0, 1, 2, 3, 5, 6}


def nibbles(frame: bytes):
    for b in frame:
        yield b >> 4
        yield b & 0x0F


def px(frame: bytes, x: int, y: int) -> int:
    b = frame[y * 600 + x // 2]
    return (b & 0x0F) if x & 1 else (b >> 4)


def test_pack_unpack_round_trip():
    rng = random.Random(1)
    idx = bytes(rng.randrange(6) for _ in range(dither.W * dither.H))
    frame = dither.pack(idx)
    assert len(frame) == 960_000
    assert dither.unpack(frame) == idx


def test_pack_even_pixel_in_high_nibble():
    idx = bytearray(dither.W * dither.H)  # all black
    idx[0], idx[1] = 3, 4  # pixel 0 red (index 3), pixel 1 blue (index 4)
    frame = dither.pack(bytes(idx))
    assert frame[0] == (dither.R << 4) | dither.B
    assert frame[1] == (dither.K << 4) | dither.K


def test_pack_rejects_index_without_colour():
    idx = bytearray(dither.W * dither.H)
    idx[123] = 6
    with pytest.raises(ValueError):
        dither.pack(bytes(idx))


@pytest.mark.parametrize("palette", sorted(dither.PALETTES))
def test_dither_uses_only_panel_inks(palette):
    # A gradient plus pure black/white, in an odd size and landscape, with alpha.
    img = Image.linear_gradient("L").resize((640, 480)).convert("RGBA")
    img.paste((255, 0, 255, 128), (0, 0, 100, 100))
    frame = dither.dither(img, palette)
    assert len(frame) == dither.FRAME_BYTES
    assert set(nibbles(frame)) <= VALID


def test_palette_has_exactly_six_entries():
    for name in dither.PALETTES:
        pal = dither._palette_image(name)
        assert len(pal.getpalette()) == 18


def test_pure_ink_maps_to_its_code():
    for i, rgb in enumerate(dither.PALETTES["d65"]):
        frame = dither.dither(Image.new("RGB", (30, 40), rgb))
        assert set(nibbles(frame)) == {dither.CODES[i]}


def test_chart_layout_matches_firmware():
    f = dither.chart()
    assert set(nibbles(f)) <= VALID
    assert px(f, 599, 800) == dither.Y  # bar 2 ends at the seam
    assert px(f, 600, 800) == dither.R  # red starts at the M/S boundary
    assert px(f, 10, 10) == dither.K
    assert px(f, 1190, 10) == dither.R
    assert px(f, 10, 1590) == dither.B
    assert px(f, 1190, 1590) == dither.WHITE
    assert px(f, 0, 800) == dither.K and px(f, 1199, 800) == dither.K
