"""Apply the Phase 5 stretch-mode fix to a copied executable.

``FUN_00407200`` loads a full-screen BMP and draws it with
``StretchDIBits(hdc, x, y, w, h, 0, 0, biWidth, biHeight, ...)``.  The game
never calls ``SetStretchBltMode``, so the DC keeps the default
``BLACKONWHITE``, which ANDs the colour values of the columns it drops.
Every asset is drawn 1:1 except ``Bmp\\HERBOR\\MUSA.DOC`` (the Musai harbor
screen), which is 587 pixels wide instead of 576: 11 columns (dst x = 0, 52,
104, ... 523) receive the AND of two 8bpp palette indices and show up as
white/cyan/magenta vertical lines.  This is original-data behaviour, not a
Win11 regression.

The ``call [StretchDIBits]`` at VA 0x0040738C is redirected to a code cave
that resolves ``SetStretchBltMode`` through ``GetModuleHandleA("GDI32.DLL")``
+ ``GetProcAddress`` (it is not imported), sets ``COLORONCOLOR`` on the
destination DC and tail-jumps to ``StretchDIBits`` with the untouched
arguments.  ``COLORONCOLOR`` only affects shrinking blits; 1:1 blits are
byte-identical.

The input is never overwritten; original bytes and the empty cave are guarded.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from patch_display_phase3 import expected_bytes, va_to_file_offset


CALL_VA = 0x0040738C
CALL_EXPECTED = bytes.fromhex("FF 15 78 C2 44 00")  # call [StretchDIBits]
CAVE_VA = 0x00438580
CAVE_LIMIT_VA = 0x00438600                          # end of .text raw data

IAT_GET_MODULE_HANDLE_A = 0x0044C31C
IAT_GET_PROC_ADDRESS = 0x0044C360
IAT_STRETCH_DIBITS = 0x0044C278
COLORONCOLOR = 3


def build_cave() -> bytes:
    str_gdi = CAVE_VA + 48
    str_fn = str_gdi + len(b"GDI32.DLL\0")
    code = b"".join((
        b"\x68" + struct.pack("<I", str_gdi),                      # push "GDI32.DLL"
        b"\xFF\x15" + struct.pack("<I", IAT_GET_MODULE_HANDLE_A),  # call [GetModuleHandleA]
        b"\x85\xC0",                                               # test eax,eax
        b"\x74\x18",                                               # jz  tail
        b"\x68" + struct.pack("<I", str_fn),                       # push "SetStretchBltMode"
        b"\x50",                                                   # push eax
        b"\xFF\x15" + struct.pack("<I", IAT_GET_PROC_ADDRESS),     # call [GetProcAddress]
        b"\x85\xC0",                                               # test eax,eax
        b"\x74\x08",                                               # jz  tail
        b"\x6A" + bytes([COLORONCOLOR]),                           # push COLORONCOLOR
        b"\xFF\x74\x24\x08",                                       # push [esp+8]  ; hdc
        b"\xFF\xD0",                                               # call eax
        b"\xFF\x25" + struct.pack("<I", IAT_STRETCH_DIBITS),       # tail: jmp [StretchDIBits]
    ))
    assert len(code) == 45, len(code)
    cave = code.ljust(48, b"\xCC") + b"GDI32.DLL\0SetStretchBltMode\0"
    assert CAVE_VA + len(cave) <= CAVE_LIMIT_VA
    return cave


CAVE_BYTES = build_cave()
CALL_REPLACEMENT = b"\xE8" + struct.pack("<i", CAVE_VA - (CALL_VA + 5)) + b"\x90"


def patch(source: Path, destination: Path) -> None:
    source = source.resolve()
    destination = destination.resolve()
    if source == destination:
        raise ValueError("refusing to overwrite the input executable")
    if destination.exists():
        raise FileExistsError(f"destination already exists: {destination}")
    if not source.is_file():
        raise FileNotFoundError(source)

    data = bytearray(source.read_bytes())
    call_offset = va_to_file_offset(data, CALL_VA)
    cave_offset = va_to_file_offset(data, CAVE_VA)
    expected_bytes(data, call_offset, CALL_EXPECTED, f"StretchDIBits call VA 0x{CALL_VA:08X}")
    expected_bytes(
        data, cave_offset, bytes(len(CAVE_BYTES)), f"Phase 5 code cave VA 0x{CAVE_VA:08X}"
    )
    data[call_offset : call_offset + len(CALL_REPLACEMENT)] = CALL_REPLACEMENT
    data[cave_offset : cave_offset + len(CAVE_BYTES)] = CAVE_BYTES

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"wrote {destination}")
    print(
        f"StretchDIBits call VA 0x{CALL_VA:08X} file 0x{call_offset:X}: "
        f"{CALL_EXPECTED.hex(' ')} -> {CALL_REPLACEMENT.hex(' ')}"
    )
    print(f"code cave VA 0x{CAVE_VA:08X} file 0x{cave_offset:X}: {CAVE_BYTES.hex(' ')}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--dest", type=Path, required=True)
    args = parser.parse_args()
    try:
        patch(args.source, args.dest)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
