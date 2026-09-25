"""Negative tests for the MOV table boundary checks.

Each case starts from a known-good SMC movie, changes one structural fact, and
requires movdec.exe to return the normal parse-error code (1).  A timeout or
any other exit code is treated as a decoder failure rather than a pass.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path


CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl"}


def atom_list(data: bytes, start: int = 0, end: int | None = None):
    end = len(data) if end is None else end
    pos = start
    while pos < end:
        if end - pos < 8:
            raise ValueError(f"trailing bytes at {pos}")
        size = struct.unpack_from(">I", data, pos)[0]
        header = 8
        if size == 1:
            if end - pos < 16:
                raise ValueError("short extended atom")
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or size > end - pos:
            raise ValueError(f"invalid atom size at {pos}")
        atom_end = pos + size
        kind = data[pos + 4:pos + 8]
        yield pos, kind, header, atom_end
        if kind in CONTAINERS:
            yield from atom_list(data, pos + header, atom_end)
        pos = atom_end


def find_atom(data: bytes, kind: bytes) -> tuple[int, int, int, int]:
    for item in atom_list(data):
        if item[1] == kind:
            return item
    raise ValueError(f"missing {kind!r}")


def write_u32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", data, offset, value)


def cases(source: bytes) -> dict[str, bytes]:
    result = {"truncated_file": source[:-7]}

    stsc_pos, _, _, _ = find_atom(source, b"stsc")
    mutated = bytearray(source)
    # atom header (8) + full-box version/flags (4) -> entry_count
    write_u32(mutated, stsc_pos + 12, 0x20000001)
    result["stsc_count_overflow"] = bytes(mutated)

    stsz_pos, _, _, _ = find_atom(source, b"stsz")
    mutated = bytearray(source)
    write_u32(mutated, stsz_pos + 16, 0x7FFFFFFF)
    result["stsz_count_overflow"] = bytes(mutated)

    stts_pos, _, _, _ = find_atom(source, b"stts")
    mutated = bytearray(source)
    old_count = struct.unpack_from(">I", mutated, stts_pos + 16)[0]
    write_u32(mutated, stts_pos + 16, old_count + 1)
    result["stts_stsz_mismatch"] = bytes(mutated)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--movdec",
        type=Path,
        default=Path(__file__).resolve().with_name("movdec.exe"),
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=(Path(__file__).resolve().parents[2] / "run" / "GT01" /
                 "Movie" / "Bombl.mov"),
    )
    args = parser.parse_args()
    source = args.source.read_bytes()
    failures: list[str] = []
    root = (Path(__file__).resolve().parents[2] / "work" / "analysis" /
            f"malformed_mov_tests_{os.getpid()}")
    root.mkdir(parents=True, exist_ok=False)
    for name, payload in cases(source).items():
        movie = root / f"{name}.mov"
        output = root
        movie.write_bytes(payload)
        try:
            completed = subprocess.run(
                [str(args.movdec), str(movie), str(output)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
                check=False,
            )
        except subprocess.TimeoutExpired:
            failures.append(f"{name}: timeout")
            continue
        if completed.returncode != 1:
            failures.append(
                f"{name}: exit={completed.returncode} "
                f"stderr={completed.stderr.decode(errors='replace').strip()}"
            )
        else:
            print(f"PASS {name}: exit=1")
    if failures:
        for failure in failures:
            print(f"FAIL {failure}", file=sys.stderr)
        return 1
    print("malformed MOV negative tests: all PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
