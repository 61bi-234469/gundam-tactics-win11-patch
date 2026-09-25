"""Minimal movie-phase patch for gundam.exe.

The 2026-04-17 re-evaluation established that `gundam_phase1_audio.exe`
(audio patches only) already completes Win11 battles end-to-end. The only
remaining user-visible defect was a `Wrong DIB handle` MessageBox that
appears when the player's flagship is destroyed.

This script applies the single 3-byte patch that suppresses that
MessageBox by turning the null-DIB call path at `FUN_0040D3A0+0x0A` into
an early return. All other phase2 patches (ESI protect, stall guards,
QT reset, frame wrappers, dummy DIB, compositor bypass, runtime logger)
were removed because the baseline does not need them, and several of
them caused regressions (e.g. compositor bypass whited out WS vs WS).
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path


IMAGE_BASE = 0x00400000

# Location of the push arguments immediately before the MessageBoxA call
# in FUN_0040D3A0 (the DrawDIB wrapper's null-handle branch).
DRAW_DIB_NULL_GUARD_VA = 0x0040D3AA
DRAW_DIB_NULL_GUARD_EXPECTED = bytes.fromhex("6A 00 68 98 B2 43 00")
# pop edi ; pop esi ; ret ; nop ; nop ; nop
DRAW_DIB_NULL_GUARD_PATCH = bytes.fromhex("5F 5E C3 90 90 90")


def va_to_file_offset(data: bytes, va: int) -> int:
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe_off = int.from_bytes(data[0x3C:0x40], "little")
    if data[pe_off:pe_off + 4] != b"PE\x00\x00":
        raise ValueError("PE signature not found")
    coff = pe_off + 4
    num_sections = int.from_bytes(data[coff + 2:coff + 4], "little")
    opt_size = int.from_bytes(data[coff + 16:coff + 18], "little")
    sec_off = coff + 20 + opt_size
    rva = va - IMAGE_BASE
    for i in range(num_sections):
        hdr = sec_off + i * 40
        vsize = int.from_bytes(data[hdr + 8:hdr + 12], "little")
        vaddr = int.from_bytes(data[hdr + 12:hdr + 16], "little")
        rsize = int.from_bytes(data[hdr + 16:hdr + 20], "little")
        raddr = int.from_bytes(data[hdr + 20:hdr + 24], "little")
        span = max(vsize, rsize)
        if vaddr <= rva < vaddr + span:
            return raddr + (rva - vaddr)
    raise ValueError(f"VA 0x{va:08X} not contained in any section")


def apply(source: Path, dest: Path) -> None:
    data = bytearray(source.read_bytes())
    off = va_to_file_offset(bytes(data), DRAW_DIB_NULL_GUARD_VA)
    existing = bytes(data[off:off + len(DRAW_DIB_NULL_GUARD_EXPECTED)])
    if existing == DRAW_DIB_NULL_GUARD_PATCH:
        print(f"already patched at VA 0x{DRAW_DIB_NULL_GUARD_VA:08X} "
              f"(file offset 0x{off:X}); copying as-is")
    elif existing != DRAW_DIB_NULL_GUARD_EXPECTED:
        raise RuntimeError(
            f"unexpected bytes at VA 0x{DRAW_DIB_NULL_GUARD_VA:08X} "
            f"(file offset 0x{off:X}): {existing.hex(' ')}"
        )
    else:
        data[off:off + len(DRAW_DIB_NULL_GUARD_PATCH)] = DRAW_DIB_NULL_GUARD_PATCH
        print(f"patched VA 0x{DRAW_DIB_NULL_GUARD_VA:08X} "
              f"(file offset 0x{off:X}): "
              f"{DRAW_DIB_NULL_GUARD_EXPECTED.hex(' ')} -> "
              f"{DRAW_DIB_NULL_GUARD_PATCH.hex(' ')}")

    if dest.resolve() == source.resolve():
        source.write_bytes(bytes(data))
    else:
        shutil.copy2(source, dest)
        dest.write_bytes(bytes(data))
    print(f"wrote {dest}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("gundam_phase1_audio.exe"),
        help="Baseline exe to patch (default: gundam_phase1_audio.exe).",
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path("gundam_phase2_movie.exe"),
        help="Output exe path (default: gundam_phase2_movie.exe).",
    )
    args = parser.parse_args()

    if not args.source.exists():
        print(f"source not found: {args.source}")
        return 1
    apply(args.source, args.dest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
