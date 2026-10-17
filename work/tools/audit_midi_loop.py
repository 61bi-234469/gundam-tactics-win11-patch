"""Compare original BGM MIDI events with the proxy's cached loop copies.

Read-only: no game launch, playback, MIDI rewriting, or save access.
Usage: python work/tools/audit_midi_loop.py [--output PATH]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from audit_game_boundaries import pe_reader

ROOT = Path(__file__).resolve().parents[2]


def vlq(data, pos, end):
    value = 0
    for _ in range(4):
        if pos >= end:
            raise ValueError("Truncated VLQ")
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 127)
        if byte < 128:
            return value, pos
    raise ValueError("Invalid VLQ")


def parse(path):
    data = path.read_bytes()
    if data[:4] != b"MThd" or len(data) < 14:
        raise ValueError("Invalid MIDI header")
    header_size, fmt, count, division = struct.unpack_from(">IHHH", data, 4)
    if fmt > 1 or division == 0 or division & 0x8000:
        raise ValueError("Only PPQ format 0/1 supported")
    pos = 8 + header_size
    tracks = []
    for track in range(count):
        if data[pos:pos + 4] != b"MTrk":
            raise ValueError("Missing track")
        length = struct.unpack_from(">I", data, pos + 4)[0]
        pos += 8
        start, end = pos, pos + length
        if end > len(data):
            raise ValueError("Truncated track")
        events, tick, running, prefix = [], 0, None, None
        while pos < end:
            event_start = pos
            delta, pos = vlq(data, pos, end)
            tick += delta
            status = data[pos]
            if status & 128:
                pos += 1
            elif running is not None:
                status = running
            else:
                raise ValueError("Missing running status")
            if status == 255:
                kind = data[pos]
                pos += 1
                size, pos = vlq(data, pos, end)
                payload = data[pos:pos + size]
                pos += size
                event = (tick, status, kind, payload.hex())
                if kind == 47:
                    prefix = data[start:event_start]
            elif status in (240, 247):
                size, pos = vlq(data, pos, end)
                event = (tick, status, -1, data[pos:pos + size].hex())
                pos += size
                running = None
            elif 128 <= status < 240:
                running = status
                size = 1 if status & 240 in (192, 208) else 2
                event = (tick, status, -1, data[pos:pos + size].hex())
                pos += size
            else:
                raise ValueError("Unsupported event")
            if pos > end:
                raise ValueError("Truncated event")
            events.append(event)
            if status == 255 and kind == 47:
                if pos != end:
                    raise ValueError("Data after EOT")
                break
        if prefix is None:
            raise ValueError("Missing EOT")
        tracks.append({"events": events, "prefix": prefix})
    if pos != len(data):
        raise ValueError("Trailing data")
    return data, division, tracks


def summarize(path):
    data, division, tracks = parse(path)
    events = sorted((event + (track,) for track, item in enumerate(tracks)
                     for event in item["events"]), key=lambda e: (e[0], e[4]))
    tempos = [(e[0], int(e[3], 16)) for e in events if e[1:3] == (255, 81)]

    def ms(tick):
        total, previous, tempo = 0.0, 0, 500000
        for at, value in tempos:
            if at > tick:
                break
            total += (at - previous) * tempo / division / 1000
            previous, tempo = at, value
        return round(total + (tick - previous) * tempo / division / 1000, 3)

    def describe(e):
        return {"tick": e[0], "ms": ms(e[0]), "status": f"{e[1]:02X}",
                "meta": e[2], "data": e[3], "track": e[4]}

    musical = [e for e in events if e[1:3] != (255, 47)]
    notes = [e for e in events if e[1] & 240 == 144 and bytes.fromhex(e[3])[1] != 0]
    return {
        "sha256": hashlib.sha256(data).hexdigest(), "ppq": division,
        "tracks": len(tracks), "non_eot_events": len(musical),
        "first_notes": [describe(e) for e in notes[:12]],
        "last_events": [describe(e) for e in musical[-12:]],
        "last_non_eot_ms": ms(musical[-1][0]),
        "eot_ms": ms(max(e[0] for e in events if e[1:3] == (255, 47))),
        "tempo_events": [describe(e) for e in events if e[1:3] == (255, 81)],
        "time_signatures": [describe(e) for e in events if e[1:3] == (255, 88)],
        "initial_events": [describe(e) for e in musical if e[0] <= division],
    }, tracks


def summarize_probe(path):
    """Inspect mciseq output captured by mci_restart_probe.c, not audio."""
    packets = {1: [], 2: []}
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    for line in lines:
        if not line.startswith("# long,"):
            continue
        _, phase, milliseconds, length, encoded = line.split(",", 5)
        phase = int(phase)
        if phase not in packets:
            continue
        data = bytes.fromhex(encoded)
        if len(data) != int(length):
            raise ValueError("Truncated MIDI output packet; increase probe buffer")
        pos, messages = 0, []
        while pos < len(data):
            status = data[pos]
            if not 128 <= status < 240:
                raise ValueError("Expected explicit channel-message status")
            size = 2 if status & 240 in (192, 208) else 3
            msg = data[pos:pos + size]
            if len(msg) != size or any(v >= 128 for v in msg[1:]):
                raise ValueError("Malformed channel message")
            messages.append(msg)
            pos += size
        packets[phase].append((float(milliseconds), messages))
    result = {"completed": any(line.startswith("# result,PASS") for line in lines)}
    for phase, name in [(1, "initial"), (2, "restart")]:
        if not packets[phase]:
            result[name] = {"packets": 0}
            continue
        origin = packets[phase][0][0]
        notes = [(round(at - origin, 3), msg.hex())
                 for at, messages in packets[phase] for msg in messages
                 if msg[0] & 240 == 144 and msg[2] != 0]
        result[name] = {
            "packets": len(packets[phase]),
            "first_packet_note_ons": [msg.hex() for msg in packets[phase][0][1]
                                       if msg[0] & 240 == 144 and msg[2] != 0],
            "first_note_ons_relative_ms": notes[:12],
        }
    if packets[1] and packets[2]:
        result["first_packet_note_ons_match"] = (
            result["initial"]["first_packet_note_ons"] ==
            result["restart"]["first_packet_note_ons"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "source_exe_01/Sound")
    parser.add_argument("--cache", type=Path, default=ROOT / "run/GT01/MidiLoop")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--probe", type=Path, nargs="*", default=[])
    args = parser.parse_args()
    exe_data = (ROOT / "source_exe_01/gundam.exe").read_bytes()
    read = pe_reader(exe_data)
    ship_bgm = struct.unpack("<9h", read(0x43D820, 18))
    index = ship_bgm[2]  # PEGASUS is ship kind 2.
    pegasus = read(0x43DC98 + index * 20, 20).split(b"\0")[0].decode("ascii")
    if pegasus.lower() != "sound\\m07gm.mid":
        raise ValueError("Unexpected PEGASUS BGM mapping")
    if read(0x43B500 + 2 * 6, 6).split(b"\0")[0] != b"PEGS":
        raise ValueError("Unexpected PEGASUS ship index")
    result = {"exe_sha256": hashlib.sha256(exe_data).hexdigest(),
              "pegasus_bgm": pegasus, "ship_bgm_indices": ship_bgm, "files": {}}
    for path in sorted(args.source.glob("M*GM.MID")):
        source, original = summarize(path)
        item = {"source": source}
        cache = args.cache / path.name
        if cache.exists():
            trimmed, copy = summarize(cache)
            item["cache"] = trimmed
            item["events_before_eot_byte_identical"] = (
                len(original) == len(copy) and
                all(a["prefix"] == b["prefix"] for a, b in zip(original, copy)))
            item["timed_non_eot_events_identical"] = (
                len(original) == len(copy) and
                all([e for e in a["events"] if e[1:3] != (255, 47)] ==
                    [e for e in b["events"] if e[1:3] != (255, 47)]
                    for a, b in zip(original, copy)))
        result["files"][path.name] = item
    if args.probe:
        result["mci_probes"] = {path.name: summarize_probe(path) for path in args.probe}
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded + "\n", encoding="utf-8")
    for name, item in result["files"].items():
        src, cache = item["source"], item.get("cache")
        print(name, "first_note_ms", src["first_notes"][0]["ms"],
              "last_event_ms", src["last_non_eot_ms"], "eot_ms", src["eot_ms"],
              "cache_eot_ms", cache["eot_ms"] if cache else None,
              "identical", item.get("events_before_eot_byte_identical"))
    print("PEGASUS:", pegasus)


if __name__ == "__main__":
    main()
