# QTIM32 compatibility proxy (v1.0.12)

`QTIM32.dll` is a 32-bit MinGW proxy. It forwards the original QuickTime
runtime through `QTIM32R.dll` and handles the movie path used by the game.
`CMGR32.dll` is the companion proxy for the second dispatcher and forwards
the real component manager through `CMGR32R.dll`.

- selector `0x2C` records the MOV path;
- selector `0x2A` remains a pass-through because it returns an OSErr, not a
  movie pointer; its output slot is recorded pre-call, and the pending path
  from `0x2C` is associated with the concrete handle from `0x31`/`0x38`
  `arg0` only after a matching `0x02` and slot value;
- selector `0x14` decodes the requested SMC frame using MOV `timescale` and
  video `stts` timing;
- selector `0x2E` returns a movable 8-bpp bottom-up DIB with the QuickTime
  palette;
- selector `0x08` consumes the proxy picture handle;
- controller clock/status selectors are handled only when an argument is the
  proxy's fake controller handle. A normal movie handle is never a controller
  binding key: `0x36` and `0x06` are forwarded to the original runtime when no
  fake controller is present. Decoder-backed SMC `0x12` duration handling is
  retained for compatibility with v1.0.7;
- `0x38` and `0x2F` never start movie audio. The first controller idle
  (`0x36`) starts raw 8-bit or big-endian `twos` audio through `waveOut` in
  real time;
- GetMoviePict-route audio is streamed from the requested movie clock only
  when no emulated controller is active, matching v1.0.3. Each forward `0x14`
  call queues only the newly reached PCM plus a 100 ms lookahead, equal
  timestamps queue nothing, and rewind/`0x31` resets the queue. A controller
  movie's real-time waveOut stream is not reset from inside `0x14`. `0x31`
  rewinds the active audio position without changing the audio mode.
  `audio_mode(handle, realtime|picture, reason)` and
  `audio_stream(time, bytes)` are traceable.

For SMC movies that use the controller path (for example
`Movie\\CRUISE\\PEGS.MOV`), the proxy additionally handles:

- primary selector `0x38`: returns a fake controller in EAX, which the game
  then stores through its normal output pointer; it does not start audio;
- selector `0x32`: records the controller port Rect and HWND/HDC;
- selector `0x36`: on the first call starts real-time audio unless the movie
  has already switched to picture-time audio, advances a `GetTickCount`-based
  movie clock, decodes the SMC frame, and draws it with `StretchDIBits` into
  the recorded port;
- selector `0x2F`: acknowledges movie activation without starting audio;
- selector `0x12`: returns the decoder duration, as in v1.0.7;
- selector `0x06`: returns the shared controller time only when an argument is
  a fake controller handle;
- selector `0x37`: releases the fake controller, holds the last frame until
  disposal, and stops that movie's waveOut audio.
- selector `0x07`: releases proxy state and passes a real SMC Movie handle
  through to QuickTime; calls containing a fake controller remain absorbed.

The QTIM32 and CMGR32 proxies share only the emulated controller clock and a
generation-numbered controller/movie lifetime journal through the PID-scoped
mapping `Local\\GundamTacticsQTIMController_<PID>`. The mapping contains an
owner PID and seqlock sequence, so CMGR consumes consistent snapshots and
imports create/dispose/bind/unbind events even if it did not observe the
active controller call. cvid movies do not enter this path and continue
through the original QuickTime controller.
Controller diagnostics are written as `controller_emulated`,
`controller_frame`, `controller_done` in `qtim_compat_trace.log`; secondary
dispatcher calls are additionally recorded in `cmgr_compat_trace.log`.
Any selector whose arguments contain an active or retired fake controller is
handled locally. A movie pointer, including one numerically reused by
QuickTime, is never sufficient to trigger the guard. Unknown fake-controller
selectors are recorded as `controller_unhandled` and return zero; retired
controller handles remain tombstones, while their movie binding is removed on
movie remap, `DisposeMovie`, and `CloseMovieFile`. Fake controller values have
low 16 bits `0xFFFF`, so the original Component Manager index/count check
rejects them as a second safety net.

The shared decoder is in `movdec.c/.h`. The standalone validation CLI is built
with:

```powershell
python .\work\tools\build_movdec.py
.\work\tools\movdec.exe <movie.mov> <output-directory>
```

The CLI writes one BMP per frame and a `manifest.json`. The Phase 2 validation
set compares all BMP bytes against the read-only historical `MovieCache` and
currently passes 216/216 frames across five SMC movies.

The three `cvid` movies remain on the original QuickTime controller path;
native Cinepak decoding is not included in this phase. If a proxy-side cache
lookup is ever reached and misses, the proxy logs the miss and returns a black
8-bpp DIB so game progress is preferred over a QuickTime 2.x revival.

Tracing is off by default. Enable it only for diagnosis with the
`QTIM_COMPAT_TRACE=1` environment variable, or with `qtim_compat.ini` in the
game folder containing `[trace]` and `enabled=1`. The proxy resolves the INI
to the directory containing its own DLL (normally the game folder), with the
absolute current directory as a fallback; the environment variable takes
precedence when it is set.

For the controller/movie lifetime regression test, set
`QTIM_COMPAT_SELFTEST=1` before starting the game. Both proxies then log
`controller_reuse_selftest=PASS`; the test covers a failed `0x2A` without
`0x02`, reuse of movie handle `03C84C08` with `0x02`, normal `0x07` pass-through,
fake-controller `0x07` absorption, and forwarding of a live `0x36` call.

For Phase 3 display diagnosis, set QTIM_GDI_TRACE=1 before starting the
game. The proxy then observes the main executable's imported GDI calls
(including StretchDIBits, StretchBlt, BitBlt, CreateDIBSection, SelectObject,
and GetObjectA) and writes qtim_gdi_trace.log in the game directory. The
GetObjectA record includes DIBSECTION dsBmih.biHeight and dsBm.bmHeight.
This hook is debug-only and is disabled by default; it does not alter
arguments or return values.

During long chained battle movies the original game loop does not dispatch
the Windows message queue. The proxy therefore calls
`PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE)` at most once per 250 ms per
thread from the fake-controller `0x36` and `GetMoviePict(0x14)` paths. The
call is made only when the saved controller HWND belongs to the current UI
thread, after leaving `g_state_lock`; an unknown HWND causes no probe. It
does not remove or dispatch messages, and is logged once as `message_pump`
when compatibility tracing is enabled.

The game accepts input only when `GetMessageTime()` is strictly newer than
the last stamp it stored, and WM_MOUSEMOVE also updates that stamp, so a
click in the same GetTickCount tick as a mouse move was dropped. The proxy
hooks the main executable's `user32.dll` `PeekMessageA`, `GetMessageA` and
`GetMessageTime` IAT entries (all or none). For each retrieved message it
reports the real time, except that key/button messages (not WM_MOUSEMOVE)
whose time is not newer than the last reported time (within 100 ms) are
reported as last + 1 ms. `GetMessageTime` returns that value while the
thread's real message time still matches the last retrieved message. On by
default (`input_fix=on hooks=3` in the trace); `QTIM_INPUT_FIX=0` or
`[input] click_fix=0` disables it. `work/tools/click_gate_test` reproduces
the game's gate (`--synth N`, optionally `--proxy QTIM32.DLL` run from the
game folder).

For a reproduced silent-BGM case, enable the normal trace and the opt-in MCI
trace together:

```powershell
$env:QTIM_COMPAT_TRACE = "1"
$env:QTIM_COMPAT_SELFTEST = "1"
$env:QTIM_MCI_TRACE = "1"
python .\work\tools\verify_launch.py --game-dir .\run\GT --wait-seconds 15 --capture window --result work\analysis\v1011_launch_verify_window.json
```

The equivalent INI configuration is:

```ini
[trace]
enabled=1
mci=1
```

When both switches are active, the proxy temporarily hooks the main
executable's `winmm.dll!mciSendCommandA` IAT entry. The original function is
called unchanged, and `qtim_compat_trace.log` receives lines in the form
`mci\ttick=...\tdev=...\tcmd=...\tflags=...\tret=...\tmode=...\tstate=...\tmidi=...\tsuppressed=...`.
Repeated `MCI_STATUS/MCI_STATUS_MODE` rows are suppressed only when
device/command/result/mode/game-state/MIDI-device all match; a heartbeat is
written at least every five seconds with the suppressed count. Other MCI
commands and mode changes are kept.
The hook is disabled by default and restored on DLL detach.

With all three environment variables above, the first QuickTime dispatch also
runs `mci_hook_selftest`. It directly calls the hook with device 0,
`MCI_STATUS` (`0x814`), `MCI_STATUS_ITEM`, and `dwItem=MCI_STATUS_MODE`; the
expected original result is `MCIERR_INVALID_DEVICE_ID` (257). A passing run
logs `mci_hook_selftest=PASS` and one `mci\t...` row. That row verifies hook
forwarding and logging, but is not evidence that the game's BGM path reached
MCI; actual BGM `MCI_OPEN`/`PLAY`/`STATUS` collection remains user-side when
the game is advanced past the title screen.

For the A/B display candidate, set QTIM_GDI_FIX=bmi_negative or flip_rows.
The optional QTIM_GDI_FIX_CALLERS value selects return VAs, for example
00409289 or 00407392,00409289. If it is omitted, both known callers are
selected. Unknown or malformed values are ignored; the selected caller mask
is recorded in qtim_gdi_trace.log.

The accepted Phase 3 executable fix is part b only. `patch_display_phase3.py`
defaults to `--part b`; part a is retained only for an explicit diagnostic
`--part a` invocation and is not accepted for release because it reverses the
PUSH layer in real-screen A/B testing. `build_all.py` applies part b by
default; `--without-display-phase3` is diagnostic-only.

Build and deploy:

```powershell
python .\work\qtim_proxy\build.py
python .\work\qtim_proxy\deploy.py --game-dir .\run\GT
python .\work\tools\verify_launch.py --game-dir .\run\GT --wait-seconds 8
```

Deployment backs up both stock DLLs as `QTIM32R.DLL` and `CMGR32R.DLL` and
restores both with `deploy.py --undeploy`.

The release `apply.ps1`/`revert.ps1` perform the same DLL backup and atomic
replacement checks for a user-selected installation directory.
