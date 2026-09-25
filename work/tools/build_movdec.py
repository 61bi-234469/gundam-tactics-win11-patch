"""Build the standalone SMC MOV decoder CLI with 32-bit MinGW."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GCC = Path(r"C:\msys64\mingw32\bin\gcc.exe")


def main() -> int:
    if not GCC.exists():
        print(f"missing compiler: {GCC}")
        return 1
    out = ROOT / "work" / "tools" / "movdec.exe"
    cmd = [str(GCC), "-m32", "-O2", "-Wall", "-Wextra", "-std=c99",
           "-o", str(out), str(ROOT / "work" / "tools" / "movdec_cli.c"),
           str(ROOT / "work" / "qtim_proxy" / "movdec.c")]
    print(" ".join(cmd))
    env = os.environ.copy()
    env["PATH"] = str(GCC.parent) + os.pathsep + env.get("PATH", "")
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    raise SystemExit(main())
