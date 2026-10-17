"""Read-only audit of FUN_004090F0 and a real GDI palette-fade reproduction.

Does not patch or launch the game, touch saves, or capture the screen.
The GDI timing is for an offscreen reproduction, not an in-game measurement.
Usage: python work/tools/audit_fade.py [--exe PATH] [--output PATH]
"""

from __future__ import annotations

import argparse
import ctypes as c
import hashlib
import json
import statistics
import struct
import time
from ctypes import wintypes as w
from pathlib import Path

from audit_game_boundaries import pe_reader

ROOT = Path(__file__).resolve().parents[2]


def audit(exe: Path) -> dict:
    data = exe.read_bytes()
    read = pe_reader(data)
    expected = {
        0x4091BB: "66c74424120000",  # counter = 0
        0x4091C2: "6683bc247c04000001",  # direction == 1
        0x4091E3: "66b96400662b4c2412",  # fade out: 100 - counter
        0x4091FC: "b964000000",  # RGB divisor = 100
        0x409289: "66834424120266837c2412640f8c27ffffff",  # +=2; <100
    }
    for address, encoded in expected.items():
        wanted = bytes.fromhex(encoded)
        if read(address, len(wanted)) != wanted:
            raise ValueError(f"Instruction guard failed at {address:#x}")

    # Check the listed calls and import names using known disassembled
    # instruction boundaries rather than a raw opcode search.
    imports = {
        0x409133: (0x44C2A8, "StretchBlt"),
        0x409147: (0x44C290, "GetObjectA"),
        0x40915D: (0x44C2D0, "GlobalLock"),
        0x40918E: (0x44C28C, "GetDIBColorTable"),
        0x4092A9: (0x44C2DC, "GlobalUnlock"),
        0x4092B7: (0x44C348, "GlobalFree"),
    }
    for address, (slot, name) in imports.items():
        if read(address, 6) != b"\xff\x15" + struct.pack("<I", slot):
            raise ValueError(f"Call guard failed at {address:#x}")
        rva = struct.unpack("<I", read(slot, 4))[0]
        if read(0x400000 + rva + 2, len(name) + 1) != name.encode() + b"\0":
            raise ValueError(f"Import guard failed for {name}")
    out = list(range(100, 0, -2))
    into = list(range(0, 100, 2))
    return {
        "exe_sha256": hashlib.sha256(data).hexdigest(),
        "function_va": "0x004090F0", "instruction_guards": "PASS",
        "fade_out_percent": out, "fade_in_percent": into,
        "white_at_last_fade_out_frame": 255 * out[-1] // 100,
        "white_at_last_fade_in_frame": 255 * into[-1] // 100,
        "note": "Direction/loop arithmetic is checked against executable bytes; GDI below is a reproduction.",
    }


def gdi_reproduction() -> dict:
    gdi = c.WinDLL("gdi32", use_last_error=True)
    gdi.CreateCompatibleDC.argtypes = [w.HDC]
    gdi.CreateCompatibleDC.restype = w.HDC
    gdi.CreateDIBSection.argtypes = [w.HDC, c.c_void_p, w.UINT,
                                    c.POINTER(c.c_void_p), w.HANDLE, w.DWORD]
    gdi.CreateDIBSection.restype = w.HBITMAP
    gdi.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
    gdi.SelectObject.restype = w.HGDIOBJ
    gdi.DeleteObject.argtypes = [w.HGDIOBJ]
    gdi.DeleteDC.argtypes = [w.HDC]
    gdi.StretchDIBits.argtypes = [w.HDC] + [c.c_int] * 8 + [
        c.c_void_p, c.c_void_p, w.UINT, w.DWORD]
    width, height = 576, 416
    dst_info = c.create_string_buffer(40)
    struct.pack_into("<IiiHH", dst_info, 0, 40, width, -height, 1, 32)
    bits = c.c_void_p()
    dc = gdi.CreateCompatibleDC(None)
    if not dc:
        raise c.WinError(c.get_last_error())
    bitmap = gdi.CreateDIBSection(dc, dst_info, 0, c.byref(bits), None, 0)
    if not bitmap:
        gdi.DeleteDC(dc)
        raise c.WinError(c.get_last_error())
    old = gdi.SelectObject(dc, bitmap)
    try:
        info = c.create_string_buffer(40 + 256 * 4)
        struct.pack_into("<IiiHH", info, 0, 40, width, -height, 1, 8)
        # Synthetic grayscale: a visible pattern even at RGB 0..5.
        pixels = (c.c_ubyte * (width * height))()
        for y in range(height):
            for x in range(width):
                pixels[y * width + x] = (x * 255 // (width - 1)) if y < 208 else 255

        def draw(percent: int):
            for index in range(256):
                value = index * percent // 100
                struct.pack_into("4B", info, 40 + index * 4, value, value, value, 0)
            if gdi.StretchDIBits(dc, 0, 0, width, height, 0, 0, width,
                                 height, pixels, info, 0, 0xCC0020) != height:
                raise RuntimeError("StretchDIBits failed")

        elapsed = []
        for _ in range(7):
            start = time.perf_counter()
            for percent in range(100, 0, -2):
                draw(percent)
            gdi.GdiFlush()
            elapsed.append((time.perf_counter() - start) * 1000)
        raw = c.string_at(bits, width * height * 4)
        rgb = [value for index, value in enumerate(raw) if index % 4 != 3]
        endpoint = {"max_rgb": max(rgb), "nonblack_rgb_components": sum(v != 0 for v in rgb)}
        draw(0)
        gdi.GdiFlush()
        raw = c.string_at(bits, width * height * 4)
        black_max = max(value for index, value in enumerate(raw) if index % 4 != 3)
        if endpoint["max_rgb"] != 5 or black_max != 0:
            raise AssertionError("GDI endpoint reproduction differed from expected arithmetic")
        return {
            "surface": "576x416 8bpp palette -> offscreen 32bpp DIB",
            "original_2_percent_endpoint": endpoint,
            "candidate_0_percent_endpoint_max_rgb": black_max,
            "50_frames_without_wait_median_ms": round(statistics.median(elapsed), 3),
            "timing_scope": "Offscreen GDI and Python palette generation; excludes game/proxy/presentation.",
            "result": "PASS",
        }
    finally:
        gdi.SelectObject(dc, old)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(dc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "source_exe_01/gundam.exe")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = audit(args.exe)
    result["gdi_reproduction"] = gdi_reproduction()
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)


if __name__ == "__main__":
    main()
