"""Enumerate Win32 startup API IAT references in the 32-bit game binary.

This is a dependency-light fallback for environments where the repository's
documented Ghidra headless installation is unavailable.  It parses the PE
import directory and scans executable sections with Capstone, recording
``CALL [IAT]``/``JMP [IAT]`` references plus nearby function-call targets.
The output is deliberately machine-readable so it can be compared with a
future Ghidra headless run.
"""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32


IMAGE_BASE_DEFAULT = 0x00400000
TARGETS = {
    "GetVersion", "GetVersionExA", "GetVersionExW",
    "RegOpenKeyA", "RegOpenKeyW", "RegOpenKeyExA", "RegOpenKeyExW",
    "RegCreateKeyA", "RegCreateKeyW", "RegCreateKeyExA", "RegCreateKeyExW",
    "CreateFileA", "CreateFileW", "CreateFileMappingA", "CreateFileMappingW",
    "WriteFile", "SetEndOfFile", "FlushFileBuffers", "GetDeviceCaps",
    "GetPrivateProfileStringA", "CreateDirectoryA", "GetFileAttributesA",
    "GetFullPathNameA", "GetModuleFileNameA", "GetCurrentDirectoryA",
}


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    raw_address: int
    raw_size: int
    characteristics: int


@dataclass(frozen=True)
class ImportEntry:
    dll: str
    name: str
    iat_rva: int
    iat_va: int


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def read_c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        end = len(data)
    return data[offset:end].decode("ascii", "replace")


def parse_headers(data: bytes) -> tuple[int, list[Section], int, int]:
    if data[:2] != b"MZ":
        raise ValueError("not an MZ executable")
    pe = u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    coff = pe + 4
    section_count = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    optional = coff + 20
    magic = u16(data, optional)
    if magic != 0x10B:
        raise ValueError(f"expected PE32 optional header, got 0x{magic:x}")
    image_base = u32(data, optional + 28)
    data_directory = optional + 96
    import_rva = u32(data, data_directory + 8)
    import_size = u32(data, data_directory + 12)
    section_table = optional + optional_size
    sections: list[Section] = []
    for index in range(section_count):
        offset = section_table + index * 40
        name = data[offset:offset + 8].rstrip(b"\0").decode("ascii", "replace")
        sections.append(Section(
            name=name,
            virtual_size=u32(data, offset + 8),
            virtual_address=u32(data, offset + 12),
            raw_size=u32(data, offset + 16),
            raw_address=u32(data, offset + 20),
            characteristics=u32(data, offset + 36),
        ))
    return image_base, sections, import_rva, import_size


def rva_to_offset(sections: list[Section], rva: int) -> int:
    for section in sections:
        span = max(section.virtual_size, section.raw_size)
        if section.virtual_address <= rva < section.virtual_address + span:
            return section.raw_address + (rva - section.virtual_address)
    raise ValueError(f"RVA 0x{rva:x} is not in a section")


def parse_imports(data: bytes, image_base: int, sections: list[Section], import_rva: int,
                  import_size: int) -> list[ImportEntry]:
    if not import_rva:
        return []
    descriptor = rva_to_offset(sections, import_rva)
    entries: list[ImportEntry] = []
    for index in range(import_size // 20 + 1):
        current = descriptor + index * 20
        if current + 20 > len(data):
            break
        original_thunk, _timestamp, _forwarder, name_rva, first_thunk = struct.unpack_from(
            "<IIIII", data, current
        )
        if not any((original_thunk, name_rva, first_thunk)):
            break
        dll = read_c_string(data, rva_to_offset(sections, name_rva))
        thunk_rva = original_thunk or first_thunk
        ordinal = 0
        while True:
            thunk_offset = rva_to_offset(sections, thunk_rva + ordinal * 4)
            value = u32(data, thunk_offset)
            if value == 0:
                break
            iat_rva = first_thunk + ordinal * 4
            if value & 0x80000000:
                name = f"ordinal_{value & 0xFFFF}"
            else:
                hint_name = rva_to_offset(sections, value)
                name = read_c_string(data, hint_name + 2)
            entries.append(ImportEntry(dll, name, iat_rva, image_base + iat_rva))
            ordinal += 1
    return entries


def disassemble_xrefs(data: bytes, image_base: int, sections: list[Section],
                      imports: list[ImportEntry]) -> list[dict[str, object]]:
    by_iat = {entry.iat_va: entry for entry in imports if entry.name in TARGETS}
    if not by_iat:
        return []
    disassembler = Cs(CS_ARCH_X86, CS_MODE_32)
    disassembler.detail = True
    xrefs: list[dict[str, object]] = []
    for section in sections:
        if not (section.characteristics & 0x20000000):  # IMAGE_SCN_MEM_EXECUTE
            continue
        raw = data[section.raw_address:section.raw_address + section.raw_size]
        base_va = image_base + section.virtual_address
        for instruction in disassembler.disasm(raw, base_va):
            for operand in instruction.operands:
                if operand.type != 3:  # X86_OP_MEM
                    continue
                absolute = operand.mem.disp
                if operand.mem.base == 0 and operand.mem.index == 0:
                    entry = by_iat.get(absolute)
                    if entry:
                        xrefs.append({
                            "import": entry.name,
                            "dll": entry.dll,
                            "iat_va": f"0x{entry.iat_va:08x}",
                            "xref_va": f"0x{instruction.address:08x}",
                            "mnemonic": instruction.mnemonic,
                            "operands": instruction.op_str,
                            "bytes": instruction.bytes.hex(" "),
                        })
    return xrefs


def analyze(exe: Path) -> dict[str, object]:
    data = exe.read_bytes()
    image_base, sections, import_rva, import_size = parse_headers(data)
    imports = parse_imports(data, image_base, sections, import_rva, import_size)
    selected = [entry for entry in imports if entry.name in TARGETS]
    xrefs = disassemble_xrefs(data, image_base, sections, selected)
    return {
        "tool": "capstone_pe_iat_fallback",
        "executable": str(exe.resolve()),
        "image_base": f"0x{image_base:08x}",
        "sections": [asdict(section) for section in sections],
        "selected_imports": [asdict(entry) for entry in selected],
        "xref_count": len(xrefs),
        "xrefs": xrefs,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path("source_exe_01/gundam.exe"))
    parser.add_argument("--output", type=Path,
                        default=Path("work/analysis/startup_import_xrefs.json"))
    args = parser.parse_args()
    result = analyze(args.exe.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
    print(json.dumps({
        "output": str(args.output.resolve()),
        "selected_imports": len(result["selected_imports"]),
        "xref_count": result["xref_count"],
    }, ensure_ascii=False))
    for xref in result["xrefs"]:
        print(f"{xref['import']:20} {xref['xref_va']} {xref['mnemonic']} {xref['operands']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
