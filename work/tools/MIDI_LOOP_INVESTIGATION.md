# PEGASUS BGM restart investigation (2026-10-17)

The reported missing opening is reproducible with the Windows 11 32-bit MCI
sequencer. The cached MIDI retains the opening, and rewind reports position
zero, but the normal seek/play restart does not output the eight note-on
events at tick zero. Its next note-ons match the source's 200 ms position.
This is stronger evidence than a perceived timing difference, although the
test observes MIDI driver messages rather than recording the audible output.

## File and loop boundary

The original executable's ship table at `0x0043D820` maps ship kind 2
(`PEGS` in the name table at `0x0043B500`) to BGM index 3. The path table at
`0x0043DC98 + index * 20` selects **Sound\M07gm.mid**.

`audit_midi_loop.py` independently parses the original and the existing
execution-copy cache. For M07:

- 13 tracks, 480 ticks per quarter note, initial tempo 400,000 microseconds
  per quarter note (150 BPM).
- Eight note-on events at tick zero; the next note-ons are at tick 240,
  or 200 ms.
- All **10,747 non-EOT events**, their timestamps, and every track's bytes
  before EOT are identical between source and cache.
- Original final EOT: **373.2 seconds**. Cache final EOT: **171.6 seconds**,
  matching the last note-off. No earlier event is trimmed.
- The final boundary is also a bar boundary: 55 complete 4/4 bars after the
  last time-signature change at tick 100320. This does not establish the
  composer's intended acoustic release tail, but there is no evidence here
  of the cached loop cutting away the beginning or final written note.
- Source SHA256:
  `ac4a7839e55c8aa94df7c52c317fb64cade9bf2c7deb96368314ad90cab0e9b3`.
- Cache SHA256:
  `a810d6f8cc35ef42cb21c5bb916df31242cfea054338216087d2795e299379c2`.

All ten existing M*GM cache files also preserve their non-EOT event bytes
and times. Absence of a cache for another song is not counted as a comparison
failure or proof of correct playback.

## Runtime hypothesis and evidence

Target: `FUN_00421600` at `0x00421600`. When the MCI device stops, the game
issues `MCI_SEEK` with `MCI_SEEK_TO_START`, then `MCI_PLAY` without `MCI_FROM`.
The proxy's normal loop correction substitutes the shorter file at open;
it does not replace this seek/play sequence. Its minimize/resume fallback
also has an end-position seek/play path.

`mci_restart_probe.c` opens a MIDI through the real 32-bit MCI sequencer and
temporarily observes that process's `mciseq.dll` imports of
`midiOutShortMsg` and `midiOutLongMsg`. It forwards every call, buffers logs
in memory, closes the device, and restores both import slots. It does not
load the game, edit any Windows DLL on disk, or change saves/settings.
Normal MIDI playback produces short channel messages packed in long-message
buffers on the tested driver, so observing only `midiOutShortMsg` is
insufficient.

Baseline input: the unmodified existing M07 cache, played to **natural end**
before rewinding. Polling is at 100 ms as in the patched game. MCI calls
succeeded; the position reported zero after rewind. The harness uses PLAY
without notification rather than a game window's MCI_NOTIFY callback; it
tests the sequencer's note output, not game notification delivery.

Observation: initial playback outputs all eight tick-zero note-ons. After
natural completion and rewind, none of those eight onset events are emitted
by either observed output API. The first output buffer instead restores
controllers and includes note-offs and the three note-ons belonging to tick
240. There are no short-message note-ons outside those captured buffers.
This explains a missing first attack of approximately **0.2 seconds**;
different instruments have different subsequent onset times.

The tested `SysWOW64/mciseq.dll` SHA256 is
`f1db56d1c881512c105a48ee94e4a078007cdec5afbb38ce80e8bbb6b54a3b06`.
Do not assume all Windows versions or MIDI devices behave identically.

## Separate workaround experiments

After the natural-end baseline, each experiment used a fresh harness
process: play the opening, seek to end, rewind to start, then replay with
only the listed change. These are standalone experiments, not game patches.

| Change on replay | First eight onset events restored? |
|---|---|
| Add `MCI_FROM` with `dwFrom=0` | No |
| Close and reopen the identical MIDI, then play | **Yes, all eight** |
| Add 12 ticks (10 ms) to every track's first delta in a separate fixture, preserving all event payloads/intervals | No |

The three experiments are sufficient to stop adding local timing tweaks and
reassess the restart mechanism. Reopening provides a demonstrated reference
for correct note emission. A production solution must account for device-ID
changes, callback delivery, startup cost, pause/resume, failure recovery, and
the game's other sequencer initialization; blindly reopening inside the
current hook would be unsafe. Simply shifting the loop point or padding the
head is not supported by these tests.

## Reproduction

Build from the `work/tools/mci_test` directory with MinGW 32-bit bin on PATH:

```powershell
gcc -m32 -O2 -Wall -Wextra mci_restart_probe.c -lwinmm -o mci_restart_probe.exe
```

From the repository root, use the existing execution-copy cache:

```powershell
work/tools/mci_test/mci_restart_probe.exe run/GT01/MidiLoop/M07gm.mid natural > work/analysis/mci_restart_m07_natural.csv
python work/tools/audit_midi_loop.py --probe work/analysis/mci_restart_m07_natural.csv --output work/analysis/midi_loop_audit.json
```

The natural-end run takes about three minutes and plays audio. Use `quick`
instead of `natural` for a forced end; append `from` or `reopen` to select
one workaround experiment. A probe's PASS means API execution/capture
succeeded; `first_packet_note_ons_match` reports the onset comparison.
An oversized long-message buffer causes the analyzer to reject truncated
data rather than drawing conclusions from it.

No production proxy, executable, source asset, installed game, or release
was changed. **ユーザー実機確認待ち** after a restart fix is prepared: record
EXE/proxy hashes, enter the PEGASUS scene using M07, compare the first attack
on scene entry with two natural repeats (about 171.6 seconds each), and check
effects, progression, scene changes, and minimize/restore. This investigation
does not claim that in-game acceptance check has passed.

## Restart fix experiment (2026-10-17)

### Why the rewind loses notes

With M07, the first output packet after a rewind holds the tick-0
controller/program events, followed straight away by the tick-240 events
(note-offs and the next note-ons). In the first packet after an open, the
tick-0 note-ons sit among those same controllers. So after a rewind the
sequencer re-sends the state of the first event group and starts playing
from the next group, and that first group's notes are lost.

More probe experiments (each in a fresh process, quick mode) that did **not**
restore the notes:

| Change on replay | Restored? |
|---|---|
| Only the tick-0 note-ons moved to tick 1 (fixture; controllers stay at 0) | No; tick-240 events again come straight after the controllers |
| `MCI_STOP` before the seek | No |
| `MCI_SET` `MCI_SEQ_SET_PORT` = MIDI mapper after the seek | No |
| `MCI_SET` time format ms, then back to song pointer | No |

These failed tweaks came in a row, so I stopped and reassessed. The
mechanism to change is the restart itself (a fresh device), not the timing
or the device settings.

### Reopen cost and the spare-device alternative

- A close/reopen at the game's rewind fixes the notes, but `MCI_OPEN` takes
  about 690 ms in the game (and 3.6-4.2 s in the probe). The game thread and
  the music stop for that long at every loop. Measured in game:
  `midi_restart total_ms=680-697`.
- A spare device on the same file (a unique alias is required, otherwise
  error 265) can be opened while the first plays. Switching to it then costs
  only close 28 ms + play 95 ms, and all 8 notes play. But a device opened
  on another thread cannot be used from the game thread (error 303, "cannot
  run on an automatically opened device"), even while the opener thread is
  alive. Opened on the game thread, it stalls the game for 0.7 s instead, so
  this approach was not used.
- Timing `mciseq.dll`'s imports showed where the time goes: open and close
  wait for its stream thread with `Sleep(4)` polls (about 266 per thread for
  one reopen; each lasts about 15 ms at the default timer resolution). With
  that `Sleep(4)` turned into `Sleep(0)`, the reopen takes 8 ms in the probe
  and the first packet has all 8 tick-0 note-ons.

### Implemented (proxy, not yet released)

`qtim_compat_proxy.c`: tracked sequencer devices are reopened instead of
rewound. `mciseq.dll` is pinned and its `Sleep` import is hooked, active
only during the proxy's reopen. See the proxy README.

In game (01 edition execution copy, new game idle on M02, 36.4 s of music, 3
loops): `midi_restart real=1->1 total_ms=1.2-1.4`, followed by the game's
`MCI_PLAY` (about 80 ms), the same cost as the original rewind and play.

Sol review (gpt-6.1-sol, 2 rounds).
Round 1 found:
- **High:** the `Sleep` hook could be called before its original pointer was set.
- **Medium:** an ID collision overwrote a live mapping.
- **Medium:** a failed reopen left the BGM permanently silent.
- **Medium:** a partial hook failure released the DLL while its hook record remained.

All were fixed. The original is now set first; a colliding new device gets a
public ID from 0xF000 up; a dead entry is reopened on the game's next command
(at most once a second); and `mciseq.dll` stays loaded once its slot is found.
The fast path also changed to `SwitchToThread()` and is capped at 50 ms per
reopen. Round 2: no blocking issues. The in-game numbers above were measured
with the build before these fixes.

Reviewed build (proxy SHA256 `1ec86b68...`) in game: a normal loop
(`real=1->1 total_ms=1.4`); minimize during M02, which pauses it at 292/340;
restore, which resumes; and the song end while resumed, where the `MCI_RESUME`
fallback reopens at 340/340 (`total_ms=1.6`) and plays. The next loop then
used the normal path, and the game kept running.

User in-game check (2026-10-17, with the build before the review fixes, which
behaves the same): the BGM loops as the user remembers it from the original.

Remaining checks for a later release test: compare the opening of a looped
BGM with its first play, for example PEGASUS (M07, about 171.6 s) or M02
(about 36 s). Also check minimize/restore at a song end, BGM changes after
loops, sound effects, and progression.
