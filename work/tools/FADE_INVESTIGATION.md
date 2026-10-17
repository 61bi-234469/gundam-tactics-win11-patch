# Fade investigation (2026-10-17)

The transition fade can likely be restored without replacing the game's
scene logic. Two separate defects explain a dark residual image and a fade
that looks like an immediate cut. An in-game fix has not yet been verified.

## Checked behavior

The common fade function is `FUN_004090F0` (both editions use the same
executable). It copies the current screen into an 8bpp work DIB for fade-out,
reads its 256-entry color table, scales each RGB component, and calls
`StretchDIBits` at `0x00409283` for each step. Fade-in uses the work DIB
prepared by its caller.

- The loop counter starts at zero (`0x004091BB`), increases by two
  (`0x00409289`), and continues while it is less than 100 (`0x0040928F`).
- Fade-out uses `100 - counter` (`0x004091E3`), so its 50 frames are
  **100, 98, ... 2 percent**. It never draws a zero-percent frame.
- Fade-in uses the counter, so its frames are **0, 2, ... 98 percent**.
- A white component therefore ends fade-out at `255 * 2 / 100 = 5`,
  using integer division. It is dark but not black.
- Inspection of the whole function's existing disassembly/decompilation
  shows no clock, sleep, message polling, or frame pacing between steps.
  Execution speed determines duration.
- The accepted Phase 3 patch changes the copied header's height to -416.
  It corrects image orientation but leaves the palette endpoints and timing
  unchanged.

## Reproduction and limits

Run `python work/tools/audit_fade.py --output <report.json>`. The tool checks
instruction bytes and selected imported API names before modeling the loop.
It never writes an executable, launches the game, or accesses saves.

Hypothesis H1: the fade endpoint leaves a residual image.

- Target: `0x004091E3`, `0x00409289` through `0x00409295`.
- Input: a synthetic grayscale pattern in a 576x416 8bpp DIB, rendered into
  a 32bpp DIB through real Windows `StretchDIBits`.
- Observation: the 2-percent frame has maximum RGB **5**, with **648,336**
  nonzero RGB components. A subsequent zero-percent frame has maximum RGB
  **0**. This supports the reported dark residual image; it is an offscreen
  reproduction, not a screenshot of the game.

Hypothesis H2: unpaced intermediate frames are too short to see.

- Target: the same 50-step loop, plus `display_scale.c` presentation.
- Input: seven batches of the original 50 palette levels on the offscreen
  GDI reproduction, including a final `GdiFlush` per batch.
- Observation: median **6.257 ms** per batch on the investigation machine.
  This includes Python palette generation but excludes actual game/proxy
  overhead, screen presentation, and display refresh. It is not an in-game
  duration measurement.
- Supporting code evidence: the scaling wrapper marks a shadow draw dirty;
  it does not present every `StretchDIBits`. The helper normally presents
  after 12 ms of drawing inactivity or 50 ms since the previous present,
  checking every 4 ms. The fade loop itself does not poll the message queue.
  Consequently intermediate palette levels may be coalesced. The exact
  number of displayed frames remains unmeasured.

The current patched executable also passes the arithmetic/import guards:
SHA256 `607299a2cd7d5aeb1375cd838cd343cd81f9289311cea659d100c700d4035ec4`.
The original passes with SHA256
`38bde2e4513c665d1425fd00203d0000001c5b81bc37899507b6ef7129f238d3`.

## Recommended implementation experiment

Proceed one hypothesis at a time:

1. Test an exact zero-percent terminal frame, scoped to `FUN_004090F0`.
   Keep the existing orientation correction. Check that the old scene is
   fully black during the transition before changing timing.
2. Add fade-only pacing and an explicit scaling presentation boundary for
   each scheduled frame. A provisional 300-500 ms duration is a tuning
   choice, not a recovered original specification. Use elapsed time rather
   than adding a long wait after each frame. Preserve exact 0/100 endpoints.
3. Confirm title, side selection, save/load screen transitions, and battle
   transitions with desktop captures, both with scaling enabled and off.
   Check music/effects and progression. Do not change the global game clock,
   battle tick, movie clock, or QuickTime runtime to implement this effect.

The exact historical fade speed requires an original-environment reference
or user assessment. The endpoint correction is independently reproducible.

## In-game verification status

The 01 edition execution copy was launched twice without changing its files:
once with the existing diagnostic GDI trace enabled, then normally. Both
created the game window and exited with `0xC0000005` during startup. No
successful in-game fade sequence was collected, and the cause of this startup
failure was not established in this investigation. No release or installed
game was modified.
The execution copy's QTIM32.dll SHA256 was
`785da89f24737fae362091ecd796a9b393218156c165e3b92e273855d95b9810`.

**ユーザー実機確認待ち** once a scoped experimental fix is prepared:
record the executable and proxy hashes, watch the startup logo/title fade,
then title-to-side-selection and battle transitions. Confirm gradual darkening
to complete black, gradual return to full brightness, correct orientation,
unchanged scene progression, and uninterrupted BGM/effects. This investigation
does not claim those acceptance checks have passed.

## Experiment H1: exact endpoints (2026-10-17)

`work/exe_patches/patch_fade_phase6.py` changes one byte on top of the
Phase 5 executable: the loop branch at `0x00409295` becomes `jle` instead of
`jl` (`0F 8C` -> `0F 8E`, same target). The loop then draws 51 frames:
fade-out **100, 98, ... 0 percent** and fade-in **0, 2, ... 100 percent**.
The counter is only the multiplier in `component * percent / 100`, so 100
cannot overflow a byte. Timing is unchanged. The script guards the original
bytes, refuses to overwrite its input, and refuses an already patched file.
Revert by writing `0F 8C` back or rebuilding from Phase 5.

- Phase 5 input SHA256 `607299a2cd7d5aeb1375cd838cd343cd81f9289311cea659d100c700d4035ec4`.
- Phase 6 output SHA256 `505f1e8ac4746c82045ff4901975476273915ad67477e790dc9c957543909238`.
  It differs from the input only at file offset `0x8696`.
- Not part of `build_all.py` or any release.

In-game observation on the 01 edition execution copy (current proxy,
`QTIM_GDI_TRACE=1`, 25 seconds without input, then the process was killed).
Both runs started normally; the earlier `0xC0000005` startup failure did
not recur.

| | Phase 5 | Phase 6 |
|---|---|---|
| Fades during startup (GetObjectA from the fade) | 2 (+0.25 s, +3.25 s) | 2 |
| StretchDIBits per fade, all returning 416 | 50 | **51** |
| Span of one fade (GetTickCount) | 0 ms | 0 ms |
| BANDAI logo background on desktop capture after fade-in | **249** | **255** |

The first fade is the BANDAI logo's fade-in. Because the original stops at
98 percent, the logo stays at 249/255 brightness for its whole display. This
is a second visible endpoint defect, now corrected. The second fade is the
logo's fade-out; the OP movie draws immediately afterwards, so its black
final frame cannot be seen at startup. The trace confirms that the 51st
(0 percent) frame is drawn. All 50 or 51 frames of each fade fall within one
GetTickCount tick, which confirms H2 (no pacing) in the game, not only offscreen.

User in-game check (2026-10-17, Phase 6 SHA256 `505f1e8a...`): passed.
The BANDAI logo background is pure white; title -> side selection,
save/load, and battle start/end go fully black during the transition with
no residual image; the next screen returns to full brightness with correct
orientation; BGM and effects continue without interruption; progression is
normal. Pacing (H2) is a separate next step.

## Experiment H2: fade pacing in the proxy (2026-10-17)

No executable change. The proxy's outermost gundam.exe `StretchDIBits` IAT
hook (`work/qtim_proxy/display_scale.c`) recognises a fade step by its return
address. Because exe Phase 3 (b) routes the call through its code cave, the
return address is `0x00438561` (`jmp 0x00409289`), not `0x00409289`; the
proxy accepts it only after verifying that `jmp` at load. Each step waits
until `first step + k * fade_ms / 50`, using QPC and a high-resolution
waitable timer. With scaling on, every step is also presented. The step count
per fade is read from the loop branch (`jl` = 50, `jle` = 51), so
back-to-back fades each get their own schedule. `[display] fade_ms` /
`QTIM_FADE_MS`: default 400 (provisional), `0` = original, max 2000. With
scaling off, only this one IAT entry is hooked.

The first build matched only `0x00409289` and never paced (span 0 ms); the
disassembly of the patched executable showed the cave return address.

Observation (01 edition execution copy, exe Phase 6, trace on):

| Setting | Steps / fade | Duration (QPC / GetTickCount) | Logo fade-out on desktop captures |
|---|---|---|---|
| scaling on, 400 | 51 | 400 ms / 391-406 ms | 198, 158, 86, 25, 0 at 100 ms spacing |
| scaling off, 400 | 51 | 400 ms / 391 ms | 229, 168, 40, 0 |
| scaling on, 0 | 51 | - / 0-15 ms | (original) |

Known risk (Sol review): while a fade runs, the game thread does not reach
`FUN_00421600`, so a BGM loop that ends during a fade restarts up to
`fade_ms` later (at most 50 ms with the final default). A second Sol review
found no blocking issue after the per-fade step count was added.

**Waiting on a user in-game check:** visible gradual fade-out and fade-in for
the logo, title -> side selection, save/load, and battle start/end; whether
400 ms feels right; music/effects (including a BGM loop boundary near a
transition), progression, and minimize/restore.

### Default changed to 50 ms (2026-10-17)

The user found 400 ms too long. Reference recordings were measured in a
browser by seeking YouTube videos in 1/30 s steps and averaging canvas
luminance:

- `tkEEngTbCts` is the Pippin version (a different port). Every screen
  change is a cut; no fade is visible.
- `bHIhEa1awXI` and `-GrLLvXqCkY` are the PC 01 edition (title "(c)BANDAI
  2001", 498x360). The only game fade found in each (load -> harbor) has 1
  intermediate frame out and 1-2 frames in, so about 33-67 ms per direction.
  They were probably recorded on a fast PC, so this is not the speed on
  hardware from the game's release.

At the user's request the default is now `fade_ms=50`. Measured on the
execution copy (proxy SHA256 `7bda4f92...`): `fade steps=51 ms=50` (one 67)
with scaling on, and `ms=50` with scaling off.

**Decision (2026-10-17):** the user checked `fade_ms=50` in game, said it is close to how the game felt when they played it originally, and adopted it as final.
Not yet part of `build_all.py` or a release.
