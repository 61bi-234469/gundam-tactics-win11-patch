"""Read-only executable checks and counterexamples for original game bugs.

Usage: python work/tools/audit_game_boundaries.py [--exe PATH]

This never starts the game, changes its executable, reads user saves, or
changes the system clock. Results are arithmetic models backed by checked
instruction bytes, not observations from a running game.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MASK = 0xFFFFFFFF


def pe_reader(data: bytes):
    if data[:2] != b"MZ":
        raise ValueError("Expected a PE executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Expected a PE signature")
    if struct.unpack_from("<H", data, pe + 24)[0] != 0x10B:
        raise ValueError("Expected PE32")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 52)[0]
    if base != 0x400000:
        raise ValueError("Unexpected image base")
    sections = []
    for index in range(count):
        offset = pe + 24 + optional_size + index * 40
        _, rva, size, raw = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((base + rva, size, raw))

    def read(address: int, size: int) -> bytes:
        for start, raw_size, offset in sections:
            if start <= address and address + size <= start + raw_size:
                result = data[offset + address - start:offset + address - start + size]
                if len(result) == size:
                    return result
        raise ValueError(f"Unbacked address: {address:#x}, size {size}")

    return read


def clock60(milliseconds: int) -> int:
    # LEA EAX,[EAX+EAX*2]; ADD EAX,EAX; XOR EDX,EDX; DIV 100.
    return ((milliseconds * 6) & MASK) // 100


def signed32(value: int) -> int:
    value &= MASK
    return value if value < 0x80000000 else value - 0x100000000


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "source_exe_01/gundam.exe")
    args = parser.parse_args()
    data = args.exe.read_bytes()
    read = pe_reader(data)
    expected = {
        0x40C010: "ff15bcc244008d0440b96400000003c02bd2f7f1c3",
        0x40C5B0: "56e85afaffff8b74240803f0e84ffaffff3bc676f75ec3",
        # Title menu: signed comparison against the previous message time.
        0x4033A9: "ff15b4c344003bc50f8e79020000",
        # Neither fread result is checked before proceeding with the load.
        0x4082FD: "e8ce90020083c41068580200006a00",
        0x408339: "e89290020083c41057e8b98d0200",
        0x4083DD: "b801000000",  # load returns success
    }
    checked = []
    for address, encoded in expected.items():
        wanted = bytes.fromhex(encoded)
        if read(address, len(wanted)) != wanted:
            raise ValueError(f"Unexpected instructions at {address:#x}; audit stopped")
        checked.append(f"0x{address:08X}")

    # Resolve each on-disk IAT thunk, rather than just searching for a name.
    for slot, name in [(0x44C2BC, b"GetTickCount"), (0x44C3B4, b"GetMessageTime")]:
        name_rva = struct.unpack("<I", read(slot, 4))[0]
        if read(0x400000 + name_rva + 2, len(name) + 1) != name + b"\0":
            raise ValueError(f"Unexpected clock import at {slot:#x}")

    first_wrap_ms = (1 << 32) // 6 + 1
    maximum_clock = (MASK - 1) // 100  # multiplication can only yield even values
    wait_start_ms = first_wrap_ms - 400
    wait_ticks = 30
    deadline = clock60(wait_start_ms) + wait_ticks
    if not deadline > maximum_clock:
        raise AssertionError("Counterexample does not prove an unreachable deadline")

    # The proxy's current input fix preserves a raw timestamp that is newer
    # by modular subtraction; the game's signed absolute comparison still fails.
    old_raw, new_raw = 0x7FFFFFF0, 0x80000010
    modular_delta = signed32(new_raw - old_raw)
    accepted = signed32(new_raw) > signed32(old_raw)
    if not (modular_delta == 32 and not accepted):
        raise AssertionError("Signed message-time counterexample failed")

    # Synthetic complete header + missing pilot body; no fixture is written.
    header = bytearray(40)
    struct.pack_into("<HH", header, 0, 1, 0)
    header[13:37] = b"\xff" * 24
    synthetic_save = bytes(header)
    missing_pilot_bytes = 50 * 12 - len(synthetic_save[40:640])
    if missing_pilot_bytes != 600:
        raise AssertionError("Short-save counterexample failed")

    result = {
        "sha256": hashlib.sha256(data).hexdigest(),
        "method": "checked instruction sites + arithmetic/input-shape models; no game execution",
        "checked_sites": checked,
        "timer_wrap": {
            "first_wrap_ms": first_wrap_ms,
            "first_wrap_days": first_wrap_ms / 86400000,
            "before": clock60(first_wrap_ms - 1),
            "after": clock60(first_wrap_ms),
            "maximum_possible_clock": maximum_clock,
            "wait_input": {"uptime_ms": wait_start_ms, "delay_ticks": wait_ticks},
            "deadline": deadline,
            "unreachable_deadline": deadline > maximum_clock,
        },
        "message_time_sign": {
            "sign_boundary_ms": 1 << 31,
            "sign_boundary_days": (1 << 31) / 86400000,
            "old_signed": signed32(old_raw),
            "new_signed": signed32(new_raw),
            "elapsed_ms": modular_delta,
            "game_accepts_new_message": accepted,
        },
        "short_save": {
            "synthetic_length": len(synthetic_save),
            "expected_nonempty_length": 640,
            "unread_pilot_bytes": missing_pilot_bytes,
            "limitation": "load failure handling verified statically; heap contents and GUI symptoms untested",
        },
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
