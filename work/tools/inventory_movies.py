"""Inventory QuickTime MOV assets without depending on QuickTime or ffprobe.

The goal is not to fully decode media.  This script reads enough QuickTime
atoms to classify the bundled movie assets by codec, dimensions, duration,
and track count so the replacement playback pipeline can be planned from
facts instead of ad hoc samples.
"""

from __future__ import annotations

import argparse
import csv
import json
import struct
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable


CONTAINER_ATOMS = {
    b"moov",
    b"trak",
    b"mdia",
    b"minf",
    b"stbl",
    b"edts",
    b"udta",
}


@dataclass
class TrackInfo:
    handler: str = ""
    codec: str = ""
    width: int | None = None
    height: int | None = None
    timescale: int | None = None
    duration_units: int | None = None
    sample_count: int | None = None

    @property
    def duration_seconds(self) -> float | None:
        if not self.timescale or self.duration_units is None:
            return None
        return self.duration_units / self.timescale


@dataclass
class MovieInfo:
    path: str
    size: int
    movie_timescale: int | None
    movie_duration_units: int | None
    tracks: list[TrackInfo]
    parse_error: str = ""

    @property
    def movie_duration_seconds(self) -> float | None:
        if not self.movie_timescale or self.movie_duration_units is None:
            return None
        return self.movie_duration_units / self.movie_timescale


def be_u16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def be_u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def atom_children(data: bytes, start: int, end: int) -> Iterable[tuple[bytes, int, int]]:
    pos = start
    while pos + 8 <= end:
        size = be_u32(data, pos)
        kind = data[pos + 4:pos + 8]
        header = 8
        if size == 1:
            if pos + 16 > end:
                return
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or pos + size > end:
            return
        yield kind, pos + header, pos + size
        pos += size


def parse_mvhd(data: bytes, start: int, end: int) -> tuple[int | None, int | None]:
    if start + 20 > end:
        return None, None
    version = data[start]
    if version == 0 and start + 20 <= end:
        return be_u32(data, start + 12), be_u32(data, start + 16)
    if version == 1 and start + 32 <= end:
        timescale = be_u32(data, start + 20)
        duration = struct.unpack_from(">Q", data, start + 24)[0]
        return timescale, int(duration)
    return None, None


def parse_mdhd(track: TrackInfo, data: bytes, start: int, end: int) -> None:
    if start + 20 > end:
        return
    version = data[start]
    if version == 0 and start + 20 <= end:
        track.timescale = be_u32(data, start + 12)
        track.duration_units = be_u32(data, start + 16)
    elif version == 1 and start + 32 <= end:
        track.timescale = be_u32(data, start + 20)
        track.duration_units = int(struct.unpack_from(">Q", data, start + 24)[0])


def parse_hdlr(track: TrackInfo, data: bytes, start: int, end: int) -> None:
    if start + 12 > end:
        return
    handler = data[start + 8:start + 12]
    # Old QuickTime files also contain data-handler atoms such as "alis".
    # For playback planning we only want the media handler.
    if handler in {b"vide", b"soun"}:
        track.handler = handler.decode("latin-1", errors="replace")


def parse_stsd(track: TrackInfo, data: bytes, start: int, end: int) -> None:
    if start + 16 > end:
        return
    entry_count = be_u32(data, start + 4)
    pos = start + 8
    if entry_count < 1 or pos + 8 > end:
        return
    entry_size = be_u32(data, pos)
    codec = data[pos + 4:pos + 8]
    track.codec = codec.decode("latin-1", errors="replace")
    entry_end = min(end, pos + entry_size)
    # QuickTime 2.x visual sample descriptions put width/height at +24/+26
    # from the sample entry start.  ISO BMFF visual sample entries use +32,
    # so fall back if the QuickTime-era offsets are empty.
    if track.handler == "vide" and pos + 28 <= entry_end:
        width = be_u16(data, pos + 24)
        height = be_u16(data, pos + 26)
        if (not width or not height) and pos + 36 <= entry_end:
            width = be_u16(data, pos + 32)
            height = be_u16(data, pos + 34)
        if width and height:
            track.width = width
            track.height = height


def parse_stts(track: TrackInfo, data: bytes, start: int, end: int) -> None:
    if start + 16 > end:
        return
    entry_count = be_u32(data, start + 4)
    if entry_count < 1:
        return
    pos = start + 8
    total = 0
    for _ in range(entry_count):
        if pos + 8 > end:
            break
        total += be_u32(data, pos)
        pos += 8
    track.sample_count = total


def parse_track(data: bytes, start: int, end: int) -> TrackInfo:
    track = TrackInfo()

    def walk(node_start: int, node_end: int) -> None:
        for kind, child_start, child_end in atom_children(data, node_start, node_end):
            if kind == b"mdhd":
                parse_mdhd(track, data, child_start, child_end)
            elif kind == b"hdlr":
                parse_hdlr(track, data, child_start, child_end)
            elif kind == b"stsd":
                parse_stsd(track, data, child_start, child_end)
            elif kind == b"stts":
                parse_stts(track, data, child_start, child_end)
            elif kind in CONTAINER_ATOMS:
                walk(child_start, child_end)

    walk(start, end)
    return track


def parse_movie(path: Path, root: Path) -> MovieInfo:
    data = path.read_bytes()
    movie_timescale = None
    movie_duration = None
    tracks: list[TrackInfo] = []

    def walk(start: int, end: int) -> None:
        nonlocal movie_timescale, movie_duration
        for kind, child_start, child_end in atom_children(data, start, end):
            if kind == b"mvhd":
                movie_timescale, movie_duration = parse_mvhd(data, child_start, child_end)
            elif kind == b"trak":
                tracks.append(parse_track(data, child_start, child_end))
            elif kind in CONTAINER_ATOMS:
                walk(child_start, child_end)

    info = MovieInfo(
        path=str(path.relative_to(root)),
        size=path.stat().st_size,
        movie_timescale=None,
        movie_duration_units=None,
        tracks=[],
    )
    try:
        walk(0, len(data))
        info.movie_timescale = movie_timescale
        info.movie_duration_units = movie_duration
        info.tracks = tracks
    except Exception as exc:  # pragma: no cover - diagnostic script
        info.parse_error = f"{type(exc).__name__}: {exc}"
    return info


def iter_movies(root: Path) -> Iterable[Path]:
    seen: set[str] = set()
    for suffix in ("*.mov", "*.MOV"):
        for path in root.rglob(suffix):
            key = str(path.resolve()).lower()
            if key in seen:
                continue
            seen.add(key)
            yield path


def write_csv(infos: list[MovieInfo], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "path",
            "size",
            "movie_duration_seconds",
            "track_index",
            "handler",
            "codec",
            "width",
            "height",
            "track_duration_seconds",
            "sample_count",
            "parse_error",
        ])
        for info in infos:
            if not info.tracks:
                writer.writerow([
                    info.path,
                    info.size,
                    info.movie_duration_seconds,
                    "",
                    "",
                    "",
                    "",
                    "",
                    "",
                    "",
                    info.parse_error,
                ])
            for idx, track in enumerate(info.tracks):
                writer.writerow([
                    info.path,
                    info.size,
                    info.movie_duration_seconds,
                    idx,
                    track.handler,
                    track.codec,
                    track.width,
                    track.height,
                    track.duration_seconds,
                    track.sample_count,
                    info.parse_error,
                ])


def print_summary(infos: list[MovieInfo]) -> None:
    codec_counts: dict[str, int] = {}
    dimensions: dict[str, int] = {}
    handlers: dict[str, int] = {}
    errors = 0
    for info in infos:
        if info.parse_error:
            errors += 1
        for track in info.tracks:
            handlers[track.handler or "?"] = handlers.get(track.handler or "?", 0) + 1
            if track.codec:
                codec_counts[track.codec] = codec_counts.get(track.codec, 0) + 1
            if track.width and track.height:
                key = f"{track.width}x{track.height}"
                dimensions[key] = dimensions.get(key, 0) + 1

    print(f"movies: {len(infos)}")
    print(f"parse_errors: {errors}")
    print("handlers:")
    for key, count in sorted(handlers.items(), key=lambda item: (-item[1], item[0])):
        print(f"  {key}: {count}")
    print("codecs:")
    for key, count in sorted(codec_counts.items(), key=lambda item: (-item[1], item[0])):
        print(f"  {key}: {count}")
    print("dimensions:")
    for key, count in sorted(dimensions.items(), key=lambda item: (-item[1], item[0])):
        print(f"  {key}: {count}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--movie-root", type=Path, default=Path("Movie"))
    parser.add_argument("--csv", type=Path, default=Path(__file__).resolve().parents[1] / "analysis" / "movie_inventory.csv")
    parser.add_argument("--json", type=Path, default=None)
    args = parser.parse_args()

    root = args.movie_root.resolve()
    infos = [parse_movie(path, root) for path in sorted(iter_movies(root))]
    write_csv(infos, args.csv)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps([asdict(info) for info in infos], ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
    print_summary(infos)
    print(f"wrote {args.csv}")
    if args.json:
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
