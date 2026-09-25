r"""Deploy or restore the QTIM32 compatibility proxy.

Usage:
    python deploy.py
    python deploy.py --game-dir C:\path\to\GundamTactics
    python deploy.py --undeploy

Deployment layout:
    <game>/QTIM32.DLL   compatibility proxy
    <game>/QTIM32R.DLL  original QuickTime runtime
    <game>/CMGR32.DLL   compatibility proxy for the second dispatcher
    <game>/CMGR32R.DLL  original component manager runtime
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
DEFAULT_GAME_DIR = REPO_ROOT / "run" / "GT"
LEGACY_GAME_DIR = SCRIPT_DIR.parent.parent
PROXY_SRC = SCRIPT_DIR / "QTIM32.dll"
CMGR_PROXY_SRC = SCRIPT_DIR / "CMGR32.dll"


def md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


def is_compat_proxy(path: Path) -> bool:
    if not path.exists():
        return False
    data = path.read_bytes()
    return b"qtim_compat_proxy" in data or b"fake_GetMoviePict" in data


def is_observation_proxy(path: Path) -> bool:
    if not path.exists():
        return False
    if is_compat_proxy(path):
        return False
    data = path.read_bytes()
    return b"qtim_proxy v7" in data or b"proxy_ord_505" in data


def is_cmgr_compat_proxy(path: Path) -> bool:
    if not path.exists():
        return False
    return b"cmgr_compat_proxy" in path.read_bytes()


def default_game_dir() -> Path:
    return DEFAULT_GAME_DIR if DEFAULT_GAME_DIR.exists() else LEGACY_GAME_DIR


def _temp_path(directory: Path, label: str) -> Path:
    fd, name = tempfile.mkstemp(prefix=f".{label}.", suffix=".tmp", dir=directory)
    os.close(fd)
    path = Path(name)
    path.unlink()
    return path


def _stage_copy(source: Path, directory: Path, label: str) -> Path:
    target = _temp_path(directory, label)
    shutil.copy2(source, target)
    return target


def _remove(path: Path | None) -> None:
    if path is not None and path.exists():
        path.unlink()


def _preflight_deploy(game_dir: Path, replace_observation: bool) -> dict[str, object] | None:
    qtim_orig = game_dir / "QTIM32.DLL"
    qtim_real = game_dir / "QTIM32R.DLL"
    cmgr_orig = game_dir / "CMGR32.DLL"
    cmgr_real = game_dir / "CMGR32R.DLL"

    if not PROXY_SRC.is_file() or not CMGR_PROXY_SRC.is_file():
        print(f"proxy not built: {PROXY_SRC} / {CMGR_PROXY_SRC}")
        print("run: python build.py")
        return None
    if not is_compat_proxy(PROXY_SRC) or not is_cmgr_compat_proxy(CMGR_PROXY_SRC):
        print("proxy preflight failed: generated DLLs are not recognized compatibility proxies")
        return None

    qtim_observation = qtim_orig.exists() and is_observation_proxy(qtim_orig)
    if qtim_observation and not replace_observation:
        print("QTIM32.DLL is the observation proxy; restore it before compat deploy.")
        print("run: python deploy.py --undeploy")
        print("or:  python deploy.py --replace-observation")
        return None
    if qtim_observation and not qtim_real.is_file():
        print(f"cannot replace observation proxy without real backup: {qtim_real}")
        return None

    move_qtim = qtim_orig.exists() and not qtim_observation and not is_compat_proxy(qtim_orig)
    if move_qtim and qtim_real.exists():
        print(f"both {qtim_orig.name} and {qtim_real.name} exist; refusing to guess.")
        return None
    if not qtim_real.is_file() and not move_qtim:
        print(f"missing real runtime: {qtim_real}")
        return None

    move_cmgr = cmgr_orig.exists() and not is_cmgr_compat_proxy(cmgr_orig)
    if move_cmgr and cmgr_real.exists():
        print(f"both {cmgr_orig.name} and {cmgr_real.name} exist; refusing to guess.")
        return None
    if not cmgr_real.is_file() and not move_cmgr:
        print(f"missing real runtime: {cmgr_real}")
        return None

    return {
        "qtim_orig": qtim_orig,
        "qtim_real": qtim_real,
        "cmgr_orig": cmgr_orig,
        "cmgr_real": cmgr_real,
        "move_qtim": move_qtim,
        "move_cmgr": move_cmgr,
    }


def deploy(game_dir: Path, replace_observation: bool) -> int:
    plan = _preflight_deploy(game_dir, replace_observation)
    if plan is None:
        return 1

    qtim_orig = plan["qtim_orig"]
    qtim_real = plan["qtim_real"]
    cmgr_orig = plan["cmgr_orig"]
    cmgr_real = plan["cmgr_real"]
    move_qtim = bool(plan["move_qtim"])
    move_cmgr = bool(plan["move_cmgr"])
    qtim_old = _stage_copy(qtim_orig, game_dir, "old_qtim") if qtim_orig.exists() else None
    cmgr_old = _stage_copy(cmgr_orig, game_dir, "old_cmgr") if cmgr_orig.exists() else None
    qtim_stage = _stage_copy(PROXY_SRC, game_dir, "proxy_qtim")
    cmgr_stage = _stage_copy(CMGR_PROXY_SRC, game_dir, "proxy_cmgr")
    qtim_installed = False
    cmgr_installed = False
    qtim_moved = False
    cmgr_moved = False
    try:
        if move_qtim:
            os.replace(qtim_orig, qtim_real)
            qtim_moved = True
            print(f"renamed stock {qtim_orig.name} -> {qtim_real.name}")
        if move_cmgr:
            os.replace(cmgr_orig, cmgr_real)
            cmgr_moved = True
            print(f"renamed stock {cmgr_orig.name} -> {cmgr_real.name}")
        os.replace(qtim_stage, qtim_orig)
        qtim_installed = True
        qtim_stage = None
        os.replace(cmgr_stage, cmgr_orig)
        cmgr_installed = True
        cmgr_stage = None
        for trace_name in ("qtim_compat_trace.log", "cmgr_compat_trace.log"):
            trace_path = game_dir / trace_name
            if trace_path.exists():
                trace_path.unlink()
                print(f"removed stale trace: {trace_path}")
    except Exception:
        _remove(qtim_stage)
        _remove(cmgr_stage)
        if move_qtim:
            if qtim_orig.exists():
                qtim_orig.unlink()
            if qtim_real.exists():
                os.replace(qtim_real, qtim_orig)
        elif qtim_old is not None and qtim_old.exists():
            os.replace(qtim_old, qtim_orig)
        elif qtim_installed:
            _remove(qtim_orig)
        if move_cmgr:
            if cmgr_orig.exists():
                cmgr_orig.unlink()
            if cmgr_real.exists():
                os.replace(cmgr_real, cmgr_orig)
        elif cmgr_old is not None and cmgr_old.exists():
            os.replace(cmgr_old, cmgr_orig)
        elif cmgr_installed:
            _remove(cmgr_orig)
        _remove(qtim_old)
        _remove(cmgr_old)
        raise
    _remove(qtim_old)
    _remove(cmgr_old)
    print(f"deployed {qtim_orig}")
    print(f"  proxy md5: {md5(qtim_orig)}")
    print(f"  real  md5: {md5(qtim_real)}")
    print(f"deployed {cmgr_orig}")
    print(f"  proxy md5: {md5(cmgr_orig)}")
    print(f"  real  md5: {md5(cmgr_real)}")
    return 0


def _preflight_undeploy(game_dir: Path) -> dict[str, Path] | None:
    qtim_orig = game_dir / "QTIM32.DLL"
    qtim_real = game_dir / "QTIM32R.DLL"
    cmgr_orig = game_dir / "CMGR32.DLL"
    cmgr_real = game_dir / "CMGR32R.DLL"
    if not qtim_real.is_file():
        print(f"missing backup: {qtim_real}")
        return None
    if qtim_orig.exists() and not is_compat_proxy(qtim_orig):
        print(f"{qtim_orig} is not the compatibility proxy; refusing to overwrite.")
        return None
    if not cmgr_real.is_file():
        print(f"missing backup: {cmgr_real}")
        return None
    if cmgr_orig.exists() and not is_cmgr_compat_proxy(cmgr_orig):
        print(f"{cmgr_orig} is not the compatibility proxy; refusing to overwrite.")
        return None
    return {
        "qtim_orig": qtim_orig,
        "qtim_real": qtim_real,
        "cmgr_orig": cmgr_orig,
        "cmgr_real": cmgr_real,
    }


def undeploy(game_dir: Path) -> int:
    plan = _preflight_undeploy(game_dir)
    if plan is None:
        return 1
    qtim_orig = plan["qtim_orig"]
    qtim_real = plan["qtim_real"]
    cmgr_orig = plan["cmgr_orig"]
    cmgr_real = plan["cmgr_real"]
    qtim_old = _temp_path(game_dir, "undeploy_qtim") if qtim_orig.exists() else None
    cmgr_old = _temp_path(game_dir, "undeploy_cmgr") if cmgr_orig.exists() else None
    qtim_saved = False
    cmgr_saved = False
    qtim_restored = False
    cmgr_restored = False
    try:
        if qtim_orig.exists():
            os.replace(qtim_orig, qtim_old)
            qtim_saved = True
        os.replace(qtim_real, qtim_orig)
        qtim_restored = True
        if cmgr_orig.exists():
            os.replace(cmgr_orig, cmgr_old)
            cmgr_saved = True
        os.replace(cmgr_real, cmgr_orig)
        cmgr_restored = True
    except Exception:
        if cmgr_restored:
            os.replace(cmgr_orig, cmgr_real)
        if cmgr_saved and cmgr_old.exists():
            os.replace(cmgr_old, cmgr_orig)
        if qtim_restored:
            os.replace(qtim_orig, qtim_real)
        if qtim_saved and qtim_old.exists():
            os.replace(qtim_old, qtim_orig)
        raise
    _remove(qtim_old)
    _remove(cmgr_old)
    print(f"restored {qtim_orig}")
    print(f"  md5: {md5(qtim_orig)}")
    print(f"restored {cmgr_orig}")
    print(f"  md5: {md5(cmgr_orig)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--undeploy", action="store_true")
    parser.add_argument(
        "--game-dir",
        type=Path,
        default=default_game_dir(),
        help="Game directory containing QTIM32.DLL and QTIM32R.DLL.",
    )
    parser.add_argument(
        "--replace-observation",
        action="store_true",
        help="Replace an active observation proxy when QTIM32R.DLL is present.",
    )
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.exists():
        print(f"game directory not found: {game_dir}")
        return 1
    return undeploy(game_dir) if args.undeploy else deploy(game_dir, args.replace_observation)


if __name__ == "__main__":
    sys.exit(main())
