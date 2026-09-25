"""Build the QTIM32 compatibility proxy with MinGW gcc."""

from __future__ import annotations

import shutil
import subprocess
import sys
import os
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_GCC = Path(r"C:\msys64\mingw32\bin\gcc.exe")


def find_gcc() -> Path | None:
    if DEFAULT_GCC.exists():
        return DEFAULT_GCC
    found = shutil.which("i686-w64-mingw32-gcc") or shutil.which("gcc")
    return Path(found) if found else None


def main() -> int:
    gcc = find_gcc()
    if gcc is None:
        print("no 32-bit MinGW gcc found")
        return 1

    common = [
        str(gcc),
        "-m32",
        "-O2",
        "-Wall",
        "-Wextra",
        "-fno-asynchronous-unwind-tables",
        "-fno-exceptions",
        "-shared",
        "-static-libgcc",
        "-Wl,--enable-stdcall-fixup",
        "-Wl,--no-insert-timestamp",
        "-Wl,--subsystem,windows",
        "-lkernel32",
    ]
    env = os.environ.copy()
    mingw_bin = str(gcc.parent)
    env["PATH"] = mingw_bin + os.pathsep + env.get("PATH", "")
    qtim_cmd = common + [
        "-o", "QTIM32.dll",
        "qtim_compat_proxy.c", "movdec.c", "display_scale.c",
        "qtim_compat_proxy.def",
        "-luser32", "-lgdi32", "-lwinmm",
    ]
    cmgr_cmd = common + [
        "-o", "CMGR32.dll",
        "cmgr_compat_proxy.c", "cmgr_compat_proxy.def",
    ]
    for cmd in (qtim_cmd, cmgr_cmd):
        print(" ".join(cmd))
        result = subprocess.call(cmd, cwd=SCRIPT_DIR, env=env)
        if result != 0:
            return result
    return 0


if __name__ == "__main__":
    sys.exit(main())
