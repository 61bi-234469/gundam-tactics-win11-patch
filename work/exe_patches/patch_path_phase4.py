"""Apply the Phase 4 install-path fix to a copied executable.

``FUN_00430F60(buf, 0x50)`` is the game's only current-directory source: a
wrapper around the CRT ``_getcwd``.  All 14 callers build
``cwd + "\\" + relative asset`` in an 80-byte stack buffer, so an install
directory longer than 54 ANSI/MBCS bytes overflows it (Jfont load error,
SIDE SELECTION never drawn).  The wrapper is replaced with code that stores
``"."`` in ``buf`` and returns it, so every asset is opened as
``.\\<relative>`` from the current directory.

The input is never overwritten; the original wrapper bytes are guarded.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from patch_display_phase3 import expected_bytes, va_to_file_offset


GETCWD_WRAPPER_VA = 0x00430F60
GETCWD_WRAPPER_EXPECTED = bytes.fromhex(
    "8B 44 24 08 8B 4C 24 04 50 51 6A 00 E8 0F 00 00 00 83 C4 0C C3"
)
GETCWD_WRAPPER_REPLACEMENT = bytes.fromhex(
    "8B 44 24 04"      # mov eax,[esp+4]      ; buf
    "66 C7 00 2E 00"   # mov word [eax],'.'   ; ".\0"
    "C3"               # ret                  ; cdecl, returns buf
).ljust(len(GETCWD_WRAPPER_EXPECTED), b"\xCC")


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
    offset = va_to_file_offset(data, GETCWD_WRAPPER_VA)
    expected_bytes(
        data, offset, GETCWD_WRAPPER_EXPECTED,
        f"getcwd wrapper VA 0x{GETCWD_WRAPPER_VA:08X}"
    )
    data[offset : offset + len(GETCWD_WRAPPER_REPLACEMENT)] = GETCWD_WRAPPER_REPLACEMENT

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"wrote {destination}")
    print(
        f"getcwd wrapper VA 0x{GETCWD_WRAPPER_VA:08X} file 0x{offset:X}: "
        f"{GETCWD_WRAPPER_EXPECTED.hex(' ')} -> {GETCWD_WRAPPER_REPLACEMENT.hex(' ')}"
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
