# AGENTS.md — ガンダムタクティクス MOBILITY FLEET0079 Win11 互換パッチ

エージェント向けの作業ルール。ローカル環境・非公開情報は `AGENTS.local.md`(git 管理外)に分離してあるので、
存在すれば必ず併せて読むこと。

## 目的

『ガンダムタクティクス MOBILITY FLEET0079』の 01版(2001-09-19 発売の復刻版)と 96版(1996 年発売の
オリジナル版)(いずれも Windows 95/98/Me 用)を Windows 11 上で遊べるようにする **配布可能な非公式互換パッチ** を作成・保守する。

当初の課題(いずれも対応済み。詳細は `release/GundamTactics_Win11Patch/CHANGELOG.txt`):

1. 起動: プロパティで「Win95 互換」「16 ビットカラー」「管理者として実行」が必要だった
2. 音声: BGM と効果音が無音になる
3. ムービー: 暗転する、または再生されずスキップされる
4. 描画: フェードイン/アウトの暗転描写が正しく動作せず、画面が上下反転する

要件は「上記が解消され Windows 11 上で遊べること」と「配布可能なパッチが作成されていること」。

版の呼び名: 文書では「01版」(2001 年の復刻版)と「96版」(1996 年のオリジナル版)に統一する。利用者向け README では
初出に括弧で補足し、英語では "2001 reissue" / "1996 original edition" とする。01版が先行し、96版への対応は
2026-09-25 に開始した(v1.2.0)。
版ごとのリポジトリ内フォルダは接尾辞 `_01` / `_96`(実行コピーは `run/GT01` / `run/GT96`)で揃える。
版に共通のものは接尾辞を付けない。96版だけのものは `96` を含む名前にする(例: `import96.ps1`)。

## 絶対ルール

- 原本フォルダ `source_iso_01/` `source_exe_01/`(01版)と `source_iso_96/` `source_exe_96/`(96版)は **絶対に改変しない**。
  書き込み・リネーム・削除も禁止。ビルドと検証の原本としてだけ使う。
- 実行検証は `run/GT01/`(01版、`source_exe_01/` の複製)と `run/GT96/`(96版、ISO の中身に `source_exe_96/Windows/SysWOW64/` の QuickTime 14 本を加えたもの)で行う。
  自由に上書きしてよく、壊れたら再コピーする。版をまたいでファイルを混ぜない。
- ゲーム本体・データ・改変済み `gundam.exe`・ビルド済み DLL・スクリーンショット等の著作物は
  コミットしない(`.gitignore` 参照)。コミット前に `git status` で追跡対象を確認する。
- `gundam.exe` へのバイナリ改変は **期待バイト列検証付きの Python スクリプト**(`work/exe_patches/`)
  でのみ行う。対象 VA/ファイルオフセット・変更前後のバイト列・根拠・戻し方をスクリプトに記録する。
  手作業のバイナリ編集は禁止。
- 同梱の QuickTime 2.x(QTIM32/CMGR32/*.QTC)を延命する対症療法パッチは行わない。過去の試行で
  副作用の連鎖に陥ると結論済み。ムービーは `work/qtim_proxy/` の互換プロキシ側で完結させる。
- 同一サブシステムで局所パッチが 3 個連続したら手を止めて方針を再評価し、記録に残す。
- 評価軸は常に「**ゲーム進行と音声を壊さない**」。ムービーや描画が不完全でも、進行と音声が
  守られていれば前進してよい。
- 1 仮説 = 対象アドレス + 入力 + 観測結果(スクリーンショット/ログ)を 1 セットで記録する。
  複数仮説の同時変更は禁止。
- 変更はこまめに git commit する。コミットメッセージは `feat:`/`fix:`/`docs:`/`chore:`/`release:`
  prefix の英語 1 行(必要なら本文を続ける)。

## フォルダ構成

| パス | 用途 | 書き換え |
|---|---|---|
| `source_iso_01/` | 01版インストールディスク ISO(原本、git 管理外) | 禁止 |
| `source_exe_01/` | 01版インストール直後のファイル群(原本、git 管理外) | 禁止 |
| `source_iso_96/` | 96版ディスク ISO(原本、git 管理外) | 禁止 |
| `source_exe_96/` | 96版インストーラーの出力一式(原本、git 管理外。`G-TACT/`・`Windows/`・`Windows/SysWOW64/`・`StartMenu/` に配置先別で保存) | 禁止 |
| `run/GT01/` | 01版の実行検証用ゲームコピー(git 管理外) | 自由 |
| `run/GT96/` | 96版の実行検証用ゲームコピー(ISO の中身 + `source_exe_96/Windows/SysWOW64/` の QuickTime 14 本。git 管理外) | 自由 |
| `work/build_all.py` | パッチ適用 → プロキシビルド → 配布 ZIP 作成 → リリーステストの一括実行 | 自由 |
| `work/exe_patches/` | `gundam.exe` バイナリパッチスクリプト(Phase 1〜5) | 自由 |
| `work/qtim_proxy/` | QuickTime 互換プロキシ DLL(`QTIM32.dll` / `CMGR32.dll`、C, MinGW 32bit)とムービーデコーダ | 自由 |
| `work/tools/` | 検証・解析ツール(起動検証、デスクトップキャプチャ、MOV 棚卸し、Ghidra 実行スクリプト等) | 自由 |
| `work/analysis/ghidra_scripts/` | 解析に使った Ghidra スクリプト(その他の解析出力は git 管理外) | 自由 |
| `release/GundamTactics_Win11Patch/` | 配布パッケージのテンプレート(インストーラー・README・CHANGELOG) | リリース時 |
| `guide/index.html` | 攻略データのページ(生成物。GitHub Pages で公開。直接編集しない) | 生成時 |

## ビルド・検証

- 必要環境: Windows 11、Python 3.10 以上、MSYS2 MinGW 32bit gcc(既定 `C:\msys64\mingw32\bin\gcc.exe`)。
  プロキシ DLL は 32bit 必須。日本語を含むパスを ld に渡すと失敗するので、ビルドは相対パスで呼ぶ。
- 一括ビルド: `python work/build_all.py`(`source_exe_01/gundam.exe` の SHA256 を照合してから実行される)。
  配布物は `release/` へ配置された後にリリーステスト(`work/tools/test_release_cycle.ps1`)が走る。
  テストが FAIL した配布物は公開・配布せず、修正して再ビルドする。
- Ghidra を使うスクリプト(`work/tools/run_ghidra_*.ps1`)は環境変数 `GHIDRA_INSTALL_DIR` または
  `-GhidraRoot` 引数でインストール先を指定する。
- 実機検証: ゲームは GUI アプリ。`run/GT01/gundam.exe` を起動し、ウィンドウ(`Gundam Tactics`)の存在、
  `ERROR` ダイアログの有無、スクリーンショットを自動取得する(`work/tools/verify_launch.py`、
  `work/tools/game_probe.ps1`)。検証後はプロセスを kill する。
  - ウィンドウ DC のスクリーンショットは実画面を反映しない(直接描画と上下反転が写らない)。
    画面の証拠はデスクトップキャプチャ(`work/tools/desktop_capture.ps1`)だけを採用する。
  - ゲームのメニューは「選択 → 確定」の 2 クリック。ムービー中のクリックはムービーを中断する。
- 人間による目視・音の確認が必要な項目は、起動 exe(ハッシュ)・操作手順・確認観点を明記して
  「ユーザー実機確認待ち」として残す。

## 技術上の確定事項(再調査不要)

- 原本 `gundam.exe` は 543,744 bytes、32bit PE、ImageBase 0x400000。SHA256 は
  `work/build_all.py` の `EXPECTED_SOURCE_SHA256`。
- 96版と 01版の差分(2026-09-25 に ISO 全ファイルのハッシュで比較): `gundam.exe` は SHA256 一致(同一ビルド)。
  ゲームデータも `Bmp/BDEC.BMP`・`Bmp/PICT1.BMP`・`Bmp/TITLE/TITLE.BMP` の 3 枚を除き一致。
  インストーラー `setup.exe` は別物(96版は 32bit PE、1996-08-29)。96版は QuickTime DLL/コーデックを
  ゲームフォルダに同梱せず、ディスク上の `qt32.exe`(QuickTime 2.x インストーラー)でシステムに導入する方式。
  01版の `QTIM32.DLL`/`CMGR32.DLL`/`*.QTC`/`MCIQTENU.Q32` と `GundamStart.exe` は 96版のディスクに無い。
- 96版のインストール(2026-09-25 に Win11 で実施): `setup.exe` はゲームを HDD にコピーせず、ディスク上の
  `gundam.exe` を指すショートカットを作るだけ(作業フォルダはディスクのルート)。既定のインストール先
  `C:\G-TACT` はセーブの保存先(`C:\Windows\Gundam.ini` の `DataDir`)で、`gundam.ico`・`readme.doc` だけが入る。
  `qt32.exe` は QuickTime を `SysWOW64` と `C:\Windows` に入れる。そのうち 14 本
  (`QTIM32.DLL`・`CMGR32.DLL`・`*.QTC`・`MCIQTENU.Q32`)は 01版の同梱ファイルとバイト単位で同一。
  96版だけにあるのは `HNDLR32.DLL`・`QTOLE32.DLL`・`QTW32.CPL`・`QTWMCI32.DLL` とプレーヤー類。
- 01版と 96版は `gundam.exe` が同じなので、`Gundam.ini` とセーブフォルダ(`DataDir`、既定 `C:\G-TACT`)を共有する。
  `C:\G-TACT` はセーブ置き場なので、パッチはゲームのコピー先にしない(ユーザー指示、2026-09-25)。
- 音声(Phase 1): 色深度チェック迂回、WAVEHDR 生存期間修正、MIDI ポーリング間引き。
  WAV は waveOut、BGM は MCI sequencer(`Sound\*.MID`)。MIDI の長い無音尾部はプロキシの
  mciSendCommand フックで短縮コピーに差し替える(v1.0.15)。
- ムービー: ゲームは `QTIM32.DLL` の selector ディスパッチャ経由で
  `OpenMovieFile(0x2C) → NewMovieFromFile(0x2A) → NewMovieController(0x38) → GetMoviePict(0x14)
  → PicToDIB(0x2E) → 自前 DrawDIB → KillPicture(0x08)` を回す。プロキシが SMC(`smc `)を
  自前デコードして DIB を返す。出撃・巡航ムービーはコントローラ経路(0x38/0x32/0x36/0x2F/0x12/0x06/0x37)を
  使うので、プロキシが fake controller でエミュレートする。プロキシが処理しない selector と、
  fake controller を含まない呼び出しは原本(`QTIM32R.DLL` にリネーム)へ転送する。
  selector ごとの挙動は `work/qtim_proxy/README.md` が正。
  Cinepak(`cvid`、OP など 3 本)は原本 QuickTime のまま動く。
  - Phase 2(`patch_movie_phase2.py`)は GetMoviePict 失敗時の NULL DIB を guard し
    `Wrong DIB handle` ダイアログを抑止する。
  - ディスパッチャのラッパーに call-then-return 経路を追加しない(スタック引数が 4 ずれて壊れる)。
    tail-jmp で通す。
  - ガードはプロキシ自身が発行した値(fake handle)だけをキーにする。QuickTime のムービーハンドルは
    再利用されるので、それをキーにしてはいけない。
  - `DisposeMovie(0x07)` は実 Movie handle なら必ず原本へ転送する(飲み込むとムービーがリークし
    65 本目の NewMovieFromFile で失敗する)。fake controller を含む呼び出しだけプロキシが吸収する。
- MOV 資産は 1,199 本。映像は `smc `(8bpp)1,196 本、`cvid` 3 本。音声は `raw `(8bit PCM)1,173 本、
  `twos` 3 本。解像度は 320x240 / 64x64 / 288x208 / 200x128 / 400x300 / 128x128 / 288x192。
  棚卸しは `work/tools/inventory_movies.py`。
- 描画反転(Phase 3): NT の `GetObject(DIBSECTION)` が top-down セクションに正の biHeight を
  返し、それが `StretchDIBits` へ渡るため。コードケーブで biHeight を反転して解決。
- パス長(Phase 4): `gundam.exe` は 80 バイト固定バッファでパスを組む。getcwd ラッパーを
  "." を返すよう変更し、インストール先の制限を撤廃(インストーラー側で 230 バイト・ANSI コードページに限定)。
- ハーバー画面の縦線(Phase 5): 原作データの幅不一致(587px → 576px 縮小)。
  `SetStretchBltMode(COLORONCOLOR)` を挿入して解決。
- 画面拡大(v1.1.0): プロキシがメインウィンドウの DC 要求にシャドウ DC を返し、
  `StretchDIBits` で拡大表示。`qtim_compat.ini` の `[display]` で設定。

## 配布物

- 配布 ZIP にゲームの著作物を含めない。インストーラー(`install.bat` → `patch_files/install.ps1`)が
  利用者の `gundam.exe` の SHA256 を照合し、未改変の 01版・96版(同一の gundam.exe)または既知の旧パッチ版にだけ適用する。
- 96版(v1.2.0 から): `install.ps1` がディスク(CD-ROM ドライブ、マウントした ISO、`-DiscDir`)を見つけると、
  インストール時に選ばせた場所の中の `GundamTactics96`(既定 `C:\GundamTactics96`、`-GameDir` で直接指定)へ
  `patch_files/import96.ps1` がディスクの中身をハッシュ付きでコピーし、
  QuickTime 14 本を `qt32.exe` 内の SZDD ブロックから展開してハッシュ照合してから、通常どおり `apply.ps1` を当てる。
  Windows には何もインストールしない。コピーや apply が失敗したら、その回に作ったものだけを消す。
- 新バージョンでは apply/revert が旧バージョンからの上書き更新・解除に対応しているかを
  リリーステストで確認する。
- 配布テンプレート(README.txt 等)は CRLF。Git Bash の `sed -i` は CRLF を落とすので編集後に確認する。
