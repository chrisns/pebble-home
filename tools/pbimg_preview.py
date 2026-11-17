"""Decode the Pebble Home wire format back to PNG, exactly as the watch would draw it."""
import struct, sys
from PIL import Image

LV = (0, 85, 170, 255)

def gc(b):
    return (LV[(b >> 4) & 3], LV[(b >> 2) & 3], LV[b & 3])

def decode(blob: bytes) -> Image.Image:
    magic, ver, bpp, w, h, stride, n = struct.unpack_from("<2sBBHHHB", blob)
    assert magic == b"PB", magic
    pal = [gc(c) for c in blob[11:11 + n]]
    data = blob[11 + n:]
    assert len(data) == stride * h, (len(data), stride * h)
    img = Image.new("RGB", (w, h))
    px = img.load()
    per = 8 // bpp
    for y in range(h):
        for x in range(w):
            if bpp == 8:
                px[x, y] = gc(data[y * stride + x])
            else:
                byte = data[y * stride + x // per]
                idx = (byte >> ((per - 1 - x % per) * bpp)) & ((1 << bpp) - 1)
                px[x, y] = pal[idx]
    return img

if __name__ == "__main__":
    decode(open(sys.argv[1], "rb").read()).save(sys.argv[2])
