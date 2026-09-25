from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import sys


IMAGE_BASE = 0x400000


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    raw_address: int
    raw_size: int


def load_sections(data: bytes) -> list[Section]:
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise ValueError("PE header not found")

    number_of_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    size_of_optional_header = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    section_offset = e_lfanew + 24 + size_of_optional_header

    sections: list[Section] = []
    for index in range(number_of_sections):
        offset = section_offset + index * 40
        name = data[offset : offset + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        sections.append(
            Section(
                name=name,
                virtual_address=virtual_address,
                virtual_size=virtual_size,
                raw_address=raw_address,
                raw_size=raw_size,
            )
        )
    return sections


def va_to_offset(sections: list[Section], virtual_address: int) -> int:
    rva = virtual_address - IMAGE_BASE
    for section in sections:
        start = section.virtual_address
        end = start + max(section.virtual_size, section.raw_size)
        if start <= rva < end:
            return section.raw_address + (rva - start)
    raise ValueError(f"VA 0x{virtual_address:08x} is not inside any section")


def build_cave_bytes() -> bytes:
    return bytes.fromhex(
        "A1 90 DC 43 00"  # mov eax,[g_waveSampleData]
        "85 C0"  # test eax,eax
        "74 1D"  # jz skip_header_cleanup
        "50"  # push eax
        "FF 15 DC C2 44 00"  # call [GlobalUnlock]
        "A1 90 DC 43 00"  # mov eax,[g_waveSampleData]
        "50"  # push eax
        "FF 15 48 C3 44 00"  # call [GlobalFree]
        "C7 05 90 DC 43 00 00 00 00 00"  # mov [g_waveSampleData],0
        "A1 8C DC 43 00"  # skip_header_cleanup: mov eax,[g_waveSampleGlobal]
        "85 C0"  # test eax,eax
        "74 1D"  # jz done
        "50"  # push eax
        "FF 15 DC C2 44 00"  # call [GlobalUnlock]
        "A1 8C DC 43 00"  # mov eax,[g_waveSampleGlobal]
        "50"  # push eax
        "FF 15 48 C3 44 00"  # call [GlobalFree]
        "C7 05 8C DC 43 00 00 00 00 00"  # mov [g_waveSampleGlobal],0
        "C3"  # ret
    )


def build_midi_poll_cave_bytes(cave_va: int, jump_back_va: int) -> bytes:
    code = bytearray()
    code += bytes.fromhex("83 EC 24")  # sub esp,24
    code += bytes.fromhex("56")  # push esi
    code += bytes.fromhex("8B 0D D8 DD 43 00")  # mov ecx,[g_midiDeviceId]
    code += bytes.fromhex("85 C9")  # test ecx,ecx
    code += bytes.fromhex("74 33")  # jz exit
    code += bytes.fromhex("83 3D 50 A1 44 00 08")  # cmp dword [DAT_0044a150],8
    code += bytes.fromhex("75 2A")  # jnz exit

    call_site_va = cave_va + len(code)
    call_next_va = call_site_va + 5
    rel32_call = 0x0040C010 - call_next_va
    code += b"\xE8" + struct.pack("<i", rel32_call)  # call FUN_0040c010

    code += bytes.fromhex("8B 15 64 F6 43 00")  # mov edx,[g_lastMidiPollTick]
    code += bytes.fromhex("3B C2")  # cmp eax,edx
    code += bytes.fromhex("74 1B")  # je exit
    code += bytes.fromhex("3B C2")  # cmp eax,edx
    code += bytes.fromhex("72 07")  # jb store_now
    code += bytes.fromhex("8D 52 06")  # lea edx,[edx+6]
    code += bytes.fromhex("3B C2")  # cmp eax,edx
    code += bytes.fromhex("72 10")  # jb exit
    code += bytes.fromhex("A3 64 F6 43 00")  # mov [g_lastMidiPollTick],eax
    code += bytes.fromhex("8B 0D D8 DD 43 00")  # mov ecx,[g_midiDeviceId]

    jump_site_va = cave_va + len(code)
    jump_next_va = jump_site_va + 5
    rel32_jump = jump_back_va - jump_next_va
    code += b"\xE9" + struct.pack("<i", rel32_jump)  # jmp original body

    code += bytes.fromhex("5E")  # exit: pop esi
    code += bytes.fromhex("83 C4 24")  # add esp,24
    code += bytes.fromhex("C3")  # ret
    return bytes(code)


def patch_file(input_path: Path, output_path: Path) -> None:
    data = bytearray(input_path.read_bytes())
    sections = load_sections(data)

    color_check_va = 0x0040FBA2
    color_check_expected = bytes.fromhex("74 1E")
    color_check_patch = bytes.fromhex("EB 1E")

    store_header_va = 0x00421521
    store_header_expected = bytes.fromhex(
        "57 FF 15 DC C2 44 00 57 8B 2D 48 C3 44 00 FF D5"
    )
    store_header_patch = bytes.fromhex(
        "89 3D 90 DC 43 00 90 90 90 90 90 90 90 90 90 90"
    )

    cleanup_hook_va = 0x0042156C
    cleanup_hook_len = 0x26
    cleanup_cave_va = 0x00438420
    cleanup_cave_bytes = build_cave_bytes()

    midi_poll_hook_va = 0x00421600
    midi_poll_hook_len = 0x1B
    midi_poll_cave_va = 0x00438480
    midi_poll_resume_va = 0x0042161B
    midi_poll_cave_bytes = build_midi_poll_cave_bytes(midi_poll_cave_va, midi_poll_resume_va)

    store_header_offset = va_to_offset(sections, store_header_va)
    cleanup_hook_offset = va_to_offset(sections, cleanup_hook_va)
    cleanup_cave_offset = va_to_offset(sections, cleanup_cave_va)
    midi_poll_hook_offset = va_to_offset(sections, midi_poll_hook_va)
    midi_poll_cave_offset = va_to_offset(sections, midi_poll_cave_va)
    color_check_offset = va_to_offset(sections, color_check_va)

    actual_color_check = bytes(data[color_check_offset : color_check_offset + len(color_check_expected)])
    if actual_color_check != color_check_expected:
        raise ValueError(
            "unexpected bytes at color-depth bypass site: "
            f"{actual_color_check.hex(' ')}"
        )

    actual_store_header = bytes(data[store_header_offset : store_header_offset + len(store_header_expected)])
    if actual_store_header != store_header_expected:
        raise ValueError(
            "unexpected bytes at WAVEHDR retention patch site: "
            f"{actual_store_header.hex(' ')}"
        )

    actual_cleanup_hook = bytes(data[cleanup_hook_offset : cleanup_hook_offset + cleanup_hook_len])
    if actual_cleanup_hook[0] == 0xE9:
        raise ValueError("cleanup hook already looks patched")

    cave_region = bytes(data[cleanup_cave_offset : cleanup_cave_offset + len(cleanup_cave_bytes)])
    if any(cave_region):
        raise ValueError("code cave is not empty; refusing to overwrite")

    actual_midi_poll_hook = bytes(data[midi_poll_hook_offset : midi_poll_hook_offset + midi_poll_hook_len])
    expected_midi_poll_hook = bytes.fromhex(
        "83 EC 24 8B 0D D8 DD 43 00 85 C9 56 0F 84 82 00 00 00 83 3D 50 A1 44 00 08 75 79"
    )
    if actual_midi_poll_hook != expected_midi_poll_hook:
        raise ValueError(
            "unexpected bytes at MIDI poll hook site: "
            f"{actual_midi_poll_hook.hex(' ')}"
        )

    midi_poll_cave_region = bytes(
        data[midi_poll_cave_offset : midi_poll_cave_offset + len(midi_poll_cave_bytes)]
    )
    if any(midi_poll_cave_region):
        raise ValueError("MIDI poll code cave is not empty; refusing to overwrite")

    data[color_check_offset : color_check_offset + len(color_check_patch)] = color_check_patch
    data[store_header_offset : store_header_offset + len(store_header_patch)] = store_header_patch

    rel32 = cleanup_cave_va - (cleanup_hook_va + 5)
    cleanup_jump = b"\xE9" + struct.pack("<i", rel32)
    cleanup_patch = cleanup_jump + b"\x90" * (cleanup_hook_len - len(cleanup_jump))
    data[cleanup_hook_offset : cleanup_hook_offset + cleanup_hook_len] = cleanup_patch

    rel32 = midi_poll_cave_va - (midi_poll_hook_va + 5)
    midi_poll_jump = b"\xE9" + struct.pack("<i", rel32)
    midi_poll_patch = midi_poll_jump + b"\x90" * (midi_poll_hook_len - len(midi_poll_jump))
    data[midi_poll_hook_offset : midi_poll_hook_offset + midi_poll_hook_len] = midi_poll_patch

    data[cleanup_cave_offset : cleanup_cave_offset + len(cleanup_cave_bytes)] = cleanup_cave_bytes
    data[midi_poll_cave_offset : midi_poll_cave_offset + len(midi_poll_cave_bytes)] = midi_poll_cave_bytes

    output_path.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Apply the phase-1 audio patch to Gundam Tactics. "
            "The patch keeps the WAVEHDR alive until CloseWaveOutEffectPlayback runs."
        )
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("gundam.exe"),
        help="input executable (default: gundam.exe)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("gundam_phase1_audio.exe"),
        help="output executable (default: gundam_phase1_audio.exe)",
    )
    args = parser.parse_args()

    try:
        patch_file(args.input, args.output)
    except Exception as exc:
        print(f"patch failed: {exc}", file=sys.stderr)
        return 1

    print(f"patched executable written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
