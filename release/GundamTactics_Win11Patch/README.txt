ガンダムタクティクス MOBILITY FLEET0079
Windows 11 互換パッチ v1.0.16
========================================

このパッチは、Windows 11 でガンダムタクティクスを遊べるようにするためのものです。
お手持ちのゲームをインストールしたフォルダに当てて使います。ゲーム本体のファイル
(gundam.exe やムービー)は含まれていません。


■ 直ること
- 起動時の「16ビットモードで実行してください」のエラー
  (画面の色数を変える必要も、互換モードの設定も不要になります)
- BGM・効果音が鳴らない
- BGM が曲の終わりで止まり、長いあいだ無音になる(ループしない)
- ゲーム画面を最小化して戻すと BGM が鳴らなくなる
- ムービーが真っ暗になる/飛ばされる
- フェードなどで画面が上下反転する
- ムサイのハーバー画面に縦線が出る(原作の画像データ由来)
- クリックが効かないことがある
- 長いフォルダ名や日本語のフォルダ名だと起動できない・NEW GAME で止まる


■ 用意するもの
- Windows 11
- インストール済みのガンダムタクティクス(CD からインストールしたもの)


■ パッチの当て方
1. ゲームを終了しておきます。
2. ダウンロードした ZIP ファイルを右クリックして「プロパティ」を開き、
   下のほうに「許可する」のチェックがあればチェックして「OK」を押します。
   (これをしないと、手順 4 で警告が出ることがあります)
3. ZIP を右クリックして「すべて展開」を選び、好きな場所に展開します。
   (ゲームのフォルダの中に展開してもかまいません)
4. 展開したフォルダの中の「install.bat」をダブルクリックします。
   - セキュリティの警告や「Windows によって PC が保護されました」が出たら、
     「実行」(または「詳細情報」→「実行」)を押してください。
5. ゲームのフォルダが自動で見つからないときは、フォルダを選ぶ画面が出ます。
   gundam.exe が入っているフォルダ(通常は C:\G-TACT)を選んでください。
   ゲームのフォルダを install.bat の上にドラッグ&ドロップしても指定できます。
6. 「よろしいですか? (Y/N)」と出たら Y を入力して Enter を押します。
7. 「完了しました」と表示されたら終わりです。何かキーを押して画面を閉じます。
8. ゲームのフォルダの gundam.exe をダブルクリックして遊んでください。

※ ゲームを C:\Program Files などに入れている場合は、途中で「このアプリが
  デバイスに変更を加えることを許可しますか?」と出ます。「はい」を選んでください。
※ 以前のバージョン(v1.0.x)のパッチを当てている場合も、そのまま同じ手順で
  上書きできます。先に元に戻す必要はありません。


■ パッチを外す(元に戻す)
1. ゲームを終了しておきます。
2. 「uninstall.bat」をダブルクリックし、同じように Y を入力します。
   ゲームはパッチを当てる前の状態に戻ります。


■ ゲームのフォルダについての注意
- ゲームはどのフォルダに置いてもかまいません(ドキュメントやデスクトップ、
  日本語の名前のフォルダでも動きます)。
- ただし、次のようなフォルダでは動かないため、パッチの適用が止まります。
  - とても長いパスのフォルダ(目安: フォルダのパスが半角 230 文字、
    全角 115 文字を超えるもの)
  - 韓国語や絵文字など、日本語の Windows で使えない文字を含む名前のフォルダ
  その場合は C:\G-TACT のような短いフォルダへゲームのフォルダごと移してから、
  もう一度 install.bat を実行してください。
- パッチを当てた後にゲームのフォルダを移動するときは、フォルダの中身を
  まるごと移してください(パッチが作った gundam.exe.orig・QTIM32R.DLL・
  CMGR32R.DLL も必要です)。
- パッチを当てたゲームは、BGM をループさせるためにゲームのフォルダの中へ
  「MidiLoop」フォルダを作ります(元の曲ファイルは変更しません)。
  パッチを外すと自動で削除されます。
- ゲームはショートカットからではなく、gundam.exe を直接ダブルクリックして
  起動するのが確実です。ショートカットを使う場合は「作業フォルダー」を
  ゲームのフォルダにしてください。


■ うまくいかないとき
- 「ゲームが起動しています」
    ゲームを終了してから、もう一度実行してください。
- 「QTIM32.DLL / CMGR32.DLL が元のファイルではありません」
    以前、パッチのファイルをゲームのフォルダへ直接コピーして上書きした
    可能性があります。ゲームを再インストールしてから、もう一度実行してください。
- 「gundam.exe が想定しているファイルと違います」
    別のバージョンのゲームや、別のパッチを当てた gundam.exe の可能性が
    あります。ゲームを再インストールしてから、もう一度実行してください。
- 途中で失敗しても、ゲームのファイルは実行前の状態に自動で戻ります。
- 解決しないときは、画面に表示された「詳細」のメッセージを添えてお知らせください。


■ 注意事項
正規に入手したゲームでのみお使いください。このパッチは無保証です。念のため、
ゲームのフォルダのバックアップを取ってから当てることをおすすめします。
ゲームと素材の権利は各権利者に帰属します。


------------------------------------------------------------------------
Technical notes (English)
------------------------------------------------------------------------

What is fixed
-------------
- Startup: bypasses the legacy 16-bit-color (16bpp) check.
- Audio: restores BGM playback from Sound\*.MID and waveOut SE playback,
  including the WAVEHDR lifetime and MIDI polling fixes.
- BGM loop: most Sound\M*GM.MID files end their music long before the
  End-of-Track event (M02GM.MID: 36 s of music, then 264 s of silence). The
  game restarts BGM only when MCI reports the song as stopped, and the
  Windows 11 sequencer plays that silent tail in full, so the music went
  quiet for minutes. When the game opens such a file, the QTIM32 proxy opens
  a copy in `<game folder>\MidiLoop\` (or `%TEMP%\GundamTacticsMidiLoop\`
  when the game folder is read-only) whose tracks end on the last real
  event; all other bytes are identical. The song then stops at the end of
  its music and the game's own loop restarts it. The Windows 11 sequencer
  also rejects the MCI resume the game sends after the window is restored
  from minimized, which left BGM silent; the proxy continues it with a play
  from the paused position instead. To turn both off, set
  `QTIM_MIDI_LOOP_FIX=0` or put `[audio]` / `midi_loop_fix=0` in
  `qtim_compat.ini`.
- Movies: the 32-bit QTIM32 proxy decodes SMC video and supplies raw 8-bit
  and big-endian twos audio. GetMoviePict-path audio follows movie time with
  a small lookahead. Controller clock/status and controller-frame drawing stay
  real-time driven for all SMC movies. 0x38/0x2F never start audio; the first
  0x36 starts real-time audio. While an emulated controller is active,
  GetMoviePict does not touch audio. Only the controller-less picture path
  uses requested movie time for audio streaming.
  The sortie SMC movies use the emulated controller path and can complete
  without the legacy controller. CMGR32 is a companion proxy for the
  secondary movie-time dispatcher. Calls containing an active or retired
  proxy controller handle are safely absorbed; an ordinary/reused movie
  handle is not a binding key, and unbound 0x36/0x06 selectors are forwarded
   to the original runtimes. Decoder-backed SMC 0x12 duration handling remains
   the v1.0.7 behavior. DisposeMovie now releases proxy state and passes the
   real Movie object to the original QuickTime runtime. Pending 0x2C/0x2A paths
   are mapped only after a matching 0x02 and output-slot check, preventing
   stale-handle remaps after a failed NewMovieFromFile.
- Display: Phase 3 part b fixes the Win11 top-down DIB height interpretation
  used by FUN_004090F0. Logos, title screens, PUSH ANY BUTTON, and the
  observed 12-second screen are upright in the real-screen A/B test.
- Harbor screen: Bmp\HERBOR\MUSA.DOC is 587 pixels wide (every other
  full-screen BMP is 576), and the game shrinks it with the DC's default
  BLACKONWHITE stretch mode, which ANDs 8bpp palette indices of the merged
  columns and draws 11 white/cyan/magenta vertical lines (also on the
  original OS). Phase 5 makes the BMP loader FUN_00407200 set COLORONCOLOR
  (resolved via GetModuleHandleA/GetProcAddress; SetStretchBltMode is not
  imported) before its StretchDIBits call. 1:1 blits are unaffected.
- Battle movies: the QTIM32 proxy performs a per-thread, 250-ms-throttled
  `PeekMessageA` with `PM_NOREMOVE` only on the UI thread that owns the saved
  controller HWND. It runs outside the state lock, preventing Windows from
  marking the game window unresponsive during long chained warship battles
  without consuming or reordering game input; an unknown/non-owner HWND is
  skipped.
- Clicks: the original game ignores a click whose message time equals the
  time of the message before it, so a click that lands in the same timer
  tick (about 15.6 ms on Windows 11) as a small mouse movement was dropped
  and had to be repeated. The QTIM32 proxy makes button/key message times
  strictly increasing (same-tick collisions are moved forward by 1 ms). The
  game's own deliberate guards (clicks ignored for 1 second after some
  screen changes, 5 seconds at the opening movie, actions that confirm on
  button release) are unchanged. To restore the original timing, set
  `QTIM_INPUT_FIX=0` or put `[input]` / `click_fix=0` in `qtim_compat.ini`.
- Install folder: the original executable joined the install folder and each
  asset name in an 80-byte buffer, so a folder longer than 54 bytes failed
  with a Jfont load error or froze at START NEW GAME. The patched executable
  opens every asset relative to the game folder (`.\Bmp\...`), so the game
  can live in ordinary folders such as Documents, the Desktop, or folders
  with Japanese names.

Requirements and path restriction
----------------------------------
- Windows 11, a 32-bit game installation, and PowerShell 5.1.
- The game folder may be long or contain Japanese characters. The full path
  of every game asset must fit in 256 ANSI/MBCS bytes (Windows MAX_PATH):
  with the longest current asset (24 bytes), the folder path itself may be
  up to 230 bytes (a Japanese character counts as 2 bytes). Every character
  of the folder path must exist in the system ANSI code page (on Japanese
  Windows: Shift_JIS); the original QuickTime cannot open movies from other
  folders, e.g. ones with Korean or emoji names. The installer
  checks only game-opened assets (patch sidecars, trace files, and `.url`
  shortcuts are excluded) and refuses an overlong path before patching.
- Start the game with the game folder as its working folder (double-click
  gundam.exe, or keep a shortcut's "Start in" set to the game folder). This
  was already required by the original game.
- Close the game before applying or reverting. The patch does not require
  the legacy 16-bit-color setting. `-RegisterAppCompat` and `-RunAsAdmin`
  are available when a particular host setup still needs those compatibility
  options. When either option is used, the previous AppCompat entry (including
  its absence) is saved as `gundam_win11patch.state.json` beside the game.

Apply
-----
Double-click `install.bat` (see the Japanese guide above), or run the engine
directly from this package directory:

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\patch_files\apply.ps1 -InstallPath 'C:\G-TACT'

`install.bat` finds the game folder automatically (the package folder or its
parent, then C:\G-TACT, then a folder dialog), asks for confirmation, and
requests administrator rights only when the folder is not writable. The
engine strips stray quotes and trailing backslashes from -InstallPath. The
package's own files are kept in `patch_files\`, so extracting the ZIP inside
the game folder never overwrites the game's QTIM32.DLL/CMGR32.DLL. The script verifies the stock hashes, checks every
  modified byte and code cave, stages replacements atomically, and keeps the
  original executable as a verified temporary `gundam.exe.orig`. If an
  unrelated AppCompat value already exists without this package's sidecar, the
  script refuses to overwrite it. A failed transaction restores all files,
  registry state, and the sidecar.

Updating from v1.0.2-v1.0.15: run the new apply.ps1 on the patched folder.
It rebuilds gundam.exe from the verified `gundam.exe.orig`; no revert is
needed first. A QTIM32.DLL proxy from v1.0.10-v1.0.14 is replaced in place
(QTIM32R.DLL must still be the verified original runtime). To move the game
to a longer folder, move the whole folder
(including `gundam.exe.orig`, QTIM32R.DLL, and CMGR32R.DLL) after applying.

Revert
------
With the game closed, double-click `uninstall.bat`, or run:

  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\patch_files\revert.ps1 -InstallPath 'C:\G-TACT'

Revert restores the original executable, QTIM32.DLL, and CMGR32.DLL, verifies
all three hashes, removes the temporary QTIM32R.DLL and CMGR32R.DLL runtime
backups, removes the hash-verified
`gundam.exe.orig` and the proxy-generated `MidiLoop\*.MID` copies, and restores the AppCompat state recorded in
`gundam_win11patch.state.json`. The sidecar is removed after a successful
restore. Without a sidecar, only the package's own AppCompat values are
removed; unrelated values are left untouched and cause a failure.

Known limitations
-----------------
- The three Cinepak (`cvid`) movies are not natively decoded. They remain on
  the original QuickTime controller path; QuickTime 2.x is not patched.
- Proxy cache/decoder misses return a black DIB so game progress is preferred.
- `QTIM_COMPAT_TRACE`, `QTIM_GDI_TRACE`, and related debug/trace behavior is
  OFF by default. Enable compatibility tracing with the
  `QTIM_COMPAT_TRACE=1` environment variable, or place `qtim_compat.ini` in
  the game folder with `[trace]` and `enabled=1`. The proxy reads the INI from
  the directory containing its DLL (normally the game folder), falling back
  to the absolute current directory; the environment variable takes
  precedence. Controller result lines are throttled to frame changes or one
  line per 250 ms.
- For a reproduced silent-BGM case, enable `QTIM_COMPAT_TRACE=1` and
  `QTIM_MCI_TRACE=1`. Add `QTIM_COMPAT_SELFTEST=1` to verify the hook path
  without needing to reach the game's BGM screen (or set `[trace] enabled=1`
  and `mci=1` in `qtim_compat.ini`) and send `qtim_compat_trace.log`. MCI
  trace lines record
  the command, result, mode changes, game BGM state, MIDI device, and
  suppressed-poll count without changing MCI arguments or return values.
  Repeated STATUS/MODE rows are suppressed only for a complete matching key
  `(device, command, result, mode, state, midi)` and emit a five-second
  heartbeat. With all three environment variables, the self-test emits an
  `mci` row for device 0 / `MCI_STATUS` / `MCI_STATUS_MODE`, checks the original
  `MCIERR_INVALID_DEVICE_ID` (257) result, and logs
  `mci_hook_selftest=PASS`. This self-test row verifies hook forwarding and is
  not the game's actual BGM MCI sequence, which remains a user-side capture
  after advancing past the title screen. The MCI hook is installed whenever
  the BGM loop fix or MCI tracing is enabled; the trace rows show the
  game's own calls, and `midi_loop` / `midi_resume` rows show what the
  proxy changed.
- Part a of the Phase 3 experiment is not included: it was rejected because
  real-screen A/B testing reversed the PUSH layer. The release contains part
  b only.

Reference SHA256
----------------
Stock gundam.exe:
  38bde2e4513c665d1425fd00203d0000001c5b81bc37899507b6ef7129f238d3
Patched gundam.exe (Phase 1 + 2 + 3b + 4 + 5):
  607299a2cd7d5aeb1375cd838cd343cd81f9289311cea659d100c700d4035ec4
Previous patched gundam.exe (accepted for upgrade):
  v1.0.13-v1.0.15: c693d60b73cbba731dbacbea255e845e97a0dae95cc80812316a5795e4f15942
  v1.0.2-v1.0.12:  82c92402ac9c9282992d3c343dbb7c62463a3a5e58569ff845670bf166b867d4
Stock QTIM32.DLL:
  dbbe7e208955c0173d2a41a8873d1ccdacdca96e42948f861768e1dde3afc77f
Release QTIM32.dll proxy:
  ce481563d035c5ba09883b7e00967ba73557969366657f31a00b7afa0c710af2
Stock CMGR32.DLL:
  9fc00aece0c9db38b7e7001d261a060567eb035a343f4d833e58fd163e80f9e4
Release CMGR32.dll companion proxy:
  db6456350aefe2e35114b183458457d88c6e7e8974f29863bd5b83b2be3a964d

Distribution contents
---------------------
install.bat, uninstall.bat, README.txt, CHANGELOG.txt, checksums.txt, and
patch_files\ (install.ps1, apply.ps1, revert.ps1, QTIM32.dll, CMGR32.dll). Original game files, patched executables, and MOV assets
are intentionally excluded.

Disclaimer
----------
Use only with a copy of the game you are entitled to use. This compatibility
patch is provided without warranty. Keep a backup of the installation and
the original media; the game and its assets remain the property of their
respective copyright holders.
