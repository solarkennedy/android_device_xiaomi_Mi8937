#!/usr/bin/env python3
"""Regenerate the solid-colour wallpapers for PepitoWallpapers.

WallpaperPicker2 loads a static wallpaper via ResourceAsset.openInputStream(),
which is Resources.openRawResource() piped into BitmapFactory/BitmapRegionDecoder.
That means the drawable MUST be a real encoded bitmap -- an XML <shape> or <color>
drawable decodes to null and the tile renders blank. Hence: actual PNGs.

720x1280 matches the FT8613 panel exactly, same as the stock Palm wallpapers.
A flat colour costs ~1 KB after PNG compression, so all of these together are
smaller than one of the photographic wallpapers.

Usage: python3 tools/gen-solids.py
"""

import pathlib
import struct
import zlib

WIDTH = 720
HEIGHT = 1280

# Name -> RGB. Neutrals first (the ones actually useful on a small screen),
# then a spread of hues wide enough to give Material You distinct palettes.
COLORS = {
    "solid_black": (0x00, 0x00, 0x00),
    "solid_charcoal": (0x1C, 0x1B, 0x1F),
    "solid_slate": (0x37, 0x47, 0x4F),
    "solid_bone": (0xE8, 0xE4, 0xDC),
    "solid_indigo": (0x1A, 0x23, 0x7E),
    "solid_blue": (0x0D, 0x47, 0xA1),
    "solid_teal": (0x00, 0x69, 0x5C),
    "solid_green": (0x1B, 0x5E, 0x20),
    "solid_amber": (0xC1, 0x7A, 0x0B),
    "solid_orange": (0xBF, 0x36, 0x0C),
    "solid_crimson": (0x8E, 0x1C, 0x2E),
    "solid_plum": (0x4A, 0x14, 0x8C),
}


def write_png(path, rgb, width=WIDTH, height=HEIGHT):
    """Minimal 8-bit truecolour PNG writer -- avoids a Pillow build dependency."""
    row = b"\x00" + bytes(rgb) * width  # filter type 0 (None) + pixel data
    raw = row * height

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(
            ">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    # bit depth 8, colour type 2 (truecolour), default compression/filter/interlace
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)
    return len(png)


def main():
    out = pathlib.Path(__file__).resolve().parent.parent / "res" / "drawable-nodpi"
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    for name, rgb in COLORS.items():
        size = write_png(out / f"{name}.png", rgb)
        total += size
        print(f"  {name}.png  #{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X}  {size:,} bytes")
    print(f"  total: {total:,} bytes")


if __name__ == "__main__":
    main()
