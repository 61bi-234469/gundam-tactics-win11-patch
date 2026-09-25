from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path
import time
import wave


winmm = ctypes.WinDLL("winmm")


UINT = wintypes.UINT
DWORD = wintypes.DWORD
WORD = wintypes.WORD
HANDLE = wintypes.HANDLE
HWAVEOUT = HANDLE
DWORD_PTR = wintypes.WPARAM
MMRESULT = UINT
MCIERROR = DWORD
MCIDEVICEID = UINT

WAVE_MAPPER = 0xFFFFFFFF
CALLBACK_NULL = 0x00000000

class WAVEFORMATEX(ctypes.Structure):
    _fields_ = [
        ("wFormatTag", WORD),
        ("nChannels", WORD),
        ("nSamplesPerSec", DWORD),
        ("nAvgBytesPerSec", DWORD),
        ("nBlockAlign", WORD),
        ("wBitsPerSample", WORD),
        ("cbSize", WORD),
    ]


winmm.waveOutGetNumDevs.restype = UINT

winmm.waveOutOpen.argtypes = [
    ctypes.POINTER(HWAVEOUT),
    UINT,
    ctypes.POINTER(WAVEFORMATEX),
    DWORD_PTR,
    DWORD_PTR,
    DWORD,
]
winmm.waveOutOpen.restype = MMRESULT

winmm.waveOutClose.argtypes = [HWAVEOUT]
winmm.waveOutClose.restype = MMRESULT

winmm.waveOutGetErrorTextA.argtypes = [MMRESULT, wintypes.LPSTR, UINT]
winmm.waveOutGetErrorTextA.restype = MMRESULT

winmm.mciGetErrorStringA.argtypes = [MCIERROR, wintypes.LPSTR, UINT]
winmm.mciGetErrorStringA.restype = wintypes.BOOL

winmm.mciSendStringA.argtypes = [wintypes.LPCSTR, wintypes.LPSTR, UINT, HANDLE]
winmm.mciSendStringA.restype = MCIERROR


def mmresult_to_text(result: int) -> str:
    if result == 0:
        return "MMSYSERR_NOERROR"
    buffer = ctypes.create_string_buffer(256)
    text_result = winmm.waveOutGetErrorTextA(result, buffer, len(buffer))
    if text_result == 0:
        return buffer.value.decode("ascii", "replace")
    return f"unknown MMRESULT 0x{result:08x}"


def mcierror_to_text(result: int) -> str:
    if result == 0:
        return "MCIERR_NO_ERROR"
    buffer = ctypes.create_string_buffer(256)
    ok = winmm.mciGetErrorStringA(result, buffer, len(buffer))
    if ok:
        return buffer.value.decode("ascii", "replace")
    return f"unknown MCIERROR 0x{result:08x}"


@dataclass
class WaveSummary:
    path: Path
    channels: int
    sample_width: int
    rate: int
    frames: int


def read_wave_summary(path: Path) -> WaveSummary:
    with wave.open(str(path), "rb") as wav:
        return WaveSummary(
            path=path,
            channels=wav.getnchannels(),
            sample_width=wav.getsampwidth(),
            rate=wav.getframerate(),
            frames=wav.getnframes(),
        )


def test_wave_out(path: Path) -> None:
    summary = read_wave_summary(path)
    fmt = WAVEFORMATEX()
    fmt.wFormatTag = 1
    fmt.nChannels = summary.channels
    fmt.nSamplesPerSec = summary.rate
    fmt.wBitsPerSample = summary.sample_width * 8
    fmt.nBlockAlign = summary.channels * summary.sample_width
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign
    fmt.cbSize = 0

    handle = HWAVEOUT()
    result = winmm.waveOutOpen(
        ctypes.byref(handle),
        WAVE_MAPPER,
        ctypes.byref(fmt),
        0,
        0,
        CALLBACK_NULL,
    )

    print(f"[waveOut] file={path}")
    print(
        "  format="
        f"{fmt.nSamplesPerSec}Hz {fmt.nChannels}ch {fmt.wBitsPerSample}bit "
        f"block={fmt.nBlockAlign} avg={fmt.nAvgBytesPerSec}"
    )
    print(f"  waveOutOpen=0x{result:08x} {mmresult_to_text(result)}")
    if result == 0:
        close_result = winmm.waveOutClose(handle)
        print(f"  waveOutClose=0x{close_result:08x} {mmresult_to_text(close_result)}")


def mci_send(command: str, capture: bool = False) -> tuple[int, str]:
    buffer = ctypes.create_string_buffer(256) if capture else None
    result = winmm.mciSendStringA(
        command.encode("mbcs"),
        buffer,
        len(buffer) if buffer is not None else 0,
        None,
    )
    text = ""
    if buffer is not None:
        text = buffer.value.decode("ascii", "replace")
    return int(result), text


def test_midi(path: Path, close_immediately: bool) -> None:
    print(f"[mci] file={path}")
    alias = "phase1mid"

    open_command = f'open "{path}" type sequencer alias {alias}'
    open_result, _ = mci_send(open_command)
    print(f"  open=0x{open_result:08x} {mcierror_to_text(open_result)}")
    if open_result != 0:
        return

    play_result, _ = mci_send(f"play {alias}")
    print(f"  play=0x{play_result:08x} {mcierror_to_text(play_result)}")

    if close_immediately:
        close_result, _ = mci_send(f"close {alias}")
        print(f"  close(immediate)=0x{close_result:08x} {mcierror_to_text(close_result)}")
        return

    time.sleep(0.2)
    status_result, mode = mci_send(f"status {alias} mode", capture=True)
    print(f"  status(mode)=0x{status_result:08x} {mcierror_to_text(status_result)} value={mode!r}")
    close_result, _ = mci_send(f"close {alias}")
    print(f"  close=0x{close_result:08x} {mcierror_to_text(close_result)}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Observe phase-1 audio APIs directly through winmm.")
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("."),
        help="game root directory (default: current directory)",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    print(f"root={root}")
    print(f"waveOutGetNumDevs={winmm.waveOutGetNumDevs()}")

    test_wave_out(root / "Sound" / "Cannon.wav")
    test_wave_out(root / "Sound" / "END_0.WAV")

    test_midi(root / "Sound" / "GMSYS_ON.MID", close_immediately=True)
    test_midi(root / "Sound" / "M04GM.MID", close_immediately=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
