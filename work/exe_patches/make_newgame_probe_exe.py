"""Create a throw-away probe executable for reaching the New Game selector.

This is diagnostic only and is not part of the Phase 4 patch.  It patches the
Openmovi message-time gate and the already-known title-menu gate in a copy of
run/GT/gundam.exe, with exact before-byte checks.  source_exe/ is never used as
an output and is never overwritten.
"""

from __future__ import annotations

import argparse
from pathlib import Path

IMAGE_BASE = 0x00400000
SITES = (
    (0x00402F0E, bytes.fromhex("7E 05"), bytes.fromhex("90 90")),
    (0x004033B1, bytes.fromhex("0F 8E 79 02 00 00"), bytes.fromhex("90 90 90 90 90 90")),
)


def va_to_file_offset(data: bytes, va: int) -> int:
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("PE signature not found")
    section_count = int.from_bytes(data[pe + 6:pe + 8], "little")
    optional_size = int.from_bytes(data[pe + 20:pe + 22], "little")
    table = pe + 24 + optional_size
    rva = va - IMAGE_BASE
    for i in range(section_count):
        h = table + i * 40
        virtual_size = int.from_bytes(data[h + 8:h + 12], "little")
        virtual_address = int.from_bytes(data[h + 12:h + 16], "little")
        raw_size = int.from_bytes(data[h + 16:h + 20], "little")
        raw_address = int.from_bytes(data[h + 20:h + 24], "little")
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_address + rva - virtual_address
    raise ValueError(f"VA 0x{va:08X} is not in a PE section")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path, default=Path("run/GT/gundam.exe"))
    ap.add_argument("--dest", type=Path, default=Path("run/GT/gundam_newgame_probe.exe"))
    args = ap.parse_args()
    source = args.source.resolve()
    dest = args.dest.resolve()
    if source == dest:
        raise SystemExit("refusing to overwrite diagnostic input")
    if dest.exists():
        raise SystemExit(f"destination already exists: {dest}")
    data = bytearray(source.read_bytes())
    print(f"source: {source}")
    for va, expected, replacement in SITES:
        off = va_to_file_offset(data, va)
        actual = bytes(data[off:off + len(expected)])
        if actual != expected:
            raise SystemExit(
                f"unexpected VA 0x{va:08X} / file 0x{off:X}: "
                f"{actual.hex(' ')} != {expected.hex(' ')}"
            )
        print(f"VA 0x{va:08X} file 0x{off:X}: {actual.hex(' ')} -> {replacement.hex(' ')}")
        data[off:off + len(replacement)] = replacement
    dest.write_bytes(data)
    print(f"wrote diagnostic copy: {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
