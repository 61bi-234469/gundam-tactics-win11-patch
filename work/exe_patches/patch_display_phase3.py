"""Apply the accepted Phase 3 display fix to a copied executable.

The default and release path is part ``b`` only: route only
``FUN_004090F0``'s ``StretchDIBits`` call through a verified zero code cave
and make the copied ``BITMAPINFO`` height negative.  Part ``a`` is retained
only as an explicit hypothesis reproducer for ``--part a``.

Part a is rejected for release: 不採用(実画面 A/B で PUSH 層反転)。
The input is never overwritten. Every modified instruction and code cave is
guarded by its expected original bytes before a separate destination is
written. Part ``b`` is intentionally limited to caller return VA 0x00409289;
the 0x00407392 caller remains unchanged.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


IMAGE_BASE = 0x00400000
STRETCH_DIBITS_IAT = 0x0044C278
STRETCH_CALL_EXPECTED = bytes.fromhex("FF 15 78 C2 44 00")

PART_A_VA = 0x0040F94F
PART_A_EXPECTED = bytes.fromhex("F7 D8 A3 C8 9A 44 00")
PART_A_REPLACEMENT = bytes.fromhex("90 90")

PART_B_CALL_VA = 0x00409283
PART_B_RESUME_VA = 0x00409289
PART_B_CAVE_VA = 0x00438550
PART_B_HEIGHT_NEGATIVE = bytes.fromhex("60 FE FF FF")


def va_to_file_offset(data: bytes, va: int) -> int:
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("PE signature not found")
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    section_table = pe + 24 + optional_size
    rva = va - IMAGE_BASE
    for index in range(section_count):
        section = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from(
            "<IIII", data, section + 8
        )
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            return raw_address + (rva - virtual_address)
    raise ValueError(f"VA 0x{va:08X} is not inside a PE section")


def expected_bytes(data: bytes, offset: int, expected: bytes, label: str) -> bytes:
    actual = bytes(data[offset : offset + len(expected)])
    if actual != expected:
        raise ValueError(
            f"{label} guard mismatch at file offset 0x{offset:X}: "
            f"{actual.hex(' ')}; expected {expected.hex(' ')}"
        )
    return actual


def build_part_b_cave(cave_va: int, resume_va: int) -> bytes:
    """Normalize the copied BMI height, call the original API, and resume."""

    code = bytearray()
    code += bytes.fromhex("8B 4C 24 28")  # mov ecx,[esp+28] (BITMAPINFO *)
    code += bytes.fromhex("C7 41 08") + PART_B_HEIGHT_NEGATIVE  # [ecx+8] = -416
    code += bytes.fromhex("FF 15") + struct.pack("<I", STRETCH_DIBITS_IAT)
    jump_next = cave_va + len(code) + 5
    code += b"\xE9" + struct.pack("<i", resume_va - jump_next)
    return bytes(code)


def canonical_part(value: str) -> str:
    if value.lower() in {"a", "b"}:
        return value.lower()
    raise ValueError(
        "Phase 3 accepts only --part b (default) or explicit --part a; "
        "a+b is not an accepted release configuration"
    )


def patch(source: Path, destination: Path, part: str) -> None:
    source = source.resolve()
    destination = destination.resolve()
    if source == destination:
        raise ValueError("refusing to overwrite the input executable")
    if destination.exists():
        raise FileExistsError(f"destination already exists: {destination}")
    if not source.is_file():
        raise FileNotFoundError(source)

    data = bytearray(source.read_bytes())
    applied: list[str] = []

    if part == "a":
        # 不採用(実画面 A/B で PUSH 層反転)。診断用に --part a のみ許可。
        offset = va_to_file_offset(data, PART_A_VA)
        before = expected_bytes(
            data, offset, PART_A_EXPECTED, f"part a VA 0x{PART_A_VA:08X}"
        )
        data[offset : offset + len(PART_A_REPLACEMENT)] = PART_A_REPLACEMENT
        applied.append(
            f"part a VA 0x{PART_A_VA:08X} file 0x{offset:X}: "
            f"{before[:2].hex(' ')} -> {PART_A_REPLACEMENT.hex(' ')}"
        )

    if part == "b":
        call_offset = va_to_file_offset(data, PART_B_CALL_VA)
        cave_offset = va_to_file_offset(data, PART_B_CAVE_VA)
        cave = build_part_b_cave(PART_B_CAVE_VA, PART_B_RESUME_VA)
        expected_bytes(
            data, call_offset, STRETCH_CALL_EXPECTED,
            f"part b call VA 0x{PART_B_CALL_VA:08X}"
        )
        expected_bytes(
            data, cave_offset, b"\x00" * len(cave),
            f"part b code cave VA 0x{PART_B_CAVE_VA:08X}"
        )
        jump = b"\xE9" + struct.pack(
            "<i", PART_B_CAVE_VA - (PART_B_CALL_VA + 5)
        ) + b"\x90"
        data[call_offset : call_offset + len(jump)] = jump
        data[cave_offset : cave_offset + len(cave)] = cave
        applied.append(
            f"part b call VA 0x{PART_B_CALL_VA:08X} file 0x{call_offset:X}: "
            f"{STRETCH_CALL_EXPECTED.hex(' ')} -> {jump.hex(' ')}"
        )
        applied.append(
            f"part b cave VA 0x{PART_B_CAVE_VA:08X} file 0x{cave_offset:X}: "
            f"{len(cave)} zero bytes -> {cave.hex(' ')}; "
            f"resume 0x{PART_B_RESUME_VA:08X}"
        )

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"wrote {destination}")
    print(f"part={part}")
    for item in applied:
        print(item)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path, required=True,
        help="input executable; it is never overwritten"
    )
    parser.add_argument(
        "--dest", "--output", dest="dest", type=Path, required=True,
        help="separate output executable"
    )
    parser.add_argument(
        "--part",
        default="b",
        choices=("a", "b"),
        help="Phase 3 sub-patch selection (default: b; a is diagnostic-only)",
    )
    args = parser.parse_args()
    try:
        patch(args.source, args.dest, canonical_part(args.part))
    except Exception as exc:
        print(f"patch failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
