"""Generate BMP frame caches for movie files used by the QTIM32 proxy.

The proxy maps:

    Movie\\ENHANCE\\ZAKU_0.mov

to:

    MovieCache\\ENHANCE\\ZAKU_0\\frame_0001.bmp

This script keeps that layout and skips caches that already contain
``frame_0001.bmp`` unless ``--overwrite`` is passed.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
DEFAULT_GAME_DIR = REPO_ROOT / "run" / "GT01"
CACHE_MISS_RE = re.compile(r'\bcache_miss\b.*\bpath="([^"]+)"')


def cache_dir_for_movie(game_dir: Path, movie: Path) -> Path:
    rel = movie.relative_to(game_dir / "Movie")
    return game_dir / "MovieCache" / rel.with_suffix("")


def iter_movies(game_dir: Path) -> list[Path]:
    movie_dir = game_dir / "Movie"
    return sorted(
        [p for p in movie_dir.rglob("*") if p.is_file() and p.suffix.lower() == ".mov"],
        key=lambda p: str(p).lower(),
    )


def find_ffmpeg(explicit: Path | None) -> str | None:
    if explicit:
        return str(explicit)
    found = shutil.which("ffmpeg")
    if found:
        return found
    for env_name in ("FFMPEG", "FFMPEG_PATH"):
        value = os.environ.get(env_name)
        if value and Path(value).exists():
            return value
    for candidate in (
        Path(r"C:\ffmpeg\bin\ffmpeg.exe"),
        Path(r"C:\Program Files\ffmpeg\bin\ffmpeg.exe"),
        Path(r"C:\ProgramData\chocolatey\bin\ffmpeg.exe"),
        Path(r"C:\msys64\mingw64\bin\ffmpeg.exe"),
        Path(r"C:\msys64\usr\bin\ffmpeg.exe"),
    ):
        if candidate.exists():
            return str(candidate)
    return None


def normalize_movie_rel_path(value: str) -> str:
    rel = value.replace("/", "\\")
    if rel.lower().startswith("movie\\"):
        rel = rel[6:]
    return rel


def read_trace_cache_misses(trace_path: Path) -> set[str]:
    misses: set[str] = set()
    with trace_path.open("r", encoding="mbcs", errors="replace") as f:
        for line in f:
            match = CACHE_MISS_RE.search(line)
            if match:
                misses.add(normalize_movie_rel_path(match.group(1)))
    return misses


def remove_existing_frames(cache_dir: Path) -> None:
    for frame in cache_dir.glob("frame_*.bmp"):
        frame.unlink()


def convert_movie(ffmpeg: str, movie: Path, cache_dir: Path, overwrite: bool) -> int:
    cache_dir.mkdir(parents=True, exist_ok=True)
    if overwrite:
        remove_existing_frames(cache_dir)

    output_pattern = cache_dir / "frame_%04d.bmp"
    cmd = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(movie),
        str(output_pattern),
    ]
    return subprocess.call(cmd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--game-dir",
        type=Path,
        default=DEFAULT_GAME_DIR,
        help="Game directory containing Movie/ and MovieCache/.",
    )
    parser.add_argument("--ffmpeg", type=Path, help="Explicit path to ffmpeg.exe.")
    parser.add_argument("--only", help="Case-insensitive regex matched against Movie-relative paths.")
    parser.add_argument(
        "--from-trace",
        type=Path,
        help="Select MOV paths from cache_miss lines in a qtim_compat_trace.log.",
    )
    parser.add_argument("--limit", type=int, help="Maximum number of movies to process.")
    parser.add_argument("--overwrite", action="store_true", help="Regenerate existing frame caches.")
    parser.add_argument("--dry-run", action="store_true", help="List planned conversions without running ffmpeg.")
    args = parser.parse_args()

    game_dir = args.game_dir.resolve()
    movie_dir = game_dir / "Movie"
    if not movie_dir.exists():
        print(f"movie directory not found: {movie_dir}")
        return 1

    pattern = re.compile(args.only, re.IGNORECASE) if args.only else None
    trace_misses: set[str] | None = None
    if args.from_trace:
        trace_path = args.from_trace.resolve()
        if not trace_path.exists():
            print(f"trace not found: {trace_path}")
            return 1
        trace_misses = {p.lower() for p in read_trace_cache_misses(trace_path)}

    movies = iter_movies(game_dir)
    selected: list[tuple[Path, Path]] = []
    skipped_existing = 0
    skipped_not_in_trace = 0
    for movie in movies:
        rel = str(movie.relative_to(movie_dir))
        rel_key = normalize_movie_rel_path(rel).lower()
        if trace_misses is not None and rel_key not in trace_misses:
            skipped_not_in_trace += 1
            continue
        if pattern and not pattern.search(rel):
            continue
        cache_dir = cache_dir_for_movie(game_dir, movie)
        if not args.overwrite and (cache_dir / "frame_0001.bmp").exists():
            skipped_existing += 1
            continue
        selected.append((movie, cache_dir))
        if args.limit and len(selected) >= args.limit:
            break

    print(
        "movies_total="
        f"{len(movies)} selected={len(selected)} skipped_existing={skipped_existing} "
        f"skipped_not_in_trace={skipped_not_in_trace}"
    )
    if trace_misses is not None:
        found = {
            normalize_movie_rel_path(str(movie.relative_to(movie_dir))).lower()
            for movie, _cache_dir in selected
        }
        missing_on_disk = sorted(trace_misses - found)
        for rel in missing_on_disk:
            print(f"trace path not found or already cached: {rel}")
    for movie, cache_dir in selected:
        rel = movie.relative_to(movie_dir)
        print(f"{rel} -> {cache_dir.relative_to(game_dir)}")

    if args.dry_run or not selected:
        return 0

    ffmpeg = find_ffmpeg(args.ffmpeg)
    if not ffmpeg:
        print("ffmpeg not found; pass --ffmpeg C:\\path\\to\\ffmpeg.exe")
        return 1

    failures = 0
    for movie, cache_dir in selected:
        rc = convert_movie(ffmpeg, movie, cache_dir, args.overwrite)
        if rc != 0:
            failures += 1
            print(f"FAILED rc={rc}: {movie}")
        else:
            frames = len(list(cache_dir.glob("frame_*.bmp")))
            print(f"OK frames={frames}: {movie}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
