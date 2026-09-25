# ガンダムタクティクス MOBILITY FLEET0079 Windows 11 互換パッチ

『ガンダムタクティクス MOBILITY FLEET0079』の 01版 (2001年9月19日発売の復刻版) と 96版 (1996年発売のオリジナル版) を Windows 11 上で遊べるようにする非公式の互換パッチのソースコードです。

## 対象

| 項目 | 内容 |
| --- | --- |
| タイトル | ガンダムタクティクス MOBILITY FLEET0079 |
| 版 | 01版: 復刻版 (2001年9月19日発売) / 96版: オリジナル版 (1996年発売、v1.2.0 から) |
| ゲームの対応 OS | Windows 95/98/Me |
| パッチの動作環境 | Windows 11 |

01版と 96版の `gundam.exe` は同一のファイルです。インストーラーは `gundam.exe` の SHA256 を照合し、未改変のもの、またはこのパッチの既知のバージョンを当てたもの以外には適用しません。

96版は CD から起動する方式なので、インストーラーが CD の中身をフォルダへコピーし、QuickTime のファイルを CD の `qt32.exe` から取り出して 01版と同じ配置にしてからパッチを当てます (v1.2.0)。

主な修正内容:

- BGM・効果音が鳴らない問題
- ムービーが暗転する、またはスキップされる問題 (QuickTime 互換プロキシ `QTIM32.dll` / `CMGR32.dll`)
- フェード演出の代わりに画面が上下反転する問題
- インストール先パスの長さ制限、港画面の縦線など
- ゲーム画面の拡大表示 (ウィンドウのサイズ変更・最大化・Alt+Enter で全画面、v1.1.0)
- 96版への対応 (v1.2.0)

各バージョンの変更点は [CHANGELOG.txt](release/GundamTactics_Win11Patch/CHANGELOG.txt) を参照してください。

## 利用者向け

このリポジトリの GitHub Releases ページから配布用 ZIP (ビルド済み DLL を含む) をダウンロードしてください。導入手順は [README.txt](release/GundamTactics_Win11Patch/README.txt) にあります。

このリポジトリには、ゲーム本体・ゲームのデータ・改変済み実行ファイル・ビルド済み DLL は含まれていません。パッチはお手元のゲームの `gundam.exe` にインストーラーがその場で適用します。ゲーム本体はご自身で用意してください。

## 攻略データ

戦闘の計算式、パイロットの成長、命令と AI の動き、キャンペーンの分岐、全ユニット・全ミッションのデータをまとめた非公式の攻略資料を公開しています。

- ブラウザで見る: https://61bi-234469.github.io/gundam-tactics-win11-patch/
- ファイル: [guide/index.html](guide/index.html)

ゲームの動作を調べて分かった仕様と数値を、独自の文章・表・図にまとめたものです。ゲームの画像・文章・プログラム・データファイルは含みません。

## 開発者向け

### 構成

| パス | 内容 |
| --- | --- |
| `release/GundamTactics_Win11Patch/` | 配布パッケージのテンプレート (インストーラー・README・CHANGELOG)。DLL はビルド時に追加されます |
| `work/exe_patches/` | `gundam.exe` にバイナリパッチを当てるスクリプト (Phase 1〜5) |
| `work/qtim_proxy/` | QuickTime 互換プロキシ DLL (`QTIM32.dll` / `CMGR32.dll`) とムービーデコーダーのソース |
| `work/tools/` | 検証・解析用のツール |
| `work/analysis/ghidra_scripts/` | 解析に使った Ghidra スクリプト |
| `work/build_all.py` | パッチ適用、プロキシのビルド、配布 ZIP の作成、リリーステストまでを通しで実行するスクリプト |
| `guide/` | 攻略データのページ (GitHub Pages で公開。`.github/workflows/pages.yml`) |

### ビルドに必要なもの

- Windows 11
- Python 3.10 以上
- MSYS2 MinGW 32bit gcc (既定のパス: `C:\msys64\mingw32\bin\gcc.exe`)
- 未改変のゲームのインストールファイル一式を `source_exe_01/` に置くこと (`gundam.exe` の SHA256 は `work/build_all.py` の `EXPECTED_SOURCE_SHA256` で照合されます)

```
python work/build_all.py
```

Ghidra を使う解析スクリプト (`work/tools/run_ghidra_*.ps1`) を使うときは、環境変数 `GHIDRA_INSTALL_DIR` を設定するか、`-GhidraRoot` 引数で Ghidra のインストール先を指定してください。

## ライセンス

このリポジトリのソースコードは [MIT License](LICENSE) で公開しています。ゲーム本体とその画像・音声・データは対象外です。

## 免責

ファンによる非公式の互換パッチです。原作の権利者・販売元とは関係ありません。「ガンダムタクティクス MOBILITY FLEET0079」およびその画像・音声・データの著作権は各権利者に帰属します。
