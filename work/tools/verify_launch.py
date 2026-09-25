"""Launch the game copy and collect deterministic startup/desktop evidence.

The runner is intentionally dependency-free.  It uses Win32 APIs through
``ctypes`` to find the process window, detect an ERROR message box, and save
either a window-DC BMP or a cropped full-desktop BMP.  The child is always
terminated after the observation unless ``--keep-running`` is explicitly
supplied.  Controller-driven movies should use ``--capture desktop`` and
``--capture-times``.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys
import time
import winreg
from ctypes import wintypes
from datetime import datetime, timezone
from pathlib import Path


user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.EnumWindows.restype = wintypes.BOOL
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.GetWindowTextLengthW.restype = ctypes.c_int
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.restype = ctypes.c_int
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetClassNameW.restype = ctypes.c_int
user32.EnumChildWindows.argtypes = [wintypes.HWND, WNDENUMPROC, wintypes.LPARAM]
user32.EnumChildWindows.restype = wintypes.BOOL
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(RECT)]
user32.GetWindowRect.restype = wintypes.BOOL
user32.GetSystemMetrics.argtypes = [ctypes.c_int]
user32.GetSystemMetrics.restype = ctypes.c_int
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
user32.PrintWindow.restype = wintypes.BOOL
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.ReleaseDC.restype = ctypes.c_int
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                         ctypes.c_int, wintypes.HDC, ctypes.c_int, ctypes.c_int,
                         wintypes.DWORD]
gdi32.BitBlt.restype = wintypes.BOOL
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT,
                            wintypes.UINT, ctypes.c_void_p, ctypes.POINTER(BITMAPINFO),
                            wintypes.UINT]
gdi32.GetDIBits.restype = ctypes.c_int
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteObject.restype = wintypes.BOOL
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.DeleteDC.restype = wintypes.BOOL

SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000
DIB_RGB_COLORS = 0
BI_RGB = 0


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def window_text(hwnd: int) -> str:
    length = user32.GetWindowTextLengthW(hwnd)
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buffer, len(buffer))
    return buffer.value


def window_class(hwnd: int) -> str:
    buffer = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buffer, len(buffer))
    return buffer.value


def child_text(hwnd: int) -> list[str]:
    texts: list[str] = []

    @WNDENUMPROC
    def callback(child: int, _lparam: int) -> bool:
        text = window_text(child).strip()
        if text:
            texts.append(text)
        return True

    user32.EnumChildWindows(hwnd, callback, 0)
    return texts


def windows_for_pid(pid: int) -> list[dict[str, object]]:
    windows: list[dict[str, object]] = []

    @WNDENUMPROC
    def callback(hwnd: int, _lparam: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            windows.append({
                "hwnd": int(hwnd),
                "title": window_text(hwnd),
                "class": window_class(hwnd),
                "child_text": child_text(hwnd),
            })
        return True

    user32.EnumWindows(callback, 0)
    return windows


def save_window_bmp(hwnd: int, path: Path) -> None:
    rect = RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        raise ctypes.WinError(ctypes.get_last_error())
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    if width <= 0 or height <= 0:
        raise RuntimeError(f"invalid window rectangle {width}x{height}")

    # Capturing the desktop DC is blocked in some Windows sandboxes.  A DC
    # owned by the game window works for both PrintWindow and BitBlt.
    window_dc = user32.GetDC(hwnd)
    memory = gdi32.CreateCompatibleDC(window_dc)
    bitmap = gdi32.CreateCompatibleBitmap(window_dc, width, height)
    if not window_dc or not memory or not bitmap:
        raise ctypes.WinError(ctypes.get_last_error())
    old_bitmap = gdi32.SelectObject(memory, bitmap)
    try:
        captured = user32.PrintWindow(hwnd, memory, 2)
        if not captured:
            captured = gdi32.BitBlt(memory, 0, 0, width, height, window_dc, 0, 0,
                                    SRCCOPY)
        if not captured:
            raise ctypes.WinError(ctypes.get_last_error())
        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = width
        info.bmiHeader.biHeight = height
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = BI_RGB
        size = width * height * 4
        pixels = (ctypes.c_ubyte * size)()
        # The bitmap is selected into ``memory`` for BitBlt.  GetDIBits must
        # receive a compatible DC in which that bitmap is not selected.
        rows = gdi32.GetDIBits(window_dc, bitmap, 0, height, pixels,
                               ctypes.byref(info), DIB_RGB_COLORS)
        if rows != height:
            raise ctypes.WinError(ctypes.get_last_error())
        file_size = 14 + ctypes.sizeof(BITMAPINFOHEADER) + size
        file_header = b"BM" + file_size.to_bytes(4, "little") + b"\0\0\0\0" + (
            14 + ctypes.sizeof(BITMAPINFOHEADER)
        ).to_bytes(4, "little")
        path.write_bytes(file_header + bytes(info.bmiHeader) + bytes(pixels))
    finally:
        gdi32.SelectObject(memory, old_bitmap)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(memory)
        user32.ReleaseDC(hwnd, window_dc)


def _powershell_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _save_desktop_crop_gdi(hwnd: int, path: Path) -> None:
    """Fallback: capture the full virtual desktop DC with BitBlt, then crop."""
    rect = RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        raise ctypes.WinError(ctypes.get_last_error())
    desktop_left = user32.GetSystemMetrics(-76)   # SM_XVIRTUALSCREEN
    desktop_top = user32.GetSystemMetrics(-77)    # SM_YVIRTUALSCREEN
    desktop_width = user32.GetSystemMetrics(78)   # SM_CXVIRTUALSCREEN
    desktop_height = user32.GetSystemMetrics(79)  # SM_CYVIRTUALSCREEN
    crop_width = rect.right - rect.left
    crop_height = rect.bottom - rect.top
    crop_x = rect.left - desktop_left
    crop_y = rect.top - desktop_top
    if (desktop_width <= 0 or desktop_height <= 0 or crop_width <= 0 or
            crop_height <= 0 or crop_x < 0 or crop_y < 0 or
            crop_x + crop_width > desktop_width or
            crop_y + crop_height > desktop_height):
        raise RuntimeError("invalid virtual desktop or window rectangle")

    desktop_dc = user32.GetDC(0)
    memory = gdi32.CreateCompatibleDC(desktop_dc)
    bitmap = gdi32.CreateCompatibleBitmap(desktop_dc, desktop_width, desktop_height)
    if not desktop_dc or not memory or not bitmap:
        if memory:
            gdi32.DeleteDC(memory)
        if bitmap:
            gdi32.DeleteObject(bitmap)
        if desktop_dc:
            user32.ReleaseDC(0, desktop_dc)
        raise ctypes.WinError(ctypes.get_last_error())
    old_bitmap = gdi32.SelectObject(memory, bitmap)
    try:
        if not gdi32.BitBlt(memory, 0, 0, desktop_width, desktop_height,
                            desktop_dc, desktop_left, desktop_top,
                            SRCCOPY | CAPTUREBLT):
            raise ctypes.WinError(ctypes.get_last_error())
        info = BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        info.bmiHeader.biWidth = desktop_width
        info.bmiHeader.biHeight = desktop_height
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = BI_RGB
        full_size = desktop_width * desktop_height * 4
        full_pixels = (ctypes.c_ubyte * full_size)()
        rows = gdi32.GetDIBits(desktop_dc, bitmap, 0, desktop_height,
                               full_pixels, ctypes.byref(info), DIB_RGB_COLORS)
        if rows != desktop_height:
            raise ctypes.WinError(ctypes.get_last_error())
        row_size = crop_width * 4
        crop_pixels = bytearray(crop_height * row_size)
        full_row_size = desktop_width * 4
        full_bytes = bytes(full_pixels)
        for top_row in range(crop_height):
            source_bottom_row = desktop_height - 1 - (crop_y + top_row)
            target_bottom_row = crop_height - 1 - top_row
            source_start = source_bottom_row * full_row_size + crop_x * 4
            target_start = target_bottom_row * row_size
            crop_pixels[target_start:target_start + row_size] = (
                full_bytes[source_start:source_start + row_size]
            )
        info.bmiHeader.biWidth = crop_width
        info.bmiHeader.biHeight = crop_height
        file_size = 14 + ctypes.sizeof(BITMAPINFOHEADER) + len(crop_pixels)
        file_header = b"BM" + file_size.to_bytes(4, "little") + b"\0\0\0\0" + (
            14 + ctypes.sizeof(BITMAPINFOHEADER)
        ).to_bytes(4, "little")
        path.write_bytes(file_header + bytes(info.bmiHeader) + bytes(crop_pixels))
    finally:
        gdi32.SelectObject(memory, old_bitmap)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(memory)
        user32.ReleaseDC(0, desktop_dc)


def save_desktop_crop_bmp(hwnd: int, path: Path) -> str:
    """Capture the full virtual desktop with CopyFromScreen, then crop hwnd.

    Some non-interactive Windows sessions reject GDI+ CopyFromScreen even
    though the desktop DC is available.  Keep CopyFromScreen as the primary
    evidence path and use a direct desktop-DC BitBlt fallback only then.
    """
    rect = RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        raise ctypes.WinError(ctypes.get_last_error())
    script = f"""
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
$full = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$graphics = [System.Drawing.Graphics]::FromImage($full)
try {{
    $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $full.Size)
    $cropRect = New-Object System.Drawing.Rectangle({rect.left} - $bounds.Left, {rect.top} - $bounds.Top, {rect.right - rect.left}, {rect.bottom - rect.top})
    if ($cropRect.X -lt 0 -or $cropRect.Y -lt 0 -or
        $cropRect.Right -gt $full.Width -or $cropRect.Bottom -gt $full.Height) {{
        throw 'window rectangle is outside the virtual desktop'
    }}
    $crop = $full.Clone($cropRect, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {{ $crop.Save({_powershell_literal(str(path))}, [System.Drawing.Imaging.ImageFormat]::Bmp) }}
    finally {{ $crop.Dispose() }}
}}
finally {{ $graphics.Dispose(); $full.Dispose() }}
"""
    completed = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
         "-Command", script],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )
    if completed.returncode == 0 and path.is_file():
        return "CopyFromScreen"
    primary_detail = completed.stderr.decode(errors="replace").strip()
    try:
        _save_desktop_crop_gdi(hwnd, path)
        return "desktop_dc_BitBlt_fallback"
    except Exception as fallback_exc:
        detail = primary_detail or f"CopyFromScreen failed with exit {completed.returncode}"
        raise RuntimeError(f"{detail}; desktop DC fallback failed: {fallback_exc}") from fallback_exc


def appcompat_layer(exe: Path) -> str | None:
    key_path = r"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as key:
            value, _kind = winreg.QueryValueEx(key, str(exe))
            return str(value)
    except FileNotFoundError:
        return None


def appcompat_layers(exe: Path) -> dict[str, str | None]:
    key_path = r"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
    values: dict[str, str | None] = {}
    for name, root in (("HKCU", winreg.HKEY_CURRENT_USER),
                       ("HKLM", winreg.HKEY_LOCAL_MACHINE)):
        try:
            with winreg.OpenKey(root, key_path) as key:
                value, _kind = winreg.QueryValueEx(key, str(exe))
                values[name] = str(value)
        except (FileNotFoundError, PermissionError):
            values[name] = None
    return values


def verify(args: argparse.Namespace) -> int:
    game_dir = args.game_dir.resolve()
    exe = (game_dir / args.exe).resolve()
    if not exe.is_file():
        print(f"executable not found: {exe}", file=sys.stderr)
        return 2
    if exe.parent != game_dir:
        print("--exe must name a file directly under --game-dir", file=sys.stderr)
        return 2

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    screenshot_dir = args.screenshot_dir.resolve()
    screenshot_dir.mkdir(parents=True, exist_ok=True)
    capture_times: list[float] = []
    for item in args.capture_times or []:
        for value in str(item).split(","):
            if value.strip():
                capture_times.append(float(value.strip()))
    capture_times = sorted(set(capture_times))
    if any(value < 0 for value in capture_times):
        print("--capture-times values must be non-negative", file=sys.stderr)
        return 2
    screenshot = screenshot_dir / f"launch_{exe.stem}_{timestamp}.bmp"
    result_path = args.result.resolve() if args.result else screenshot.with_suffix(".json")
    started = datetime.now(timezone.utc).isoformat()
    process: subprocess.Popen[bytes] | None = None
    running_after_wait = False
    windows: list[dict[str, object]] = []
    observed_windows: dict[int, dict[str, object]] = {}
    observed_dialogs: dict[int, dict[str, object]] = {}
    sample_count = 0
    captures: list[dict[str, object]] = []

    def record_windows(items: list[dict[str, object]]) -> None:
        for item in items:
            hwnd = int(item["hwnd"])
            observed_windows[hwnd] = item
            title = str(item["title"])
            klass = str(item["class"])
            child = " ".join(str(value) for value in item["child_text"])
            if (klass == "#32770" or "ERROR" in title.upper() or
                    "ERROR" in child.upper()):
                observed_dialogs[hwnd] = item

    def capture_one(hwnd: int | None, requested: float | None) -> dict[str, object]:
        if requested is None:
            path = screenshot
        else:
            label = f"t{requested:07.3f}s".replace(".", "p")
            path = screenshot_dir / f"launch_{exe.stem}_{timestamp}_{label}.bmp"
        item: dict[str, object] = {
            "requested_seconds": requested,
            "path": str(path),
            "saved": False,
            "error": None,
            "capture": args.capture,
            "method": None,
        }
        if hwnd is None:
            item["error"] = "Gundam Tactics window was not present at capture time"
            captures.append(item)
            return item
        try:
            if args.capture == "desktop":
                item["method"] = save_desktop_crop_bmp(hwnd, path)
            else:
                save_window_bmp(hwnd, path)
                item["method"] = "window_dc"
            item["saved"] = True
        except Exception as exc:
            item["error"] = str(exc)
        captures.append(item)
        return item

    try:
        process = subprocess.Popen([str(exe)], cwd=str(game_dir),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        started_monotonic = time.monotonic()
        observation_seconds = max([args.wait_seconds, *capture_times])
        deadline = started_monotonic + observation_seconds
        next_capture = 0
        # Continue sampling until the requested deadline.  A window appearing
        # early is not evidence that the game survives the full observation.
        while True:
            windows = windows_for_pid(process.pid)
            sample_count += 1
            record_windows(windows)
            elapsed = time.monotonic() - started_monotonic
            while next_capture < len(capture_times) and elapsed >= capture_times[next_capture]:
                main_window = next((int(item["hwnd"]) for item in windows
                                    if str(item["title"]).strip() == "Gundam Tactics"), None)
                capture_one(main_window, capture_times[next_capture])
                next_capture += 1
            now = time.monotonic()
            remaining = deadline - now
            if remaining <= 0:
                break
            until_capture = (capture_times[next_capture] - (now - started_monotonic)
                             if next_capture < len(capture_times) else remaining)
            time.sleep(min(0.1, remaining, max(0.01, until_capture)))
        running_after_wait = process.poll() is None
        windows = windows_for_pid(process.pid)
        record_windows(windows)
        main_window = next((int(item["hwnd"]) for item in windows
                            if str(item["title"]).strip() == "Gundam Tactics"), None)
        error_windows = list(observed_dialogs.values())
        while next_capture < len(capture_times):
            capture_one(main_window, capture_times[next_capture])
            next_capture += 1
        if not capture_times:
            capture_one(main_window, None)
        screenshot_saved = bool(captures) and bool(captures[-1]["saved"])
        screenshot_error = captures[-1]["error"] if captures else None
        requested_captures_ok = all(bool(item["saved"]) for item in captures)
        result = {
            "started_utc": started,
            "finished_utc": datetime.now(timezone.utc).isoformat(),
            "pid": process.pid,
            "exe": str(exe),
            "exe_sha256": sha256(exe),
            "game_dir": str(game_dir),
            "wait_seconds": args.wait_seconds,
            "capture": args.capture,
            "capture_times": capture_times,
            "process_alive_after_wait": running_after_wait,
            "exit_code_before_cleanup": process.poll(),
            "windows": windows,
            "observed_windows": list(observed_windows.values()),
            "observation_samples": sample_count,
            "window_present": bool(windows),
            "main_window_present": main_window is not None,
            "error_dialog_present": bool(error_windows),
            "error_windows": error_windows,
            "screenshot": str(screenshot) if screenshot_saved else None,
            "screenshot_saved": screenshot_saved,
            "screenshot_error": screenshot_error,
            "captures": captures,
            "elevated_process": bool(shell32.IsUserAnAdmin()),
            "appcompat_layer": appcompat_layer(exe),
            "appcompat_layers": appcompat_layers(exe),
            "compat_layer_environment": os.environ.get("__COMPAT_LAYER"),
        }
        result["passed"] = all((
            running_after_wait,
            main_window is not None,
            not error_windows,
            requested_captures_ok,
        ))
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                               encoding="utf-8")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["passed"] else 1
    finally:
        if process is not None and not args.keep_running and process.poll() is None:
            process.kill()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path,
                        default=Path(__file__).resolve().parents[2] / "run" / "GT01")
    parser.add_argument("--exe", default="gundam.exe",
                        help="executable filename directly under --game-dir")
    parser.add_argument("--wait-seconds", type=float, default=8.0)
    parser.add_argument("--capture", choices=("window", "desktop"), default="window",
                        help="window DC capture or full desktop CopyFromScreen crop")
    parser.add_argument("--capture-times", nargs="+", metavar="SECONDS",
                        help="capture one or more times; comma-separated values are accepted")
    parser.add_argument("--screenshot-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "analysis" / "screenshots")
    parser.add_argument("--result", type=Path,
                        help="JSON result path (default: next to the screenshot)")
    parser.add_argument("--keep-running", action="store_true",
                        help="leave the child running for manual inspection")
    return verify(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
