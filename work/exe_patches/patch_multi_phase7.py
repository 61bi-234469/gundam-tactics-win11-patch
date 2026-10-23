"""Phase 7: let qtim_compat.ini allow several copies of the game at once.

WinMain's helper ``FUN_0040F100`` starts with

    0x0040F10E  call [FindWindowA]      ; FindWindowA("Gundam", NULL)
    0x0040F114  test eax, eax
    0x0040F116  je   0x0040F13A         ; 74 22: no other copy, start
    0x0040F118  push 10h                ; MessageBoxA(hWnd = eax, "already
    0x0040F11A  push 0x0043D6F8 ...     ;   running", "ERROR", MB_ICONHAND)
                ...                     ; return -1

so a second copy (01 or 96 edition, which share this executable) showed the
ERROR box and quit.  The check runs before the window class is registered and
before the QuickTime proxy is loaded, so the proxy cannot decide it.

This patch replaces ``test eax,eax; je; push 10h`` (6 bytes at 0x0040F114)
with ``jmp CAVE; nop`` and adds a code cave in the zero slack of .text after
the Phase 1 MIDI cave.  The cave keeps the original behaviour unless the
game folder's qtim_compat.ini says otherwise:

    test eax, eax ; je 0x0040F13A        ; no other copy: start as before
    push eax                             ; keep the other window's handle
    sub  esp, 8 ; mov ecx, esp
    GetPrivateProfileStringA("game", "multi_instance", "0", ecx, 4,
                             ".\\qtim_compat.ini")   ; imported by the game
    mov  cx, [esp] ; add esp, 8 ; pop eax
    cmp  cx, '1'   ; je 0x0040F13A       ; exactly "1" (and its NUL): start
                                         ; as a second copy ("10" does not)
    push 10h ; jmp 0x0040F11A            ; otherwise the original ERROR path

``.\\qtim_compat.ini`` is the INI the proxy reads (the game folder, which is
the working folder the game needs anyway).  ``[game] multi_instance`` is set
in that file only (it is not in the menu) and is off by default, as in the
original.  Every copy uses the same save folder (Gundam.ini DataDir); the
release README says so.

Revert: write ``85 C0 74 22 6A 10`` back at 0x0040F114 and zero the cave (or
rebuild from Phase 6).  The input is never overwritten; the original bytes
and the empty cave are guarded.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from patch_display_phase3 import expected_bytes, va_to_file_offset


CHECK_VA = 0x0040F114
# test eax,eax ; je 0x0040F13A ; push 10h ; push 0x0043D6F8 ("ERROR")
CHECK_EXPECTED = bytes.fromhex("85 C0 74 22 6A 10 68 F8 D6 43 00")
START_VA = 0x0040F13A        # the start-up path after the check
ERROR_PATH_VA = 0x0040F11A   # push 0x0043D6F8, after "push 10h"
GET_PROFILE_STRING_IAT = 0x0044C2C0  # [GetPrivateProfileStringA]
CAVE_VA = 0x004384D0         # zero slack 0x004384C6..0x00438550
CAVE_LIMIT_VA = 0x00438550   # Phase 3 part b cave starts here

SECTION = b"game\0"
KEY = b"multi_instance\0"
DEFAULT = b"0\0"
INI_PATH = b".\\qtim_compat.ini\0"


def rel32(from_va: int, length: int, target: int) -> bytes:
    return struct.pack("<i", target - (from_va + length))


def build_cave() -> bytes:
    code = bytearray()

    def va() -> int:
        return CAVE_VA + len(code)

    # The strings follow the code; their addresses are fixed up below.
    code += bytes.fromhex("85 C0")                      # test eax, eax
    code += b"\x0F\x84" + rel32(va(), 6, START_VA)      # je start
    code += bytes.fromhex("50")                         # push eax
    code += bytes.fromhex("83 EC 08")                   # sub esp, 8
    code += bytes.fromhex("8B CC")                      # mov ecx, esp
    fix_ini = len(code) + 1
    code += b"\x68\0\0\0\0"                             # push ini path
    code += bytes.fromhex("6A 04")                      # push 4
    code += bytes.fromhex("51")                         # push ecx
    fix_default = len(code) + 1
    code += b"\x68\0\0\0\0"                             # push "0"
    fix_key = len(code) + 1
    code += b"\x68\0\0\0\0"                             # push key
    fix_section = len(code) + 1
    code += b"\x68\0\0\0\0"                             # push section
    code += b"\xFF\x15" + struct.pack("<I", GET_PROFILE_STRING_IAT)
    code += bytes.fromhex("66 8B 0C 24")                # mov cx, [esp]
    code += bytes.fromhex("83 C4 08")                   # add esp, 8
    code += bytes.fromhex("58")                         # pop eax
    code += bytes.fromhex("66 83 F9 31")                # cmp cx, '1' (+NUL)
    code += b"\x0F\x84" + rel32(va(), 6, START_VA)      # je start
    code += bytes.fromhex("6A 10")                      # push 10h
    code += b"\xE9" + rel32(va(), 5, ERROR_PATH_VA)     # jmp error path

    strings = {}
    for name, text in (("section", SECTION), ("key", KEY),
                       ("default", DEFAULT), ("ini", INI_PATH)):
        strings[name] = CAVE_VA + len(code)
        code += text
    for offset, name in ((fix_ini, "ini"), (fix_default, "default"),
                         (fix_key, "key"), (fix_section, "section")):
        code[offset:offset + 4] = struct.pack("<I", strings[name])
    if CAVE_VA + len(code) > CAVE_LIMIT_VA:
        raise ValueError("Phase 7 cave does not fit")
    return bytes(code)


def check_replacement() -> bytes:
    # jmp CAVE ; nop (the operand byte of the old "push 10h")
    return b"\xE9" + rel32(CHECK_VA, 5, CAVE_VA) + b"\x90"


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
    cave = build_cave()
    check_offset = va_to_file_offset(data, CHECK_VA)
    cave_offset = va_to_file_offset(data, CAVE_VA)
    expected_bytes(data, check_offset, CHECK_EXPECTED,
                   f"single-instance check VA 0x{CHECK_VA:08X}")
    expected_bytes(data, cave_offset, bytes(len(cave)),
                   f"Phase 7 code cave VA 0x{CAVE_VA:08X}")
    replacement = check_replacement()
    data[check_offset:check_offset + len(replacement)] = replacement
    data[cave_offset:cave_offset + len(cave)] = cave

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"wrote {destination}")
    print(f"check VA 0x{CHECK_VA:08X} file 0x{check_offset:X}: "
          f"{CHECK_EXPECTED[:6].hex(' ')} -> {replacement.hex(' ')}")
    print(f"code cave VA 0x{CAVE_VA:08X} file 0x{cave_offset:X} "
          f"({len(cave)} bytes): {cave.hex(' ')}")


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
