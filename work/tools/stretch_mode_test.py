"""Reproduce the Musai harbor vertical lines with the real Windows GDI.

Draws ``Bmp\\HERBOR\\MUSA.DOC`` (587x416, 8bpp) into a 576x416 DIB section
exactly like FUN_00407200 does (StretchDIBits, DIB_RGB_COLORS, SRCCOPY) under
the default BLACKONWHITE mode and under COLORONCOLOR (Phase 5), then counts
destination pixels that match neither neighbouring source column.

usage: stretch_mode_test.py [--out DIR]   (writes PNGs when --out is given)
"""

from __future__ import annotations

import argparse
import ctypes
import struct
import sys
from ctypes import wintypes
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / "source_exe" / "Bmp" / "HERBOR" / "MUSA.DOC"
BLACKONWHITE, COLORONCOLOR = 1, 3
gdi32 = ctypes.WinDLL("gdi32")
user32 = ctypes.WinDLL("user32")
gdi32.CreateDIBSection.restype = wintypes.HBITMAP
gdi32.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT,
                                   ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.StretchDIBits.argtypes = [wintypes.HDC] + [ctypes.c_int] * 8 + [
    ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT, wintypes.DWORD]
gdi32.SetStretchBltMode.argtypes = [wintypes.HDC, ctypes.c_int]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]


def render(file_data: bytes, dst_bpp: int, mode: int | None):
    info = bytearray(file_data[14:14 + 40 + 1024])
    bits = file_data[struct.unpack_from("<I", file_data, 10)[0]:]
    width, height = struct.unpack_from("<ii", info, 4)
    dw, dh = 576, 416
    # Destination DIB section: same palette for 8bpp, RGB555 for 16bpp.
    dinfo = bytearray(info) if dst_bpp == 8 else bytearray(40)
    struct.pack_into("<IiiHHI", dinfo, 0, 40, dw, dh, 1, dst_bpp, 0)
    pbits = ctypes.c_void_p()
    hdc = gdi32.CreateCompatibleDC(None)
    dinfo_buf = ctypes.create_string_buffer(bytes(dinfo), len(dinfo))
    hbm = gdi32.CreateDIBSection(hdc, dinfo_buf, 0, ctypes.byref(pbits), None, 0)
    old = gdi32.SelectObject(hdc, hbm)
    if mode is not None:
        gdi32.SetStretchBltMode(hdc, mode)
    src_info = ctypes.create_string_buffer(bytes(info), len(info))
    src_bits = ctypes.create_string_buffer(bits, len(bits))
    lines = gdi32.StretchDIBits(hdc, 0, 0, dw, dh, 0, 0, width, height,
                                src_bits, src_info, 0, 0x00CC0020)
    assert lines, "StretchDIBits failed"
    gdi32.GdiFlush() if hasattr(gdi32, "GdiFlush") else None
    stride = ((dw * dst_bpp + 31) // 32) * 4
    raw = ctypes.string_at(pbits, stride * dh)
    gdi32.SelectObject(hdc, old)
    gdi32.DeleteObject(hbm)
    gdi32.DeleteDC(hdc)
    return raw, stride, width


def rgb_rows(raw, stride, bpp, palette):
    rows = []
    for y in range(416):
        row = raw[y * stride:(y + 1) * stride]
        if bpp == 8:
            rows.append([palette[row[x]] for x in range(576)])
        else:
            px = struct.unpack_from("<576H", row)
            rows.append([((p >> 10 & 31) << 3, (p >> 5 & 31) << 3, (p & 31) << 3) for p in px])
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    data = SOURCE.read_bytes()
    pal_raw = data[54:54 + 1024]
    palette = [(pal_raw[i * 4 + 2], pal_raw[i * 4 + 1], pal_raw[i * 4]) for i in range(256)]
    width = struct.unpack_from("<i", data, 18)[0]
    src_stride = (width + 3) & ~3
    src = data[struct.unpack_from("<I", data, 10)[0]:]
    src_rgb = [[palette[src[y * src_stride + x]] for x in range(width)] for y in range(416)]

    failed = False
    for bpp in (8, 16):
        for name, mode in (("default", None), ("COLORONCOLOR", COLORONCOLOR)):
            raw, stride, _ = render(data, bpp, mode)
            rows = rgb_rows(raw, stride, bpp, palette)
            q = (lambda c: c) if bpp == 8 else (lambda c: (c[0] & 0xF8, c[1] & 0xF8, c[2] & 0xF8))
            bad_cols = set()
            bad = 0
            for y in range(416):
                for x in range(576):
                    lo = x * width // 576
                    cands = {q(src_rgb[y][s]) for s in range(max(0, lo - 1), min(width, lo + 3))}
                    if q(rows[y][x]) not in cands:
                        bad += 1
                        bad_cols.add(x)
            heavy = sorted(c for c in bad_cols
                           if sum(1 for y in range(416)
                                  if q(rows[y][c]) not in {q(src_rgb[y][s]) for s in range(max(0, c * width // 576 - 1), min(width, c * width // 576 + 3))}) > 20)
            print(f"dst {bpp:2d}bpp {name:12s}: foreign pixels={bad:6d}  line columns={heavy}")
            if name == "COLORONCOLOR" and heavy:
                failed = True
            if args.out:
                from PIL import Image
                img = Image.new("RGB", (576, 416))
                img.putdata([c for row in reversed(rows) for c in row])
                args.out.mkdir(parents=True, exist_ok=True)
                img.save(args.out / f"musa_doc_{bpp}bpp_{name}.png")
    print("FAIL" if failed else "PASS: COLORONCOLOR leaves no vertical lines")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
