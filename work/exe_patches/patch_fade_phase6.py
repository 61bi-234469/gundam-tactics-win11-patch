"""Experimental Phase 6: let the palette fade reach its exact endpoints.

``FUN_004090F0`` runs ``for (counter = 0; counter < 100; counter += 2)`` and
scales every colour-table entry by ``percent / 100`` before each
``StretchDIBits``.  Fade-out uses ``100 - counter`` and fade-in uses
``counter``, so the original frames are 100..2 % and 0..98 %.  The last
fade-out frame keeps white at RGB 5, which leaves a dark residual image of the
old scene.

This patch changes only the loop condition at VA 0x00409295 from ``jl`` to
``jle`` (``0F 8C`` -> ``0F 8E``, same rel32).  The loop then runs 51 times:
fade-out 100..0 % (last frame fully black) and fade-in 0..100 %.  The counter
is used only as the multiplier in ``byte * percent / 100``, so 100 cannot
overflow (max 25500 / 100 = 255).  Timing and frame pacing are unchanged;
that is a separate hypothesis.

Revert: write ``0F 8C`` back at the same address (or rebuild from Phase 5).
The input is never overwritten; the original bytes are guarded.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from patch_display_phase3 import expected_bytes, va_to_file_offset


# add word [esp+12h],2 ; cmp word [esp+12h],64h ; jl 0x004091C2
LOOP_VA = 0x00409289
LOOP_EXPECTED = bytes.fromhex("66 83 44 24 12 02 66 83 7C 24 12 64 0F 8C 27 FF FF FF")
LOOP_REPLACEMENT = bytes.fromhex("66 83 44 24 12 02 66 83 7C 24 12 64 0F 8E 27 FF FF FF")


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
    offset = va_to_file_offset(data, LOOP_VA)
    expected_bytes(data, offset, LOOP_EXPECTED, f"fade loop VA 0x{LOOP_VA:08X}")
    data[offset : offset + len(LOOP_REPLACEMENT)] = LOOP_REPLACEMENT

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"wrote {destination}")
    print(
        f"fade loop VA 0x{LOOP_VA:08X} file 0x{offset:X}: "
        f"{LOOP_EXPECTED.hex(' ')} -> {LOOP_REPLACEMENT.hex(' ')}"
    )


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
