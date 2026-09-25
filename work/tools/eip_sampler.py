"""Launch the game, drive clicks, then sample the main thread's EIP to see where it spins.

Usage: python eip_sampler.py [--exe gundam.exe] [--steps "wait:5,click:0.5,0.5,..."] [--samples 40]
Requires no third-party modules (ctypes only). 64-bit Python samples the 32-bit
game via Wow64GetThreadContext.  In addition to EIP, the sampler records ESP/EBP,
the stack around ESP, and selected FUN_00403260 globals.  This makes it possible
to distinguish a GDI call loop from a stale timer/message-time gate without
patching or instrumenting the executable.
"""
import argparse, ctypes, ctypes.wintypes as wt, subprocess, time, collections, os, sys

k32 = ctypes.windll.kernel32
u32 = ctypes.windll.user32
THREAD_ALL = 0x1F03FF
CONTEXT_CONTROL = 0x10001
CONTEXT_INTEGER = 0x10002

class WOW64_FLOATING_SAVE_AREA(ctypes.Structure):
    _fields_ = [("ControlWord", wt.DWORD), ("StatusWord", wt.DWORD), ("TagWord", wt.DWORD), ("ErrorOffset", wt.DWORD),
                ("ErrorSelector", wt.DWORD), ("DataOffset", wt.DWORD), ("DataSelector", wt.DWORD),
                ("RegisterArea", ctypes.c_ubyte * 80), ("Cr0NpxState", wt.DWORD)]

class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [("ContextFlags", wt.DWORD), ("Dr0", wt.DWORD), ("Dr1", wt.DWORD), ("Dr2", wt.DWORD), ("Dr3", wt.DWORD),
                ("Dr6", wt.DWORD), ("Dr7", wt.DWORD), ("FloatSave", WOW64_FLOATING_SAVE_AREA),
                ("SegGs", wt.DWORD), ("SegFs", wt.DWORD), ("SegEs", wt.DWORD), ("SegDs", wt.DWORD),
                ("Edi", wt.DWORD), ("Esi", wt.DWORD), ("Ebx", wt.DWORD), ("Edx", wt.DWORD), ("Ecx", wt.DWORD), ("Eax", wt.DWORD),
                ("Ebp", wt.DWORD), ("Eip", wt.DWORD), ("SegCs", wt.DWORD), ("EFlags", wt.DWORD), ("Esp", wt.DWORD), ("SegSs", wt.DWORD),
                ("ExtendedRegisters", ctypes.c_ubyte * 512)]

class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]

def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
    out = []
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid: out.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, ctypes.byref(te)): break
    k32.CloseHandle(snap)
    return out

def main_window(pid):
    found = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(h, l):
        p = wt.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            buf = ctypes.create_unicode_buffer(256); u32.GetWindowTextW(h, buf, 256)
            if buf.value: found.append(h)
        return True
    u32.EnumWindows(cb, 0)
    return found[0] if found else None

def click(h, fx, fy, settle=0.2):
    # Fractions are relative to the client area, not the non-client title bar.
    # The game menu rectangles are expressed in client coordinates (576x416).
    r = wt.RECT(); u32.GetClientRect(h, ctypes.byref(r))
    pt = wt.POINT(int((r.right - r.left) * fx), int((r.bottom - r.top) * fy))
    u32.ClientToScreen(h, ctypes.byref(pt))
    x, y = pt.x, pt.y
    u32.SetForegroundWindow(h); u32.SetCursorPos(x, y); time.sleep(settle)
    u32.mouse_event(2, 0, 0, 0, 0); time.sleep(settle / 2); u32.mouse_event(4, 0, 0, 0, 0)

def press_key(h, name):
    keys = {"escape": 0x1B, "esc": 0x1B, "enter": 0x0D, "return": 0x0D,
            "space": 0x20}
    vk = keys.get(name.lower())
    if vk is None:
        raise ValueError("unsupported key: %s" % name)
    u32.SetForegroundWindow(h)
    u32.keybd_event(vk, 0, 0, 0)
    u32.keybd_event(vk, 0, 2, 0)

def read_stack(hproc, esp, n=16):
    buf = (wt.DWORD * n)(); got = ctypes.c_size_t()
    if k32.ReadProcessMemory(hproc, ctypes.c_void_p(esp), buf, ctypes.sizeof(buf), ctypes.byref(got)):
        return list(buf)
    return []

def read_dwords(hproc, address, n=1):
    if address < 0 or n <= 0:
        return []
    buf = (wt.DWORD * n)(); got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(
        hproc, ctypes.c_void_p(address), buf, ctypes.sizeof(buf), ctypes.byref(got)
    )
    return list(buf) if ok and got.value >= ctypes.sizeof(buf) else []

def read_global(hproc, address):
    values = read_dwords(hproc, address, 1)
    return values[0] if values else None

def hex_or_none(value):
    return "?" if value is None else "%08X" % value

def read_target_locals(hproc, eip, esp):
    # FUN_00403260 reserves 0x44 bytes, then saves EBX/ESI/EDI/EBP.  At the
    # steady-state loop the decompiler's locals are therefore at ESP+0x10:
    # RECT local_44, RECT local_34, local_24, local_20, MSG local_1c.
    if not (0x00403260 <= eip <= 0x0040363F):
        return {}
    values = read_dwords(hproc, esp + 0x10, 0x28 // 4)
    if len(values) < 0x28 // 4:
        return {}
    return {
        "local_44_left": values[0], "local_44_top": values[1],
        "local_44_right": values[2], "local_44_bottom": values[3],
        "local_34_left": values[4], "local_34_top": values[5],
        "local_34_right": values[6], "local_34_bottom": values[7],
        "local_24": values[8], "local_20": values[9],
        "msg_message": values[10], "msg_hwnd": values[11],
    }

ap = argparse.ArgumentParser()
ap.add_argument("--game-dir", default=os.path.join(os.path.dirname(__file__), "..", "..", "run", "GT01"))
ap.add_argument("--exe", default="gundam.exe",
                help="executable filename inside --game-dir (for A/B copies such as gundam_ng.exe)")
ap.add_argument("--steps", default="wait:5,click:0.5,0.5,wait:3,click:0.5,0.58,wait:1.5,click:0.5,0.58,wait:4")
ap.add_argument("--samples", type=int, default=40)
ap.add_argument("--stack-dwords", type=int, default=24,
                help="number of DWORDs to read from ESP for each distinct EIP/register state")
ap.add_argument("--verbose-samples", action="store_true",
                help="print one line for each sample taken in the game module or near the stall")
ap.add_argument("--main-only", action="store_true",
                help="sample only the thread owning the visible game window")
ap.add_argument("--sample-delay", type=float, default=0.05,
                help="seconds between context samples (default: 0.05)")
ap.add_argument("--click-settle", type=float, default=0.2,
                help="seconds to settle before mouse-down (0 captures the immediate post-click path)")
a = ap.parse_args()
gd = os.path.abspath(a.game_dir)
proc = subprocess.Popen([os.path.join(gd, a.exe)], cwd=gd)
pid = proc.pid
try:
    toks = a.steps.split(",")
    i = 0
    while i < len(toks):
        kind, val = toks[i].split(":", 1)
        if kind == "wait":
            time.sleep(float(val)); i += 1
        elif kind == "click":
            fx = float(val); fy = float(toks[i + 1]); i += 2
            h = main_window(pid)
            if h: click(h, fx, fy, a.click_settle)
        elif kind == "key":
            h = main_window(pid)
            if h: press_key(h, val)
            i += 1

    hproc = k32.OpenProcess(0x1F0FFF, False, pid)
    tids = threads_of(pid)
    main_tid = None
    if h:
        owner_pid = wt.DWORD()
        main_tid = u32.GetWindowThreadProcessId(h, ctypes.byref(owner_pid))
    if a.main_only and main_tid in tids:
        tids = [main_tid]
    print("pid", pid, "exe", a.exe, "threads", tids, "main_tid", main_tid)
    print("watch_globals",
          "deadline[0043B4A0]", hex_or_none(read_global(hproc, 0x0043B4A0)),
          "mouse_x[00449AB8]", hex_or_none(read_global(hproc, 0x00449AB8)),
          "mouse_y[00449ABC]", hex_or_none(read_global(hproc, 0x00449ABC)),
          "mouse_btn[00449AA4]", hex_or_none(read_global(hproc, 0x00449AA4)),
          "msg_flag[00449F30]", hex_or_none(read_global(hproc, 0x00449F30)),
          "exit_flag[00449F28]", hex_or_none(read_global(hproc, 0x00449F28)))
    counts = collections.Counter(); states = {}
    for s in range(a.samples):
        for tid in tids:
            ht = k32.OpenThread(THREAD_ALL, False, tid)
            if not ht: continue
            k32.SuspendThread(ht)
            try:
                ctx = WOW64_CONTEXT(); ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER
                ok = k32.Wow64GetThreadContext(ht, ctypes.byref(ctx))
                if ok:
                    stack_address = max(0, ctx.Esp - 0x10)
                    stack = read_stack(hproc, stack_address, a.stack_dwords + 4)
                    globals_now = {
                        "deadline": read_global(hproc, 0x0043B4A0),
                        "mouse_x": read_global(hproc, 0x00449AB8),
                        "mouse_y": read_global(hproc, 0x00449ABC),
                        "mouse_btn": read_global(hproc, 0x00449AA4),
                        "msg_flag": read_global(hproc, 0x00449F30),
                        "exit_flag": read_global(hproc, 0x00449F28),
                    }
                    locals_now = read_target_locals(hproc, ctx.Eip, ctx.Esp)
                    # Include EBP because FUN_00403260 loads DAT_0043B4A0 into
                    # EBP immediately before the 0x4033AF comparison.
                    key = (tid, ctx.Eip, ctx.Esp, ctx.Ebp,
                           tuple(globals_now.values()), tuple(locals_now.values()))
                    counts[(tid, ctx.Eip)] += 1
                    states.setdefault(key, (stack_address, stack, globals_now, locals_now))
                    if a.verbose_samples and (0x00400000 <= ctx.Eip < 0x00490000 or
                                               0x00403380 <= ctx.Eip <= 0x00403BC0 or
                                               0x00403640 <= ctx.Eip <= 0x004039B4):
                        vals = " ".join(hex_or_none(v) for v in stack[:a.stack_dwords])
                        print("sample=%d tid=%d eip=%08X esp=%08X ebp=%08X stack_base=%08X "
                              "deadline=%s mouse=(%s,%s) btn=%s flag=%s exit=%s locals=%s stack=[%s]" % (
                                  s, tid, ctx.Eip, ctx.Esp, ctx.Ebp, stack_address,
                                  hex_or_none(globals_now["deadline"]),
                                  hex_or_none(globals_now["mouse_x"]),
                                  hex_or_none(globals_now["mouse_y"]),
                                  hex_or_none(globals_now["mouse_btn"]),
                                  hex_or_none(globals_now["msg_flag"]),
                                  hex_or_none(globals_now["exit_flag"]), locals_now, vals))
            finally:
                k32.ResumeThread(ht)
                k32.CloseHandle(ht)
        time.sleep(max(0.001, a.sample_delay))

    print("eip_counts")
    for (tid, eip), c in counts.most_common(25):
        matching = [item for item in states.items() if item[0][0] == tid and item[0][1] == eip]
        if matching:
            key, (stack_address, stack, globals_now, locals_now) = matching[0]
            st = " ".join("%08X" % v for v in stack[:a.stack_dwords])
            print("tid=%d eip=%08X n=%d esp=%08X ebp=%08X stack_base=%08X "
                  "deadline=%s mouse=(%s,%s) btn=%s flag=%s exit=%s locals=%s stack=[%s]" % (
                      tid, eip, c, key[2], key[3], stack_address,
                      hex_or_none(globals_now["deadline"]),
                      hex_or_none(globals_now["mouse_x"]),
                      hex_or_none(globals_now["mouse_y"]),
                      hex_or_none(globals_now["mouse_btn"]),
                      hex_or_none(globals_now["msg_flag"]),
                      hex_or_none(globals_now["exit_flag"]), locals_now, st))
finally:
    try:
        if 'hproc' in locals() and hproc:
            k32.CloseHandle(hproc)
    finally:
        proc.kill()
