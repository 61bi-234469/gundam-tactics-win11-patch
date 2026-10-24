"""Read-only snapshot of the v1.4.0 proxy's pending audio teardown queue.

No injection, suspension, audio commands, or writes to the target process.
Only accepts the known v1.4.0 DLL: offsets below are specific to that build.
The target keeps running; the queue can move while it is being read. Treat
counts as observations, not an atomic snapshot or an exhaustive leak count.
Usage: python work/tools/inspect_audio_teardown.py PID --output result.json
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import struct
import time

DLL_SHA256 = "6b395c569151843f6c36965b72bfbd819804a5fe76c3e284561c13bbe8fc8520"
# COFF: .bss RVA 0x2b000 + _g_audio_teardown_queue offset 0x14660.
QUEUE_RVA = 0x3F660
NODE_SIZE = 56  # 32-bit AudioTeardown; next is the last DWORD.


def snapshot(pid):
    kernel = c.WinDLL("kernel32", use_last_error=True)
    psapi = c.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p,
                                        c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.ReadProcessMemory.restype = w.BOOL
    psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(w.HMODULE),
                                         w.DWORD, c.POINTER(w.DWORD), w.DWORD]
    psapi.EnumProcessModulesEx.restype = w.BOOL
    psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]
    psapi.GetModuleFileNameExW.restype = w.DWORD
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        raise c.WinError(c.get_last_error())
    try:
        modules = (w.HMODULE * 1024)()
        needed = w.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, c.sizeof(modules),
                                         c.byref(needed), 3):
            raise c.WinError(c.get_last_error())
        if needed.value > c.sizeof(modules):
            raise RuntimeError("module list truncated")
        base = None
        for module in modules[:needed.value // c.sizeof(w.HMODULE)]:
            path = c.create_unicode_buffer(32768)
            if not psapi.GetModuleFileNameExW(handle, module, path, len(path)):
                continue
            if Path(path.value).name.lower() == "qtim32.dll":
                if hashlib.sha256(Path(path.value).read_bytes()).hexdigest() != DLL_SHA256:
                    raise RuntimeError("unsupported DLL; refusing hard-coded offsets")
                base = module
                break
        if base is None:
            raise RuntimeError("QTIM32.dll not loaded")

        def read(address, size):
            data = c.create_string_buffer(size)
            received = c.c_size_t()
            if not kernel.ReadProcessMemory(handle, address, data, size,
                                            c.byref(received)) or received.value != size:
                raise c.WinError(c.get_last_error())
            return data.raw

        def head():
            return struct.unpack("<I", read(base + QUEUE_RVA, 4))[0]

        samples = []
        for attempt in range(5):
            first = head()
            address = first
            seen = set()
            nodes = []
            error = None
            while address and len(nodes) < 20000:
                if address in seen:
                    error = "cycle or concurrent queue mutation"
                    break
                seen.add(address)
                try:
                    values = struct.unpack("<14I", read(address, NODE_SIZE))
                except OSError as exc:
                    error = str(exc)
                    break
                nodes.append({"address": hex(address), "wave_handle": hex(values[0]),
                              "header_flags": values[5], "buffer_bytes": values[10],
                              "prepared": values[11], "chunks": hex(values[12])})
                address = values[13]
            samples.append({"head": hex(first), "head_unchanged": head() == first,
                            "error": error, "truncated": bool(address),
                            "node_count": len(nodes),
                            "inqueue_prepared": sum(bool(n["header_flags"] & 0x10)
                                                     and n["prepared"] == 1 for n in nodes),
                            "buffer_bytes": sum(n["buffer_bytes"] for n in nodes),
                            "nodes": nodes})
            time.sleep(0.05)
        return {"pid": pid, "dll_sha256": DLL_SHA256,
                "queue_rva": hex(QUEUE_RVA), "samples": samples}
    finally:
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = snapshot(args.pid)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps([{k: v for k, v in sample.items() if k != "nodes"}
                      for sample in result["samples"]], indent=2))
