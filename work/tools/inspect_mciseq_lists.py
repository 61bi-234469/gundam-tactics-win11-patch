"""Count used slots in mciseq.dll's global 500-entry list table of a 32-bit process.
Table: mciseq base + 0x63a0, 12-byte entries {elem_size, ?, head}; elem_size != 0 = in use
(mciseq 10.0.26100.x, SysWOW64; list create at +0x17c8 scans for the first zero size).
Usage: python inspect_mciseq_lists.py <pid> [interval_s]   (prints GetTickCount, used, used-with-nodes)
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


class ME32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE), ("szModule", ctypes.c_char * 256),
                ("szExePath", ctypes.c_char * 260)]


def mciseq_base(pid):
    snap = k32.CreateToolhelp32Snapshot(0x8 | 0x10, pid)  # MODULE | MODULE32
    me = ME32()
    me.dwSize = ctypes.sizeof(ME32)
    ok = k32.Module32First(wt.HANDLE(snap), ctypes.byref(me))
    base = None
    while ok:
        if me.szModule.lower() == b"mciseq.dll":
            base = me.modBaseAddr
            break
        ok = k32.Module32Next(wt.HANDLE(snap), ctypes.byref(me))
    k32.CloseHandle(wt.HANDLE(snap))
    return base


def read_table(h, base):
    buf = ctypes.create_string_buffer(500 * 12)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(base + 0x63A0), buf, len(buf), ctypes.byref(got)):
        return None
    used = nodes = 0
    for i in range(500):
        size, _, head = int.from_bytes(buf.raw[i * 12:i * 12 + 4], "little"), 0, \
            int.from_bytes(buf.raw[i * 12 + 8:i * 12 + 12], "little")
        if size:
            used += 1
            if head:
                nodes += 1
    return used, nodes


def main():
    pid = int(sys.argv[1])
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 0
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)
    base = None
    last = None
    while True:
        if base is None:
            base = mciseq_base(pid)
            if base is None and interval:
                r = ("unloaded", 0)
                if r != last:
                    print(f"{k32.GetTickCount()}	used=unloaded", flush=True)
                    last = r
        if base is not None:
            r = read_table(h, base)
            if r is None:  # unloaded (no sequencer open): find it again later
                base = None
                r = ("unloaded", 0)
            if r != last or not interval:
                print(f"{k32.GetTickCount()}\tused={r[0]}\twith_nodes={r[1]}", flush=True)
                last = r
        if not interval:
            break
        code = wt.DWORD()
        k32.GetExitCodeProcess(h, ctypes.byref(code))
        if code.value != 259:
            print("exit", code.value, flush=True)
            break
        time.sleep(interval)


if __name__ == "__main__":
    main()
