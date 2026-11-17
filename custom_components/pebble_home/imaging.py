"""Convert camera JPEGs into bitmaps a Pebble can draw without decoding.

The watch has 128 KB for code and heap and no JPEG decoder, so the server does
the heavy lifting: crop, enhance, pick a palette from the Pebble 64-colour gamut,
dither and pack the pixels in the exact GBitmap layout the firmware uses.

Wire format (all little endian):

    0  'P' 'B'      magic
    2  u8           version (1)
    3  u8           bits per pixel (1, 2, 4 or 8)
    4  u16          width
    6  u16          height
    8  u16          row stride in bytes
   10  u8           palette entries (0 for 8 bpp)
   11  u8[n]        palette, GColor8 bytes (0b11RRGGBB)
   ..  u8[]         pixel rows, palettised formats packed MSB first
"""

from __future__ import annotations

import io
import struct

from PIL import Image, ImageEnhance, ImageFilter, ImageOps

LEVELS = (0, 85, 170, 255)


def _pebble_palette_image() -> Image.Image:
    """A 'P' image whose palette is the full 64 colour Pebble gamut."""
    pal: list[int] = []
    for r in range(4):
        for g in range(4):
            for b in range(4):
                pal += [LEVELS[r], LEVELS[g], LEVELS[b]]
    pal += [0, 0, 0] * (256 - 64)
    img = Image.new("P", (1, 1))
    img.putpalette(pal)
    return img


_PEBBLE64 = _pebble_palette_image()


def _snap(v: int) -> int:
    """Nearest Pebble channel level index (0-3)."""
    return min(range(4), key=lambda i: abs(LEVELS[i] - v))


def _gcolor(r: int, g: int, b: int) -> int:
    return 0xC0 | (_snap(r) << 4) | (_snap(g) << 2) | _snap(b)


def _prepare(src: bytes, width: int, height: int, fit: str) -> Image.Image:
    img = Image.open(io.BytesIO(src))
    img = ImageOps.exif_transpose(img).convert("RGB")
    if fit == "contain":
        img = ImageOps.pad(img, (width, height), Image.Resampling.LANCZOS, color=(0, 0, 0))
    else:
        img = ImageOps.fit(img, (width, height), Image.Resampling.LANCZOS)
    # A 64 colour, 1.3" screen rewards punchy images: stretch the histogram,
    # lift saturation a little and sharpen what the downscale softened.
    img = ImageOps.autocontrast(img, cutoff=1)
    img = ImageEnhance.Color(img).enhance(1.25)
    img = img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=80, threshold=2))
    return img


def convert(
    src: bytes,
    width: int = 260,
    height: int = 170,
    bpp: int = 4,
    fit: str = "cover",
) -> bytes:
    """Return the wire format described in the module docstring."""
    width = max(8, min(260, int(width)))
    height = max(8, min(260, int(height)))
    if bpp not in (1, 2, 4, 8):
        bpp = 4
    if fit == "auto":
        # Portrait sources (doorbells) get a portrait frame rather than a
        # letterbox slice through someone's chest.
        with Image.open(io.BytesIO(src)) as probe:
            portrait = probe.height > probe.width
        if portrait != (height > width):
            width, height = height, width
        fit = "cover"
    img = _prepare(src, width, height, fit)

    if bpp == 8:
        q = img.quantize(palette=_PEBBLE64, dither=Image.Dither.FLOYDSTEINBERG)
        pal = q.getpalette()[: 64 * 3]
        lut = bytes(_gcolor(*pal[i * 3 : i * 3 + 3]) for i in range(64))
        # Padding entries are black; map them to opaque black, not clear.
        data = q.tobytes().translate(lut + bytes([0xC0]) * (256 - 64))
        header = struct.pack("<2sBBHHHB", b"PB", 1, 8, width, height, width, 0)
        return header + data

    colours = 1 << bpp
    if bpp == 1:
        # Black and white reads better than an adaptive two-colour palette.
        palette_rgb = [(0, 0, 0), (255, 255, 255)]
    else:
        # Pick an adaptive palette, then snap it onto the Pebble gamut so the
        # dither targets colours the screen can actually show.
        adaptive = img.quantize(colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        raw = adaptive.getpalette()[: colours * 3]
        snapped: list[tuple[int, int, int]] = []
        for i in range(0, len(raw), 3):
            c = tuple(LEVELS[_snap(v)] for v in raw[i : i + 3])
            if c not in snapped:
                snapped.append(c)  # type: ignore[arg-type]
        palette_rgb = snapped
    while len(palette_rgb) < colours:
        palette_rgb.append(palette_rgb[-1])

    pal_img = Image.new("P", (1, 1))
    flat = [v for c in palette_rgb for v in c]
    # Pad with copies of entry 0 so a stray index >= colours is still exact.
    pal_img.putpalette(flat + list(palette_rgb[0]) * (256 - len(palette_rgb)))
    q = img.quantize(palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)

    per_byte = 8 // bpp
    stride = (width * bpp + 7) // 8
    px = q.tobytes()
    out = bytearray(stride * height)
    mask = colours - 1
    for y in range(height):
        row = px[y * width : (y + 1) * width]
        base = y * stride
        for x, idx in enumerate(row):
            shift = (per_byte - 1 - (x % per_byte)) * bpp
            out[base + x // per_byte] |= (idx if idx <= mask else 0) << shift

    palette = bytes(_gcolor(*c) for c in palette_rgb)
    header = struct.pack("<2sBBHHHB", b"PB", 1, bpp, width, height, stride, colours)
    return header + palette + bytes(out)
