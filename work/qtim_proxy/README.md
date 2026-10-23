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
  the recorded port (v1.2.1: after leaving `g_state_lock`, from a copy of the
  DIB, and only when the frame, port or HWND/HDC changed, the game or the
  original runtime drew into the scaled memory DC, or 100 ms passed; with
  scaling off it draws on every call);
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
set compared all BMP bytes against a read-only historical `MovieCache` (frames
rendered by the original QuickTime, kept as a private archive outside this
repository) and passed 216/216 frames across five SMC movies.

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
QTIM32 also logs `modal_pause_selftest=PASS`: a 150 ms move/size loop must
pause the movie's waveOut stream and push its controller and the shared
clock back by the loop's length.

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
thread, after leaving `g_state_lock`. When there is no controller (v1.2.1,
`GetMoviePict` movies) the current thread's "Gundam" window, found with
`EnumThreadWindows`, is used instead; with no such window there is no
probe. It
does not remove or dispatch messages, and is logged once as `message_pump`
when compatibility tracing is enabled.

Window scaling (`display_scale.c`, v1.1.0) replaces every DC request on the
main "Gundam" window with one 576x416 32bpp memory DC: the main executable's
`GetDC`/`ReleaseDC` and the original runtime's (`QTIM32R.DLL`)
`GetDC`/`GetDCEx`/`ReleaseDC` IAT entries, plus the proxy's own SMC frame
drawing (`gt_scale_get_dc`). Each acquisition does `SaveDC`, each release
`RestoreDC`. Draw calls into that DC (`BitBlt`, `StretchBlt`,
`StretchDIBits`, `PatBlt`, `SetPixel`; the runtime's `BitBlt`, `PatBlt`,
`SetDIBitsToDevice`, `FillRect`) mark it dirty, and it is stretched onto
the window on DC release, on the game's `PeekMessageA`/`GetMessageA`, and
from a helper thread after 12 ms without drawing. When the window is not a
whole multiple of 576x416 the stretch uses `HALFTONE` (5-7 ms); from v1.2.1
the game thread then only copies the frame and the helper thread stretches
it (all `HALFTONE` presents, `WM_PAINT` included, go through the helper in
order; helper stretches are serialized and a helper retires when a newer
one is created). A window subclass maps
mouse `lParam` back to 576x416, repaints from the memory DC, hides resizes
from the game (it only sees minimize and the restore after it), and toggles
borderless fullscreen on Alt+Enter. From the movie paths above, from
`gundam.exe`'s `GetTickCount` import (its only clock read, which its timed
waits spin on: the 3 s Bandai logo, the pauses between battle movies) and
its `ReadFile` import (scene loads), at most once per tick value, and from
the fade-pacing wait, queued `WM_NCLBUTTONDOWN`/`DBLCLK` on the caption,
border or maximize button are dispatched so the window can be moved and
resized there. Input is read in order, so while a mouse button message is
queued the mouse moves ahead of it (client or non-client) are dispatched
first, as the game's own loop would (its `WM_MOUSEMOVE` only records the
position); sent messages are processed as by any `PeekMessage` (input can
wait behind the activation a click starts). Any other input at the head
stops the scan, with one exception for a client button release while the
user is pressing the caption or border at that moment (logical left button
down, cursor on the window, `DefWindowProc` hit test on the frame). The game
calls `SetCapture` on a button press and acts on some presses at once (a
click that skips a movie starts the next movie, which reads no input), so
the release stays queued and the window keeps the capture: every later
press, the caption included, arrives as a client click behind it. Such a
release is taken out of the queue and held (at most 16), the capture is
released, and the game's own next `PeekMessage`/`GetMessage` returns it
before anything newer, with `GetMessageTime` reporting the time the
input-time fix below gives it (trace `input_held` / `input_released`).
Presses are never held: their release would then be scanned without the
capture the press sets and arrive as `WM_NCLBUTTONUP`. The music
switch between scenes is covered too: Windows 11's `mciseq.dll` waits for
its stream thread with `Sleep(4)` polls during `MCI_OPEN`/`MCI_CLOSE`
(0.7-0.8 s each), and its `Sleep` import (hooked at the game's first
`MCI_OPEN`, see the MIDI restart below) services presses before each poll on
the game's thread (trace `frame_input msg=... wait_ms=...`).
During the runtime's initialisation (0x39) its `midiOutOpen`,
`midiOutClose` and `waveOutGetNumDevs` (about 100 ms each for the MIDI
mapper, sometimes seconds) run on a short-lived worker thread with the same
arguments and result, while the game's thread waits with
`MsgWaitForMultipleObjects` and services the same presses (trace
`device_call kind=... ms=... wakes=...`).
A caption press found there (the game is busy: a movie, a wait, a load)
does not enter USER32's modal move loop, which would block the game until
the button is released and stop every movie the game composites itself
(battle movies, map sprites). The proxy captures the mouse and moves the
window to follow the cursor (`GetCursorPos`, `SetWindowPos`) on every later
service call and every `PeekMessage`/`GetMessage` of the game, while the game
keeps running; the drag ends when the logical left button is up
(`GetAsyncKeyState`). Until the drag's release has gone by, the window's
mouse messages are dropped from the queue and swallowed by the subclass, so
the game never sees the drag as input; a new press after the drag also
ends that and reaches the game. Held releases (above) wait for the
drag, since their `ReleaseCapture` would end it; keyboard input is not held
back meanwhile (trace `drag begin wait_ms=...` / `drag end ms=... moves=...
why=...` / `drag input_done`). When the press cannot be reached because
older input the busy game has not read is in front of it (a click that
skipped the previous movie, a key) and the user is pressing the caption
(borders and the maximize button wait for the game), a detached drag starts
(`drag begin detached=1`). Each service call samples, once per tick, where
the left button was first seen down, so the drag starts from that point and
window position even if the cursor has left the caption since. No capture is
taken (under a capture the queued press would be scanned as a client click),
the older input stays queued and reaches the game in order (held releases
first), and swallowing starts at the drag's own press, recognised by its
screen point (`MSG.pt`) inside the caption band of the window as it was when
the drag started. The game's own `SetCapture`/`ReleaseCapture` for that
older click do not cancel a detached drag. The game can take seconds to
read that older input (battle movies); meanwhile only a drag in progress
stops the scan, so a new caption press starts another drag. Each detached
drag still waiting for its own press keeps its caption band and the span
of message times its press can have (from when the button was last seen up
to when this press was first seen down); a press matching one in both is
swallowed and the next waiting drag's input passes until its own press
comes. Up to 32 drags can wait; beyond that no new detached drag starts. A
waiting drag whose press arrives as a double-click passes it to the window
and stops waiting; the others keep waiting. The press sample is used by one drag only, is
dropped when the window has moved since it was taken, and is renewed after
every drag and by the game's own message polls. A captured drag gives the
capture back as soon as the button is up. A caption or border
`WM_NCLBUTTONDOWN` that reaches the window after the button is already up
(a press the busy game read late) is dropped, so USER32's move/size loop
does not start late from a stale point (trace `frame_press_stale`). The
game hides the cursor (`ShowCursor(FALSE)`) for the Bandai logo, and that
count applies over the caption too; `gundam.exe`'s `ShowCursor` is hooked
to track the game's count, and while it is below zero the proxy raises it
to zero whenever the cursor is on the main window's non-client area
(checked once per tick), so the caption can be seen and grabbed; over the
client area the cursor stays hidden, and the game's own calls always see
its own count (re-checked inside the hook, so the cursor over the frame does
not blink when the game hides it). While the game is busy no WM_SETCURSOR
arrives, so the same per-tick check also sets the cursor for the frame part
under it as DefWindowProc would (arrow on the title bar and menu bar, sizing
arrows on the border): the busy cursor of the launch no longer stays over
the menu bar during QuickTime's initialisation. Border presses,
caption double-clicks and presses the game's own loop dispatches still use
USER32's modal loop.
The game's thread is blocked in USER32's modal move/size loop while the
button is held. Between `WM_ENTERSIZEMOVE` and `WM_EXITSIZEMOVE` a 10 ms
timer on the main window keeps the movies the proxy can drive playing: the
emulated controllers that draw onto the main window, whose audio runs in
real time (started before the 0x36 handler services the queue) and that the
game polled (0x36/0x06) in the last 300 ms are drawn from their running
clock, with their waveOut stream left running, and the stock controller the
game last idled through CMGR (cvid: the opening, the side movies), as
reported by the CMGR proxy's `GtLastStockIdle` export and not disposed
(0x37, a 16-entry history) since that idle, is idled
through the game's own `MCDoAction(controller, mcActionIdle, 0)` thunk at
`0x430974` (bytes verified). Every other movie is paused: its waveOut
stream stops and its controller's start time (and the shared clock CMGR
reads) is pushed back by the loop's length, so it resumes at the frame where
it stopped, in step with its sound. That covers the battle sprites, which
play in hidden windows and are composited by the game from GetMoviePict
frames (trace `modal_loop enter ... playing=N stock=...` and
`exit ... ticks=N`). Controller-less GetMoviePict movies take their time
from the game and wait with it.
Menu bar: while scaling is active the window gets a menu bar (ゲーム: exit,
which posts `WM_CLOSE` so the game's own confirmation runs; 表示: the client
area at 1-4 times 576x416, keeping the window's top-left corner inside the
work area, sizes that do not fit greyed, fullscreen, the stretch filter, and
hiding the bar, which keeps the client area's size; ヘルプ: バージョン情報...,
an about window with the version (`GT_PATCH_VERSION` at build time) and the
guide, releases and source links, each in a read-only field with an Open
button; it runs on its
own thread and message loop and has no owner, so the game keeps running and
its window procedure is never re-entered (opening it again brings it to the
front); ツール:
a screenshot and opening the screenshot folder, the save folder (`DataDir`
of `Gundam.ini`, read as the game reads it), the game folder and the INI).
The tools are in `menu_tools.c`: the screenshot copies the shadow (the
576x416 frame on screen, movie frames included) under the present lock and
a worker thread saves it as PNG through GDI+ (`Pictures\GundamTactics\GT_
<date>_<time>.png`, BMP if GDI+ fails); folders, the INI (in Notepad) and
URLs are opened with `ShellExecuteW` on a worker thread, so the game's
thread is never pumped or stalled by the shell. shell32, ole32, gdiplus and
advapi32 are loaded only there, so the proxy's imports are unchanged. A
missing INI is first created as a commented template (UTF-8 without BOM,
every setting commented out); when the process is virtualized, a
zero-byte `WriteFile` makes Windows copy a protected INI into the
VirtualStore (opening for writing alone does not), and Notepad (not
virtualized) is given that copy. Screenshot names are reserved with
`CREATE_NEW`, so two shots within a second get `_2`.
Workers report back with a posted `WM_APP+0x47`, shown in the title bar
(the window is ANSI, so the text stays within CP932). The window menu gets メニューバーを表示 to
bring a hidden bar back. Choices are written to the INI's `[display]`
(`scale=1..4` from the size menu, `filter`, `menu=0|1`) with
`WritePrivateProfileStringA`; the game has no manifest, so under Program
Files the write goes to the user's VirtualStore and is read back from there.
A failed write keeps the choice for the run and says so in the title bar for
2.5 s (trace `menu saved` / `menu save_failed`). `scale=1` is the original
size with scaling on (resizable, menu bar); `off` (or `0`) is still off. The game has no menu and handles no
`WM_COMMAND`, so the commands (`0x4700`-`0x47FF`) are taken by the subclass;
it swallows `WM_SYSCHAR`, so the labels have no mnemonics, and
`SC_KEYMENU` from Alt alone or F10 is dropped so that the arrow keys stay
with the game. Every `AdjustWindowRectEx` passes the menu, the bar is taken
off in fullscreen (and put back before the saved placement), and
`[display] menu=0` leaves it out. The menu loop blocks the game's thread
like the move/size loop and is reported the same way
(`WM_ENTERMENULOOP`/`WM_EXITMENULOOP`, same timer and movie handling). A
menu-bar press is dispatched by the same service calls as a border press,
so the menu opens during movies, waits and loads too; there the exit item
is greyed (its `WM_CLOSE` would only arrive after the movie). While a
client release the busy game has not read is queued in front and the user
is pressing the menu bar at that moment, the release is held for the game as
for a caption press, so the menu-bar press behind it is scanned without the
game's capture and reaches the menu. A menu-bar press scanned while the game
still holds the capture of its previous click arrives as a client
`WM_LBUTTONDOWN` above the screen; the subclass recognises it by its
`GetMessagePos` point (hit test `HTMENU`), keeps it from the game, releases
the capture and opens the menu there (trace `menu_press ... captured=1`). A
menu-bar press the game reads with the button already up and more than
500 ms late is dropped, captured or not (trace `menu_press_stale`); others
are logged as `menu_press age=... service=...`. Measured: the opening (stock
cvid) keeps playing while the menu is open; battle cut-ins (`BOMB.mov` and
the like, composited by the game) pause with their sound and resume in step,
as for a border drag.
Window position and size: the INI's `[window]` (`left`, `top` of the window
in screen coordinates, `client_width`, `client_height`, `maximized`) is
written with `WritePrivateProfileSectionA` whenever it changes (end of a
move/size loop or of the proxy's own caption drag, maximize and the restore
from it, the size menu, leaving fullscreen), never at exit: the game ends
the process inside its `WM_DESTROY`. The values are the window's normal
(not maximized, minimized or fullscreen) placement; the client size is kept
rather than the window's, so the bar or the frame can change without
changing the game screen. At adoption it is used instead of the scale
setting: the client size is clamped to 576x416 .. the monitor's work area,
and when the caption band (`SM_CYCAPTION` high) does not cover at least
64 px of any monitor's work area the window is moved into the nearest one;
`maximized=1` maximizes it after placing (trace `window saved/restored/
reset`). 表示 has 記憶する (`[display] remember_window=0|1`) and
最初の状態に戻す (deletes `[window]` and centres the window at the scale
setting). `QTIM_DISPLAY_SCALE` turns the memory off.
`[game] multi_instance=1` is read by the executable itself, not by the
proxy: exe Phase 7's start-up check lets a second copy start only with it
(the proxy is not loaded yet at that point). It is not in the menu.
Message boxes: MessageBox centres its window on the monitor, not on its
owner, so the game's boxes (the WM_CLOSE exit confirmation, the 0x7E
notice, the debug boxes) only looked centred while the 576x416 window sat in
the middle of the screen. `gundam.exe`'s `MessageBoxA` is hooked: from the
window's thread, with no owner or the main window as owner, a thread CBT
hook for that call moves the box at `HCBT_ACTIVATE` to the centre of the
window, kept in its work area (trace `msgbox centred`).
`[display] scale=off` (or `QTIM_DISPLAY_SCALE=off`) installs nothing except
the fade pacing hook below (and no menu bar).

Fade pacing (experimental): the game's palette fade (`FUN_004090F0`) draws
its 50 steps (51 with exe Phase 6) with no wait, so a fade finished in a few
ms and, with scaling on, was mostly merged into one present. The executable's
`StretchDIBits` hook recognises a fade step by its return address:
`0x00409289`, or `0x00438561` with exe Phase 3 (b), whose code cave makes the
call and then jumps back to `0x00409289`. The cave is accepted only after the
`jmp` there is verified when the proxy loads. It spaces the steps so that 50 of them take
`[display] fade_ms` (`QTIM_FADE_MS`, default 50, `0` = original, max 2000)
and presents each step when scaling is on. The schedule is anchored at the
first step (high-resolution waitable timer, then a short yield), and it restarts
after a pause longer than 100 ms plus one step (trace `display_scale fade
steps=N ms=M`, logged when the next fade starts). With scaling off, only
that one IAT entry is hooked.
The window is adopted on the first QuickTime selector (0x39, the runtime's
initialisation), before that call is forwarded: the runtime's initialisation
opens and closes the MIDI mapper (about 230 ms here, occasionally seconds),
and the window used to stay at 576x416 until it returned (trace
`display_scale adopted_early`).
`work/tools/qtdraw_probe` logs the original runtime's drawing calls.

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

Startup timing (off by default): `[diag] startup=1` (or
`QTIM_DIAG_STARTUP=1`) appends to `qtim_startup.log` next to the INI the
first-call time of the first 12 selectors (`prev_to_this_ms` after 0x39 is
the whole runtime initialisation) and the duration and caller of the
runtime's `midiOut*`/`waveOut*` device calls, `CreateDCA`/`CreateICA`,
`GetProfileStringA` and `LoadLibraryA` (all calls during startup, then only
those of 100 ms or more; at most 400 lines per launch). The hooks only time
and forward.

For a reproduced silent-BGM case, enable the normal trace and the opt-in MCI
trace together:

```powershell
$env:QTIM_COMPAT_TRACE = "1"
$env:QTIM_COMPAT_SELFTEST = "1"
$env:QTIM_MCI_TRACE = "1"
python .\work\tools\verify_launch.py --game-dir .\run\GT01 --wait-seconds 15 --capture window --result work\analysis\v1011_launch_verify_window.json
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
python .\work\qtim_proxy\deploy.py --game-dir .\run\GT01
python .\work\tools\verify_launch.py --game-dir .\run\GT01 --wait-seconds 8
```

BGM loop restart (experimental): on Windows 11, a sequencer that has played
and is rewound with `MCI_SEEK_TO_START` skips its first event group's
note-ons (M07GM loses 8 notes, the first 200 ms). The game rewinds that way
on every loop (`FUN_00421600`). For sequencer devices the game opened with
a type name and a file name only, the proxy's `mciSendCommandA` hook replaces
that rewind (and the rewind in its `MCI_RESUME` fallback) with a close and a
reopen of the same file. If the device ID changes, the game's ID is translated
until the game closes it. `mciseq.dll` waits for its stream thread with
`Sleep(4)` polls, which made a reopen take 0.7 s in the game. The proxy keeps
`mciseq.dll` loaded and hooks its `Sleep` import, and only while it is
reopening does `Sleep(4)` become `Sleep(0)`; the reopen then takes about 1 ms.
Without that hook it does not reopen. `[audio] midi_restart_reopen=0`
(`QTIM_MIDI_RESTART_REOPEN=0`) turns it off; it also needs `midi_loop_fix`.
Trace: `midi_restart dev=... real=old->new ... total_ms=...`. Details:
`work/tools/MIDI_LOOP_INVESTIGATION.md`.

Deployment backs up both stock DLLs as `QTIM32R.DLL` and `CMGR32R.DLL` and
restores both with `deploy.py --undeploy`.

The release `apply.ps1`/`revert.ps1` perform the same DLL backup and atomic
replacement checks for a user-selected installation directory.
