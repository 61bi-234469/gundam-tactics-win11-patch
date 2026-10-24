# 長時間プレイの音声消失・異常終了の調査 (2026-10-24)

## 結論 (同日の最終結果)

**BGM 消失と異常終了の原因は、プロキシが `mciseq.dll` を読み込んだまま保持していたこと。**
Windows 11 の `mciseq.dll` は、内部のリスト表(500 枠)の枠を、再生してから閉じたシーケンサー 1 個につき 1 枠ずつ使ったまま残す。
この表は DLL のアンロード(最後のシーケンサーを閉じたとき)でしか空にならない。
プロキシは Sleep フックを残すために DLL を保持していたので、表は空にならなかった。
約 820 回開いたところで満杯になり、以後は MCI_OPEN が 264 で失敗し(BGM 消失)、数分後に異常終了した。
修正は bc3387c(下の「本当の原因」)。

下の H1(ムービー音声の WAVEHDR)は実在する別の不具合で、97ab67e で修正した。
ただし、これだけを直した耐久テストでも約 61 分で同じ症状が出たので、BGM 消失・異常終了の原因ではなかった。
「耐久テストとの照合」節の H1 を原因とした結論は誤りで、「本当の原因」節で訂正している。

## 本当の原因: mciseq.dll のリスト表の枯渇 (再現・修正済み)

- 計測: 耐久テスト 3 回とも、シーケンサーの OPEN 成功 **812 / 826 / 820 回**(61〜63 分)の後に MCI_OPEN が 264 になった。
  waveOut を直した版でも同じだった。
  直前まで、ハンドル・スレッド・メモリ・移動可能メモリハンドル・タイマーは横ばい。
  `mciseq` 内の midiOutOpen / mmTaskCreate / mmioOpenW / Global・LocalAlloc に失敗はなかった(IAT フックで計測)。
- 仕組み: SysWOW64 の `mciseq.dll`(10.0.26100.x)は、リスト記述子の静的な表を 500 個持つ。
  表は base+0x63A0 にある 12 バイトの配列で、+0x17C8 の作成処理が空き枠を探し、0x1F4 個埋まっていれば -1 を返す。
  外部から読むと、再生したシーケンサーを閉じるたびに 1 枠が使われたまま残る。
  MIDI ファイルの種類は関係なく、PLAY が 337 で失敗したデバイスでは残らない。
- 普通は無害: WinMM は最後のシーケンサーを閉じると `mciseq.dll` をアンロードし、表は空になる。
  ゲームは曲を変えるたびに全デバイスを閉じてから開く。
- プロキシの問題: Sleep(4) フックを残すために `LoadLibrary` で DLL を保持していた。
  bdddbb8(v1.3.0)では曲の最初のループ時から、19c2cbe(v1.4.0)では最初の MCI_OPEN から保持していた。
- A/B の結果:
  - 単体: 保持あり → 再生・閉じる 1 回ごとに +1、保持なし → 横ばい。
  - ゲーム内: フックあり(サービス処理なしでも)→ 停止/再開 1 回につき +1.5、フックを入れない → 5 のまま。
- バトルカウントの停止/再開は 1 回で曲変更が 2 回(GMSYS_ON とその曲)になるので、使う回数が多いほど早く満杯になる。
- 修正 (bc3387c): DLL を常時保持しない。
  - MCI_OPEN のたびに(プロキシ自身の reopen/revive の OPEN も含む)その呼び出しの間だけ参照を持ち、新しく読み込まれた DLL にフックを入れ直す。
  - 修正後は停止/再開を続けても使用枠は 5 のまま。フックは再読み込みのたびに入っていることも確認した。
- 修正後の耐久テスト(97ab67e + bc3387c、同じ自動操作で 100 分):
  - 33 周、停止/再開 348 回。シーケンサーの OPEN 1,360 回はすべて成功(修正前は約 820 回で失敗)。
  - MCI のエラーは 337(GMSYS_ON)以外になく、ゲームは終了せずに動き続けた。
  - 使用枠は 5 のままで、DLL のアンロードによる初期化も確認した。
  - ムービー音声の装置は 630 回開いて 630 回閉じ、後始末待ちは 0。
  - ハンドル・スレッドは横ばい。プライベートメモリは 9 → 33 MB で、別件として経過観察。
- 曲末尾のループ(reopen)も確認した: M02(36 秒)が 2 回ループした。
  reopen 1 回の所要時間は 2.0〜2.2 ms、フックは再読み込み後も入っていた。
- 調査用ツール: `inspect_mciseq_lists.py PID [間隔秒]` で、外部から使用枠数を読み取る。
  mciseq 10.0.26100.x の配置が前提。

## 結論 (調査初期、H1 のみ確認した時点)

v1.4.0 のムービー音声の終了処理に、解放待ちが解消しなくなる不具合を確認した。
実装そのものを使う 32bit 単体検証で再現し、実行中の96版でも同じ状態の
AudioTeardown が 105 件残っていることを読み取り専用で確認した。
**今回報告された BGM 消失と異常終了の直接原因まで確定したわけではない。**
調査用ツールのみ追加し、ゲーム本体・プロキシ・配布物の修正や差し替えは行っていない。

## H1: 再生中の WAVEHDR を移動・初期化している (再現済み)

- 対象: `qtim_compat_proxy.c` の `start_movie_audio` →
  `detach_movie_audio_state` → `audio_teardown_run`。
- 入力: real-time モードの音声を再生完了前に終了する。
- 実装: `waveOutPrepareHeader` / `waveOutWrite` に渡したのは
  `&movie->audio_header`。detach はそれを `teardown->audio_header` へ値コピーし、
  元を `memset` する。その後 `waveOutReset` して、コピー側を unprepare している。
- 観測: Windows が完了フラグを書き戻す先は元の WAVEHDR。
  コピー側には `WHDR_PREPARED | WHDR_INQUEUE` (18 / 0x12) が残り、
  `waveOutUnprepareHeader` が `WAVERR_STILLPLAYING` (33) を返す。
  retry してもコピーのフラグは更新されず、音声デバイスとバッファを解放できない。
- ゲーム内の読み取り: v1.4.0 DLL の `.bss` RVA 0x2b000 +
  `_g_audio_teardown_queue` offset 0x14660 = RVA **0x3f660**。
  32bit `AudioTeardown` は 56 bytes、header.dwFlags は +20、next は +52。
  105 件すべてが prepared=1 / INQUEUE、バッファ合計 **2,662,698 bytes**。
  時間を置いた二度目にも同じノードが 105 件残っていた。
  読み取り中にゲームを停止していないので、全資源の厳密な一括スナップショットではない。

| 単体ケース | 結果 |
|---|---|
| 元の場所に WAVEHDR を保持して reset → unprepare → close | 全 API 成功 |
| プロキシの実処理で、再生中に detach → teardown | unprepare=33、3 回 retry 後も未解放 |
| 再生完了 (WHDR_DONE) を待ってから同じ detach → teardown | 正常に解放 |

このため全ムービーで必ず漏れるわけではない。スキップや終了のタイミングで増え方が変わる。
対象は real-time 音声の埋め込み WAVEHDR。picture モードの AudioChunk は
ポインターを引き継ぐ構造であり、同じ値コピーの問題とは分けて扱う。
既存の MIDI 単体反復テストでは、このムービー音声処理を実行しないため検出できない。

### 再実行

`wave_header_lifetime_probe.c` は production の C ソースを直接 include し、
ゲームをロードせずに終了処理を呼ぶ。DLL の初期化処理は実行しない。
入力 PCM は 8bit の無音。各ケースは別プロセスで実行し、再現ケースの未解放資源は
検証プロセス終了で回収する。ビルド方法はファイル冒頭に記載。

```text
wave_header_probe.exe stable
wave_header_probe.exe detached
wave_header_probe.exe detached_done
```

`detached` の exit=1 は不具合再現、環境・API の準備失敗は exit=3、
完了待ち timeout は exit=4。出力が最後まであることを確認する。
検証子プロセスには呼び出し側で時間制限を設けること。

`inspect_audio_teardown.py PID --output result.json` はプロセスに対して
QUERY_INFORMATION / VM_READ の権限だけを要求する。既知の v1.4.0 DLL の
SHA256 と一致しなければ読まない。注入・メモリ書き込み・ゲームの停止はしない。
実行中に DLL を差し替えた環境では使用しない。

### 修正方針 (未実装)

WAVEHDR は prepare から unprepare 成功まで同じアドレスと内容を保持する。
real-time 用のヘッダーを独立した所有オブジェクトにし、MovieMap から
AudioTeardown へはポインターと所有権を移す。ムービーのスロット再利用と寿命を分離する。
`WHDR_INQUEUE` を強制的に消して解放する変更は行わない。
途中終了・自然終了・再開・デバイス失敗時・スロット再利用を個別に検証し、
修正後の長時間テストで queue とハンドルの増加を比較する。

仕様の根拠:
[Audio Data Blocks](https://learn.microsoft.com/en-us/windows/win32/multimedia/audio-data-blocks)、
[waveOutWrite](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutwrite)、
[waveOutUnprepareHeader](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutunprepareheader)。

## 耐久テストとの照合 (同日、検証用コピーで自動操作)

ゲームを 1 プロセスのまま「新規ゲーム → 戦闘 → バトルカウントの停止/再開を繰り返す →
GAME OVER → タイトル」を繰り返した。v1.4.0 のプロキシ(QTIM32 `6b395c56`)のまま。

| 時点 | 観測 |
|---|---|
| 開始〜約 60 分 | ハンドル数とスレッドプール (`TpWorkerFactory` / `ALPC Port`) が単調に増加。プライベートメモリは 35 MB 以下、空きアドレス空間は 1.8 GB 以上で余裕あり |
| 約 62 分 | 曲変更のたびに MCI_OPEN が **264 (MCIERR_OUT_OF_MEMORY)** で失敗。曲変更ごとに OPEN は再試行されるが、すべて失敗する(下の訂正どおり) |
| 約 65 分 | ゲームが 0xC0000005 で終了(エラー表示なし)。直前のスレッドプールは 136、ハンドルは約 2,100 |

- 増え方の内訳: スレッドプールの増加は MIDI の曲変更回数とは比例せず、
  音声付きムービーの数とおおむね比例した。MIDI の開閉だけを 300 回繰り返す単体検証、
  waveOut / midiOut / sndPlaySound の単体反復では増えない。
- waveOutOpen / waveOutClose を数える計測版プロキシ(検証用コピーだけに配置)で 12 分:
  **open 91 / close 61**。差の 30 は同じ時間のスレッドプール増加 (+31) と一致した。
  未完了の teardown の再試行は約 300 万回(drain のたびに waveOutReset を呼び直す)。
- 単体再確認: キュー中の WAVEHDR を値コピーして元を 0 クリアし、reset 後にコピーを
  unprepare すると 33 が返り続ける(H1 と同じ)。reset は元のアドレスに WHDR_DONE を書く。

**(訂正: 次の結論は誤り。**
**H1 を直した版でも約 61 分で同じ症状になった。原因は冒頭の「本当の原因」。)**

当初の結論: H1 で閉じられなくなった waveOut が、開いたまま音声ストリームを保持し続ける。
これが積み上がると、プロセスの MCI_OPEN が 264 で失敗する(BGM 消失)。
ゲームはその数分後に異常終了する。
BGM が「停止を押した瞬間」に消えるのは、停止/再開が曲変更(CLOSE → OPEN)を伴い、
OPEN の失敗が最初に表に出るのがその操作だから。
以後、停止/再開が即座に切り替わるのは、OPEN が即座に失敗して MIDI の開閉待ち
(数百 ms)がなくなったため。
異常終了の直接の命令位置はダンプがないため未特定。
ただし発生順(漏れの蓄積 → 264 → 終了)は報告と再現で一致している。

補足: `g_movies` は解放したスロットを次のムービーが再利用する。
detach 後も旧デバイスが元のアドレス(スロット内の `audio_header`)を参照している。
そのため、同じスロットで次のムービーが同じアドレスの WAVEHDR を prepare すると、
二つのデバイスが一つのアドレスを共有しうる。
reset が WHDR_DONE を書き込む先も、別ムービーのヘッダーになりうる。
ヒープ破損の経路としては未検証だが、修正で同時に解消する。

## 修正計画

### 方針

H1 だけを直す。MIDI・入力サービス・メニューの仮説は別件として扱い、同時に変更しない
(1 仮説 = 1 変更)。

### 変更内容 (`work/qtim_proxy/qtim_compat_proxy.c`)

1. real-time 用の全長ヘッダーを、独立したヒープオブジェクトにする。
   - `MovieMap.audio_header` / `audio_buffer` / `audio_bytes` / `audio_prepared` を、
     ポインター 1 個 (`AudioChunk *audio_full`) に置き換える。
     既存の `AudioChunk`(WAVEHDR・データ・prepared を一体で所有)を流用する。
   - `start_movie_audio` / `restart_realtime_movie_audio` は `&movie->audio_full->header`
     を prepare / write / unprepare する。
   - データは今の `audio_buffer` と同じ HeapAlloc。解放は `free_audio_chunk` に統一する。
2. 所有権は値コピーせずにポインターで移す。
   - `detach_movie_audio_state` は `audio_full` のポインターを `AudioTeardown` へ移し、
     MovieMap 側を NULL にする。ヘッダーのアドレスと内容は prepare から unprepare 成功まで
     変わらない。
   - `AudioTeardown.audio_header` / `audio_buffer` / `audio_prepared` も、同じポインター 1 個にする。
3. `audio_teardown_run` の手順は reset → unprepare(元のアドレス)→ close → free。
   失敗時は今と同じく保持して再試行し、ハンドルは捨てない。`WHDR_INQUEUE` の強制クリアもしない。
4. 再試行の間隔を空ける。
   - 1 件ごとに最終試行時刻を持ち、100 ms 未満では reset を呼ばない。
     drain のたびに waveOutReset を呼び直すのをやめる。
   - 5 秒以上残った teardown は、trace に 1 回だけ記録する
     (`audio_teardown_stuck result=... age_ms=...`)。
5. 通常 trace(`[trace] enabled=1`)に音声デバイス数を出す。
   - waveOutOpen と close 成功の累計、未完了 teardown 数を、増減時に 1 行記録する。
   - 次にユーザー環境で起きたとき、ログだけで判別できるようにする。
6. picture モードの `AudioChunk` 群は、すでにポインターで引き継いでいるので変更しない。
   ただし `audio_full` と同じ teardown を通ることを確認する。

### やらないこと

- `WHDR_INQUEUE` を消して unprepare を通す、未解放のハンドルを捨てる。
- MIDI の開閉や `mciseq` Sleep フックの変更(再入の仮説は別途検証)。
- `shell_open` の ShellExecuteEx + SEE_MASK_NOASYNC 化(妥当だが別件。別コミットで扱う)。
- ゲーム本体の STATUS/PLAY 失敗時に ID が残る挙動(原作どおり。今回の再現では通っていない)。

### 検証

1. 単体 (`wave_header_lifetime_probe.c`)
   - 既存の `stable` / `detached` / `detached_done` に、次のケースを加える。
     - `restart`: GoToBeginning の再始動
     - `slot_reuse`: 解放直後に同じスロットで次のムービーを開始
     - `pause`: modal 中に pause してから終了
     - `open_fail`: デバイスを開けない
   - 修正後は全ケースで open = close、teardown キュー 0 になること。
2. 実機の耐久テスト(上と同じ自動操作、90 分以上)
   - 計測: waveOut の open − close と未完了 teardown 数が、ムービー終了後に 0 へ戻ること。
     スレッドプールとハンドル数が横ばいであること。
   - MCI_OPEN 264 がなく、異常終了しないこと。
   - 修正前は約 65 分で終了した(比較基準)。
3. 回帰
   - 出撃・巡航ムービー(コントローラー経路)の音声
   - 戦闘ムービーの音声
   - ムービー途中のクリックによるスキップ
   - ドラッグ中の pause / 再開
   - 最小化 → 復元
   - OP(cvid、原本 QuickTime)
4. `python work/build_all.py`(リリーステスト PASS)。
   プロキシの変更なので Sol レビューを受ける。
5. ユーザー実機確認待ち: 同じ操作で 30〜90 分以上、BGM が続くこと、停止/再開の反応が
   従来どおり(数百 ms の切り替え)であること、ゲーム進行と音声に問題がないこと。

### リリース

- v1.4.1(QTIM32 だけの変更)。
- apply.ps1 の既知プロキシ一覧に新しいハッシュを加え、v1.4.0 からの上書き更新を確認する。
- CHANGELOG には「長時間プレイで BGM が消え、その後に終了することがある問題を修正」と記載する。

## BGM の説明の訂正

`FUN_00413250` の停止・再開は、それぞれ `FUN_004215a0` → `FUN_004216a0`
で曲を変更する。BGM の ID は 0x0043ddd8。
0x0042174d の CLOSE の後に 0x0042174f で ID を 0 にし、
0x00421782 の OPEN が失敗するとそのまま戻る。ポーリングは ID=0 なら何もしない。
しかし **次の曲変更では OPEN が再び呼ばれる**。
「一度 OPEN に失敗すると、その後は二度と BGM を開かない」は正しくない。

さらに STATUS/PLAY の失敗時には、0x004217bc / 0x004217f4 で CLOSE した後、
ゲーム側 ID を 0 にしていない。失敗を取得する場合は OPEN だけでなく
STATUS/PLAY/CLOSE と ID の対応も確認する必要がある。
この既存の失敗処理が今回通った証拠はまだない。

## 既存の二つの仮説

- `mciseq_sleep_hook` → `gt_scale_service_frame_input` → `DispatchMessageA` /
  sent-message dispatch → `WM_SIZE` → ゲームの 0x0040f230 → MCI_PAUSE
  という再入経路は存在する。CLOSE から戻る前は旧 BGM ID が残っている。
  `g_service_busy` は入力サービスの再帰を止めるもので、MCI 全体の再入防止ではない。
  実際の障害再現はまだない。現在の長時間テストの入力経路ログにも再現記録はない。
- `menu_tools.c:shell_open` は短命のスレッドで ShellExecuteW を呼び、直後に
  CoUninitialize / FreeLibrary する。ShellExecuteEx + SEE_MASK_NOASYNC を使う
  べき条件に該当するが、これだけで今回のヒープ破損原因とは断定できない。
  Microsoft の説明は DDE/起動処理が完了しない問題を挙げている。
  [SHELLEXECUTEINFO](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfoa)。

## クラッシュと検証の限界

- Windows の障害ログに同じ PID・開始時刻の ntdll 内アクセス違反が 3 秒差で 2 件。
  独立した二回のプレイのクラッシュではない。例外コードは 0xc0000005。
  ダンプ・スタックなしでは、上書きしたコードや BGM 消失との因果関係は特定できない。
- 耐久テストでは、約 44 分時点の MCI 記録に OPEN 失敗はなかった。
  PLAY の非ゼロはリセット音の 337。その後、約 62 分で OPEN 264 が始まり、
  約 65 分で終了した(上の「耐久テストとの照合」)。
  再現中の記録に `midi_restart`、`frame_input`、`modal_loop enter` はなく、
  曲末尾の reopen と UI 再入は再現に関与していない。
- メニューバー使用の有無はユーザーの記憶が不確かなため、使用なしとは扱わない。
- ユーザー実機確認待ち: 修正候補を作成・検証した後、同じ EXE と操作で
  BGM 継続、停止/再開、音声・ゲーム進行、30〜90 分以上の安定性を確認する。
  EXE SHA256(v1.4.0・v1.4.1 で共通):
  `77a2b963068f4a1b794bea234b737d1c28770a5615b1d788d6b77306c21771cd`。
  QTIM32 SHA256: 不具合のある v1.4.0 は
  `6b395c569151843f6c36965b72bfbd819804a5fe76c3e284561c13bbe8fc8520`、
  修正版の v1.4.1 は
  `58008148b61592adbc5c53f70aefaf2ddd20e11a54a31747c1d7e8e71aab6398`。
