"""Build the v1.1.0 executable, proxies, release package, ZIP, and release tests.

The pipeline is intentionally linear and reproducible:

    clean source executable -> audio/movie/display-b/path/stretch patches -> proxy build
    -> release staging/allowlist/checksums -> deterministic ZIP -> atomic publish
    -> ZIP-extraction release-cycle test -> runtime executable update

``source_exe`` and ``source_iso`` are never written.  The runtime executable is
updated only after the proxy has built successfully and the generated release
has passed its ZIP-extraction test (unless ``--skip-release-test`` is supplied).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
import zipfile
from pathlib import Path


EXPECTED_SOURCE_SHA256 = "38bde2e4513c665d1425fd00203d0000001c5b81bc37899507b6ef7129f238d3"
EXPECTED_PHASE2_SHA256 = "9145d0a5fbc0070e52ab3fe873c18a849b11ba5cae8c5a9fa06c9bba706acb52"
EXPECTED_PHASE3_B_SHA256 = "82c92402ac9c9282992d3c343dbb7c62463a3a5e58569ff845670bf166b867d4"
EXPECTED_PHASE4_SHA256 = "c693d60b73cbba731dbacbea255e845e97a0dae95cc80812316a5795e4f15942"
EXPECTED_PHASE5_SHA256 = "607299a2cd7d5aeb1375cd838cd343cd81f9289311cea659d100c700d4035ec4"

REPO_ROOT = Path(__file__).resolve().parents[1]
RUN_ROOT = REPO_ROOT / "run"
PYTHON = Path(sys.executable)
PATCH_AUDIO = REPO_ROOT / "work" / "exe_patches" / "patch_audio_phase1.py"
PATCH_MOVIE = REPO_ROOT / "work" / "exe_patches" / "patch_movie_phase2.py"
PATCH_DISPLAY = REPO_ROOT / "work" / "exe_patches" / "patch_display_phase3.py"
PATCH_PATH = REPO_ROOT / "work" / "exe_patches" / "patch_path_phase4.py"
PATCH_STRETCH = REPO_ROOT / "work" / "exe_patches" / "patch_stretch_phase5.py"
PROXY_BUILD = REPO_ROOT / "work" / "qtim_proxy" / "build.py"
PROXY_DLL = REPO_ROOT / "work" / "qtim_proxy" / "QTIM32.dll"
CMGR_PROXY_DLL = REPO_ROOT / "work" / "qtim_proxy" / "CMGR32.dll"
RELEASE_PARENT = REPO_ROOT / "release"
RELEASE_DIR = RELEASE_PARENT / "GundamTactics_Win11Patch"
RELEASE_ZIP = RELEASE_PARENT / "GundamTactics_Win11Patch_v1.1.0.zip"
RELEASE_ZIP_CHECKSUMS = RELEASE_PARENT / "checksums_zip.txt"
RELEASE_TEST = REPO_ROOT / "work" / "tools" / "test_release_cycle.ps1"
PACKAGE_ENTRIES = (
    "install.bat",
    "uninstall.bat",
    "README.txt",
    "CHANGELOG.txt",
    "patch_files/install.ps1",
    "patch_files/apply.ps1",
    "patch_files/revert.ps1",
    "patch_files/QTIM32.dll",
    "patch_files/CMGR32.dll",
    "checksums.txt",
)
CHECKSUM_ENTRIES = PACKAGE_ENTRIES[:-1]
# Rewritten with the current proxy hashes (CRLF, UTF-8 without BOM).
TEMPLATE_TEXT_ENTRIES = (
    "README.txt",
    "CHANGELOG.txt",
    "patch_files/apply.ps1",
    "patch_files/revert.ps1",
)
# Copied byte for byte: install.ps1 needs its UTF-8 BOM for PowerShell 5.1.
TEMPLATE_BINARY_ENTRIES = (
    "install.bat",
    "uninstall.bat",
    "patch_files/install.ps1",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_patch(script: Path, *args: Path | str) -> None:
    command = [str(PYTHON), str(script), *map(str, args)]
    print("[build]", " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=REPO_ROOT)
    if completed.returncode != 0:
        raise RuntimeError(f"patch failed ({completed.returncode}): {script.name}")


def build_executable_artifact(source: Path, expected_source: str) -> tuple[list[Path], Path, str]:
    source = source.resolve()
    if not source.is_file():
        raise FileNotFoundError(source)
    actual_source = sha256(source)
    if actual_source.lower() != expected_source.lower():
        raise RuntimeError(
            f"source hash mismatch for {source}: {actual_source}; expected {expected_source}"
        )

    staging_root = REPO_ROOT / "work" / "build_staging"
    staging_root.mkdir(parents=True, exist_ok=True)
    prefix = f"build_all_{os.getpid()}_{uuid.uuid4().hex}"
    paths = [
        staging_root / f"{prefix}_original.exe",
        staging_root / f"{prefix}_audio.exe",
        staging_root / f"{prefix}_movie.exe",
        staging_root / f"{prefix}_display_b.exe",
        staging_root / f"{prefix}_path.exe",
        staging_root / f"{prefix}_stretch.exe",
    ]
    original, audio, movie, display, path_fixed, stretch_fixed = paths
    try:
        shutil.copy2(source, original)
        run_patch(PATCH_AUDIO, "--input", original, "--output", audio)
        run_patch(PATCH_MOVIE, "--source", audio, "--dest", movie)
        phase2_hash = sha256(movie)
        print(f"[build] Phase 2 sha256={phase2_hash}")
        if phase2_hash.lower() != EXPECTED_PHASE2_SHA256:
            raise RuntimeError(f"Phase 2 output hash mismatch: {phase2_hash}")
        run_patch(PATCH_DISPLAY, "--source", movie, "--dest", display, "--part", "b")
        output_hash = sha256(display)
        print(f"[build] Phase 3(b) sha256={output_hash}")
        if output_hash.lower() != EXPECTED_PHASE3_B_SHA256:
            raise RuntimeError(f"Phase 3(b) output hash mismatch: {output_hash}")
        run_patch(PATCH_PATH, "--source", display, "--dest", path_fixed)
        output_hash = sha256(path_fixed)
        print(f"[build] Phase 4 sha256={output_hash}")
        if output_hash.lower() != EXPECTED_PHASE4_SHA256:
            raise RuntimeError(f"Phase 4 output hash mismatch: {output_hash}")
        run_patch(PATCH_STRETCH, "--source", path_fixed, "--dest", stretch_fixed)
        output_hash = sha256(stretch_fixed)
        print(f"[build] Phase 5 sha256={output_hash}")
        if output_hash.lower() != EXPECTED_PHASE5_SHA256:
            raise RuntimeError(f"Phase 5 output hash mismatch: {output_hash}")
        return paths, stretch_fixed, output_hash
    except Exception:
        for path in paths:
            path.unlink(missing_ok=True)
        raise


def build_proxy() -> tuple[Path, str, Path, str]:
    command = [str(PYTHON), str(PROXY_BUILD)]
    print("[build]", " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=REPO_ROOT)
    if completed.returncode != 0:
        raise RuntimeError(f"proxy build failed ({completed.returncode})")
    if not PROXY_DLL.is_file():
        raise RuntimeError(f"proxy build did not produce {PROXY_DLL}")
    if not CMGR_PROXY_DLL.is_file():
        raise RuntimeError(f"proxy build did not produce {CMGR_PROXY_DLL}")
    actual_proxy = sha256(PROXY_DLL)
    actual_cmgr_proxy = sha256(CMGR_PROXY_DLL)
    print(f"[build] proxy sha256={actual_proxy}")
    print(f"[build] CMGR32 proxy sha256={actual_cmgr_proxy}")
    return PROXY_DLL, actual_proxy, CMGR_PROXY_DLL, actual_cmgr_proxy


def replace_proxy_hashes(data: bytes, proxy_hash: str, cmgr_proxy_hash: str, name: str) -> bytes:
    text = data.decode("utf-8-sig")
    if name in {"apply.ps1", "revert.ps1"}:
        text, count_qtim = re.subn(
            r'(?m)(\$ExpectedProxySha256\s*=\s*")[0-9a-fA-F]{64}(")',
            rf"\g<1>{proxy_hash}\g<2>",
            text,
        )
        text, count_cmgr = re.subn(
            r'(?m)(\$ExpectedCmgrProxySha256\s*=\s*")[0-9a-fA-F]{64}(")',
            rf"\g<1>{cmgr_proxy_hash}\g<2>",
            text,
        )
        if count_qtim != 1 or count_cmgr != 1:
            raise RuntimeError(f"could not update proxy hash in {name}")
    elif name == "README.txt":
        text, count_qtim = re.subn(
            r"(?im)(Release QTIM32\.dll proxy:\s*\r?\n\s*)[0-9a-f]{64}",
            rf"\g<1>{proxy_hash}",
            text,
        )
        text, count_cmgr = re.subn(
            r"(?im)(Release CMGR32\.dll companion proxy:\s*\r?\n\s*)[0-9a-f]{64}",
            rf"\g<1>{cmgr_proxy_hash}",
            text,
        )
        if count_qtim != 1 or count_cmgr != 1:
            raise RuntimeError("could not update proxy hash in README.txt")
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


def write_package_checksums(package_dir: Path) -> None:
    lines = [
        "# SHA256 for the v1.1.0 distribution files.",
        "# The original game files and patched executable are intentionally excluded.",
    ]
    for name in CHECKSUM_ENTRIES:
        lines.append(f"{sha256(package_dir / name).upper()}  {name}")
    (package_dir / "checksums.txt").write_bytes(("\r\n".join(lines) + "\r\n").encode("utf-8"))


def assert_package_allowlist(package_dir: Path) -> None:
    actual = tuple(sorted(
        path.relative_to(package_dir).as_posix()
        for path in package_dir.rglob("*") if path.is_file()
    ))
    expected = tuple(sorted(PACKAGE_ENTRIES))
    if actual != expected:
        raise RuntimeError(f"release allowlist mismatch: actual={actual}, expected={expected}")


def write_deterministic_zip(package_dir: Path, target: Path) -> None:
    with zipfile.ZipFile(
        target,
        "w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for name in PACKAGE_ENTRIES:
            data = (package_dir / name).read_bytes()
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 0
            info.create_version = 20
            info.extract_version = 20
            info.flag_bits = 0
            info.external_attr = 0
            info.internal_attr = 0
            archive.writestr(info, data)


def build_release(
    proxy: Path, proxy_hash: str, cmgr_proxy: Path, cmgr_proxy_hash: str
) -> tuple[Path, Path, Path, Path]:
    stage_parent = REPO_ROOT / "work" / "build_staging"
    stage_parent.mkdir(parents=True, exist_ok=True)
    stage_root = stage_parent / f"release_stage_{uuid.uuid4().hex}"
    stage_root.mkdir()
    package_stage = stage_root / RELEASE_DIR.name
    package_stage.mkdir()
    (package_stage / "patch_files").mkdir()
    for name in TEMPLATE_TEXT_ENTRIES + TEMPLATE_BINARY_ENTRIES:
        template = RELEASE_DIR / name
        if not template.is_file():
            raise FileNotFoundError(f"release template missing: {template}")
        data = template.read_bytes()
        if name in TEMPLATE_TEXT_ENTRIES:
            data = replace_proxy_hashes(data, proxy_hash, cmgr_proxy_hash, Path(name).name)
        (package_stage / name).write_bytes(data)
    shutil.copy2(proxy, package_stage / "patch_files" / "QTIM32.dll")
    shutil.copy2(cmgr_proxy, package_stage / "patch_files" / "CMGR32.dll")
    write_package_checksums(package_stage)
    assert_package_allowlist(package_stage)

    zip_stage = stage_root / RELEASE_ZIP.name
    write_deterministic_zip(package_stage, zip_stage)
    zip_checksum_stage = stage_root / RELEASE_ZIP_CHECKSUMS.name
    zip_checksum_stage.write_bytes(
        (
            "# SHA256 for the v1.1.0 release archive.\r\n"
            f"{sha256(zip_stage).upper()}  {RELEASE_ZIP.name}\r\n"
        ).encode("utf-8")
    )
    print(f"[build] package staged at {package_stage}")
    print(f"[build] deterministic ZIP sha256={sha256(zip_stage)}")
    return stage_root, package_stage, zip_stage, zip_checksum_stage


def remove_path(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def publish_atomically(
    package_stage: Path, zip_stage: Path, zip_checksum_stage: Path
) -> None:
    token = uuid.uuid4().hex
    old_package = RELEASE_PARENT / f".old_package_{token}"
    old_zip = RELEASE_PARENT / f".old_zip_{token}.tmp"
    old_zip_checksum = RELEASE_PARENT / f".old_checksums_zip_{token}.tmp"
    moved_old: list[tuple[Path, Path]] = []
    published: list[Path] = []
    try:
        for current, backup in (
            (RELEASE_DIR, old_package),
            (RELEASE_ZIP, old_zip),
            (RELEASE_ZIP_CHECKSUMS, old_zip_checksum),
        ):
            if current.exists():
                os.replace(current, backup)
                moved_old.append((current, backup))
        os.replace(package_stage, RELEASE_DIR)
        published.append(RELEASE_DIR)
        os.replace(zip_stage, RELEASE_ZIP)
        published.append(RELEASE_ZIP)
        os.replace(zip_checksum_stage, RELEASE_ZIP_CHECKSUMS)
        published.append(RELEASE_ZIP_CHECKSUMS)
    except Exception:
        for path in reversed(published):
            remove_path(path)
        for current, backup in reversed(moved_old):
            if backup.exists():
                os.replace(backup, current)
        raise
    else:
        for _current, backup in moved_old:
            remove_path(backup)
    print(f"[build] atomically published {RELEASE_DIR}")
    print(f"[build] atomically published {RELEASE_ZIP}")
    print(f"[build] atomically published {RELEASE_ZIP_CHECKSUMS}")


def update_runtime_executable(artifact: Path, destination: Path, expected_hash: str) -> None:
    destination = destination.resolve()
    if destination == artifact.resolve():
        raise RuntimeError("refusing to publish the build staging artifact in place")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temp_fd, temp_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    os.close(temp_fd)
    temp = Path(temp_name)
    try:
        shutil.copy2(artifact, temp)
        if sha256(temp).lower() != expected_hash.lower():
            raise RuntimeError("runtime executable staging hash mismatch")
        os.replace(temp, destination)
    finally:
        if temp.exists():
            temp.unlink()
    if sha256(destination).lower() != expected_hash.lower():
        raise RuntimeError("runtime executable publish hash mismatch")
    print(f"[build] runtime executable updated: {destination} sha256={expected_hash}")


def run_release_test(zip_path: Path) -> None:
    if not RELEASE_TEST.is_file():
        raise FileNotFoundError(RELEASE_TEST)
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(RELEASE_TEST),
        "-ReleaseZip",
        str(zip_path),
    ]
    print("[build]", " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=REPO_ROOT)
    if completed.returncode != 0:
        raise RuntimeError(f"release cycle test failed ({completed.returncode})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=REPO_ROOT / "source_exe" / "gundam.exe",
        help="read-only source executable",
    )
    parser.add_argument(
        "--game-dir",
        type=Path,
        default=REPO_ROOT / "run" / "GT",
        help="verification game directory receiving gundam.exe",
    )
    parser.add_argument(
        "--expected-source-sha256",
        default=EXPECTED_SOURCE_SHA256,
        help="expected source hash for the read-only baseline",
    )
    parser.add_argument(
        "--skip-release-test",
        action="store_true",
        help="skip the automatic test_release_cycle.ps1 run against the generated ZIP",
    )
    args = parser.parse_args()

    artifact_paths: list[Path] | None = None
    release_stage_root: Path | None = None
    try:
        artifact_paths, artifact, output_hash = build_executable_artifact(
            args.source, args.expected_source_sha256
        )
        # The runtime executable is deliberately untouched until this succeeds.
        proxy, proxy_hash, cmgr_proxy, cmgr_proxy_hash = build_proxy()
        (
            release_stage_root,
            package_stage,
            zip_stage,
            zip_checksum_stage,
        ) = build_release(proxy, proxy_hash, cmgr_proxy, cmgr_proxy_hash)
        publish_atomically(package_stage, zip_stage, zip_checksum_stage)
        if not args.skip_release_test:
            run_release_test(RELEASE_ZIP)
        update_runtime_executable(artifact, args.game_dir / "gundam.exe", output_hash)
        print("[build] build_all completed")
        return 0
    except Exception as exc:
        print(f"build_all failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if release_stage_root and release_stage_root.exists():
            shutil.rmtree(release_stage_root, ignore_errors=True)
        if artifact_paths:
            for path in artifact_paths:
                path.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
