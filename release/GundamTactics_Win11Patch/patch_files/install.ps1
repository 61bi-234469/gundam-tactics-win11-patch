# v1.2.0 - install.bat / uninstall.bat から呼ばれる対話インストーラー。
# 実際のファイル操作は apply.ps1 / revert.ps1 が行う。96版はディスクから
# 起動する方式なので、import96.ps1 でゲームをフォルダへコピーしてから当てる。
param(
    [ValidateSet("install", "uninstall")][string]$Mode = "install",
    [string]$GameDir = "",
    [string]$DiscDir = "",
    [switch]$Yes,
    [switch]$Elevated
)

$ErrorActionPreference = "Stop"
$packageRoot = Split-Path -Parent $PSScriptRoot
$isInstall = $Mode -eq "install"
$action = if ($isInstall) { "パッチの適用" } else { "パッチの解除(元に戻す)" }
# 96版のコピー先。インストール時に選んだ場所の中に GundamTactics96 を作る。選ばなかったとき
# (と -Yes)の既定は C:\GundamTactics96。C:\G-TACT は 96版のセーブの保存先なので使わない。
$Destination96FolderName = "GundamTactics96"
$DefaultDestination96 = "C:\GundamTactics96"
$script:discSource = ""
$script:destinationGiven = $false

function Write-Title([string]$Text) {
    Write-Host ""
    Write-Host "==== $Text ====" -ForegroundColor Cyan
}

function ConvertTo-CleanPath([string]$Path) {
    $clean = $Path.Trim().Trim('"').Trim()
    if ($clean.Length -gt 3) { $clean = $clean.TrimEnd([char[]]@('\', '/')) }
    return $clean
}

function Test-GameFolder([string]$Directory) {
    if (-not $Directory) { return $false }
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return $false }
    if ($isInstall) { return Test-Path -LiteralPath (Join-Path $Directory "gundam.exe") }
    return Test-Path -LiteralPath (Join-Path $Directory "gundam.exe.orig")
}

# 96版のディスク: gundam.exe と qt32.exe があり、QuickTime はゲームの横に無い。
function Test-Disc96([string]$Directory) {
    if (-not $Directory) { return $false }
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return $false }
    return (Test-Path -LiteralPath (Join-Path $Directory "gundam.exe") -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Directory "qt32.exe") -PathType Leaf) -and
        -not (Test-Path -LiteralPath (Join-Path $Directory "QTIM32.DLL"))
}

function Find-Disc96 {
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -ne [IO.DriveType]::CDRom -or -not $drive.IsReady) { continue }
        if (Test-Disc96 $drive.RootDirectory.FullName) { return $drive.RootDirectory.FullName }
    }
    return ""
}

function Select-GameFolderDialog {
    Add-Type -AssemblyName System.Windows.Forms
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "ガンダムタクティクスをインストールしたフォルダ(gundam.exe があるフォルダ)を選んでください。"
    $dialog.ShowNewFolderButton = $false
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
        return ""
    } finally { $dialog.Dispose(); $owner.Dispose() }
}

function Select-Destination96Dialog {
    Add-Type -AssemblyName System.Windows.Forms
    $owner = New-Object System.Windows.Forms.Form
    $owner.TopMost = $true
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "96版のゲームをコピーする場所を選んでください。選んだフォルダの中に $Destination96FolderName フォルダを作ります。(キャンセルすると $DefaultDestination96)"
    $dialog.ShowNewFolderButton = $true
    $dialog.SelectedPath = "C:\"
    try {
        if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
        return ""
    } finally { $dialog.Dispose(); $owner.Dispose() }
}

function Test-PathWithin([string]$Path, [string]$Root) {
    if (-not $Path -or -not $Root) { return $false }
    try {
        $full = [IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
        $base = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    } catch { return $false }
    return $full.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)
}

# セーブの保存先: 既定の C:\G-TACT と、Gundam.ini(UAC の VirtualStore 側も)の DataDir。
function Get-SaveFolders {
    $folders = @("C:\G-TACT")
    $inis = @((Join-Path $env:WINDIR "Gundam.ini"))
    if ($env:LOCALAPPDATA) { $inis += Join-Path $env:LOCALAPPDATA "VirtualStore\Windows\Gundam.ini" }
    foreach ($ini in $inis) {
        try {
            foreach ($line in [IO.File]::ReadAllLines($ini, [Text.Encoding]::Default)) {
                if ($line -match '^\s*DataDir\s*=\s*(.+?)\s*$') { $folders += $Matches[1] }
            }
        } catch { }
    }
    return $folders
}

# 96版のコピー先にできない場所なら理由を返す(できるなら空)。
function Get-Destination96Refusal([string]$Destination) {
    if ($script:discSource -and (Test-PathWithin $Destination $script:discSource)) {
        return "ディスクの中はコピー先にできません: $Destination"
    }
    foreach ($save in Get-SaveFolders) {
        if (Test-PathWithin $Destination $save) {
            return "$save はセーブデータの保存先なので、ゲームのコピー先にできません。別の場所を選んでください: $Destination"
        }
    }
    return ""
}

# 選ばれた場所がすでにコピー済みのゲームか GundamTactics96 自体ならそのまま、それ以外はその中に作る。
function Resolve-Destination96([string]$Picked) {
    if ((Test-GameFolder $Picked) -or (Split-Path -Leaf $Picked) -ieq $Destination96FolderName) { return $Picked }
    return Join-Path $Picked $Destination96FolderName
}

# 自動で見つけたゲームのフォルダを使う。96版のディスクも入っていれば、どちらにするか聞く。
function Resolve-AutoCandidate([string]$Candidate) {
    if ($isInstall -and -not $Yes -and -not $Elevated) {
        $disc = Find-Disc96
        if ($disc) {
            Write-Host "ゲームのフォルダが見つかりました: $Candidate"
            Write-Host "96版のディスクも入っています: $disc"
            $answer = Read-Host "1 = このフォルダにパッチを当てる / 2 = 96版のディスクからコピーしてパッチを当てる (1/2)"
            if ($answer -match '^\s*[2２]') { $script:discSource = $disc; return $DefaultDestination96 }
        }
    }
    return $Candidate
}

function Resolve-GameFolder {
    if ($isInstall -and $DiscDir) {
        # 96版: -DiscDir がコピー元、-GameDir(省略時は既定)がコピー先
        $script:discSource = ConvertTo-CleanPath $DiscDir
        if ($GameDir) { $script:destinationGiven = $true; return ConvertTo-CleanPath $GameDir }
        return $DefaultDestination96
    }
    if ($GameDir) {
        $candidate = ConvertTo-CleanPath $GameDir
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $candidate = Split-Path -Parent $candidate }
        if ($isInstall -and (Test-Disc96 $candidate)) { $script:discSource = $candidate; return $DefaultDestination96 }
        return $candidate
    }
    # ZIP の中身をゲームフォルダ(またはその中のサブフォルダ)に展開した場合
    foreach ($candidate in @($packageRoot, (Split-Path -Parent $packageRoot))) {
        if (Test-GameFolder $candidate) { return Resolve-AutoCandidate $candidate }
    }
    # 01版の setup の既定フォルダ(ドライブ付きの "\Program Files\BANDAI\GundamTactics\")
    $defaults01 = @("C:\Program Files\BANDAI\GundamTactics")
    if (${env:ProgramFiles(x86)}) { $defaults01 += Join-Path ${env:ProgramFiles(x86)} "BANDAI\GundamTactics" }
    foreach ($candidate in $defaults01) {
        if (Test-GameFolder $candidate) { return Resolve-AutoCandidate $candidate }
    }
    # 旧版のパッチの自動検出先(96版の setup のインストール先。セーブの保存先でもある)
    if (Test-GameFolder "C:\G-TACT") { return Resolve-AutoCandidate "C:\G-TACT" }
    # 96版をこのパッチでコピーした既定のフォルダ
    if (Test-GameFolder $DefaultDestination96) { return Resolve-AutoCandidate $DefaultDestination96 }
    if ($isInstall) {
        $disc = Find-Disc96
        if ($disc) { $script:discSource = $disc; return $DefaultDestination96 }
    }
    Write-Host "ゲームのフォルダを自動で見つけられませんでした。フォルダ選択の画面から選んでください。"
    Write-Host "(96版の場合は、ディスクのドライブを選んでください)"
    $selected = Select-GameFolderDialog
    if ($isInstall -and (Test-Disc96 $selected)) { $script:discSource = $selected; return $DefaultDestination96 }
    return $selected
}

# このパッチが今回コピーした 96版のファイルだけを消して、実行前の状態に戻す。
# 消せずに残ったパスを返す(空なら実行前の状態に戻った)。
function Undo-Import96($Import, [bool]$CreatedDestination, [string]$Destination) {
    $left = @()
    if ($Import) {
        foreach ($path in $Import.Created) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue }
            if (Test-Path -LiteralPath $path) { $left += $path }
        }
        if ($Import.CreatedDestination) { $CreatedDestination = $true }
    }
    if ($CreatedDestination -and $Destination -and (Test-Path -LiteralPath $Destination) -and -not (Get-ChildItem -LiteralPath $Destination -Force)) {
        Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $Destination) { $left += $Destination }
    }
    return $left
}

function Test-WriteAccess([string]$Directory) {
    $probe = Join-Path $Directory (".gt_patch_write_test_{0}.tmp" -f [guid]::NewGuid().ToString("N"))
    try {
        [IO.File]::WriteAllBytes($probe, [byte[]]@())
        Remove-Item -LiteralPath $probe -Force
        return $true
    } catch [UnauthorizedAccessException] {
        return $false
    }
}

function Test-Administrator {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Windows のコマンドライン規則: 閉じ引用符の直前の \ は二重にする。
function ConvertTo-QuotedArgument([string]$Value) {
    return '"' + ($Value -replace '(\\+)$', '$1$1') + '"'
}

function Get-FriendlyHint([string]$Message) {
    if ($Message -match "install path is too long") {
        return "ゲームのフォルダのパスが長すぎます。C:\GundamTactics のような短いフォルダへゲームのフォルダごと移動してから、もう一度実行してください。"
    }
    if ($Message -match "cannot represent") {
        return "フォルダ名に、このパッチで扱えない文字(韓国語・絵文字など)が含まれています。英数字か日本語だけの名前のフォルダへ移動してから、もう一度実行してください。"
    }
    if ($Message -match "rollback incomplete") {
        return "ディスクからコピーしたファイルの一部を消せませんでした。下の詳細に書かれたファイルやフォルダを手で削除してください(セーブデータ sfd1〜sfd5 は消さないでください)。"
    }
    if ($Message -match "1996 disc|not a 1996 edition disc|unexpected .* on the 1996 disc") {
        return "96版のディスクとして想定しているファイルと違います。別の版のディスクか、読み取りに失敗した可能性があります。ディスクの汚れを確認して、もう一度実行してください。"
    }
    if ($Message -match "destination already contains|destination is on the disc|destination is not a folder") {
        return "コピー先のフォルダに、ゲームのファイルと同じ名前の別のファイルやフォルダがあります。空のフォルダか、存在しないフォルダをコピー先に指定してください(-GameDir で指定できます)。"
    }
    if ($Message -match "copy verification failed|expanded .* has the wrong hash|written .* has the wrong hash|SZDD") {
        return "ディスクからのコピー、または QuickTime の取り出しで内容が一致しませんでした。ディスクの汚れを確認して、もう一度実行してください。"
    }
    if ($Message -match "QTIM32R\.DLL|CMGR32R\.DLL|not a recognized stock or proxy layout") {
        return "ゲームフォルダの QTIM32.DLL / CMGR32.DLL が元のファイルではありません。パッチのファイルをゲームフォルダへ直接コピーして上書きした可能性があります。ゲームを再インストールする(または CD から QTIM32.DLL と CMGR32.DLL を戻す)と直ります。"
    }
    if ($Message -match "neither the expected original nor a known patched build|not the expected original") {
        return "gundam.exe が想定しているファイルと違います。別のバージョンのゲーム、別のパッチを当てたもの、または壊れたファイルの可能性があります。ゲームを再インストールしてから、もう一度実行してください。"
    }
    if ($Message -match "backup not found|no verified original backup") {
        return "このフォルダには、このパッチで保存した元のファイル(gundam.exe.orig)がありません。パッチを当てていないか、フォルダが違う可能性があります。"
    }
    if ($Message -match "being used by another process|used by another process") {
        return "ゲームが起動したままになっています。ゲームを終了してから、もう一度実行してください。"
    }
    if ($Message -match "denied|UnauthorizedAccess") {
        return "フォルダへの書き込みが許可されていません。管理者として実行するか、ゲームを C:\GundamTactics のような書き込めるフォルダへ移してください。"
    }
    return "予期しないエラーです。下の詳細メッセージを添えて作者へお知らせください。"
}

function Wait-BeforeClose {
    if ($Elevated -and -not $Yes) { [void](Read-Host "Enter キーを押すとこのウィンドウを閉じます") }
}

$exitCode = 1
$import = $null
$createdDestination = $false
$dir = ""
try {
    Write-Title "ガンダムタクティクス Windows 11 互換パッチ v1.2.0: $action"

    $dir = Resolve-GameFolder
    if (-not $dir) { throw [OperationCanceledException]::new("フォルダが選ばれませんでした。") }
    if ($script:discSource) {
        if (-not (Test-Disc96 $script:discSource)) {
            throw [IO.DirectoryNotFoundException]::new("選ばれたフォルダは 96版のディスクではありません(gundam.exe と qt32.exe が必要です): $($script:discSource)")
        }
        $script:discSource = (Resolve-Path -LiteralPath $script:discSource).Path
        if (-not $script:destinationGiven -and -not $Yes -and -not $Elevated) {
            Write-Host "96版のディスクが見つかりました: $($script:discSource)"
            Write-Host "ゲームをコピーする場所を選ぶ画面を開きます。"
            while ($true) {
                $picked = Select-Destination96Dialog
                if (-not $picked) { break }
                $candidate = Resolve-Destination96 $picked
                $refusal = Get-Destination96Refusal $candidate
                if (-not $refusal) { $dir = $candidate; break }
                Write-Host $refusal -ForegroundColor Yellow
            }
        }
        $dir = [IO.Path]::GetFullPath($dir)
        $refusal = Get-Destination96Refusal $dir
        if ($refusal) { throw [IO.DirectoryNotFoundException]::new($refusal) }
        if (Test-GameFolder $dir) {
            # 以前にコピー済み: コピーは省いてパッチだけ当てる
            Write-Host "コピー先にはすでにゲームがあるので、ディスクからのコピーは行いません。"
            $script:discSource = ""
        }
    }
    if (-not $script:discSource) {
        if (-not (Test-GameFolder $dir)) {
            $need = if ($isInstall) { "gundam.exe" } else { "gundam.exe.orig(このパッチを当てた印)" }
            throw [IO.DirectoryNotFoundException]::new("選ばれたフォルダに $need がありません: $dir")
        }
        $dir = (Resolve-Path -LiteralPath $dir).Path
        Write-Host "対象フォルダ: $dir"
    } else {
        Write-Host "96版のディスク: $($script:discSource)"
        Write-Host "コピー先のフォルダ: $dir"
        Write-Host "(96版はディスクから起動する方式なので、ゲームをこのフォルダへコピーしてからパッチを当てます。"
        Write-Host " QuickTime はディスクの qt32.exe から取り出してゲームのフォルダに置きます。Windows には何もインストールしません)"
    }

    $running = @(Get-Process -Name gundam -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw [InvalidOperationException]::new("ゲームが起動しています。ゲームを終了してから、もう一度実行してください。")
    }

    if (-not $Yes -and -not $Elevated) {
        $question = if ($script:discSource) { "ゲームをコピーして$($action)を行います。よろしいですか? (Y/N)" } else { "このフォルダに$($action)を行います。よろしいですか? (Y/N)" }
        $answer = Read-Host $question
        if ($answer -notmatch '^\s*[yYｙＹ]') { throw [OperationCanceledException]::new("中止しました。何も変更していません。") }
    }

    $writable = $true
    if ($script:discSource -and -not (Test-Path -LiteralPath $dir)) {
        try {
            [void][IO.Directory]::CreateDirectory($dir)
            $createdDestination = $true
        } catch [UnauthorizedAccessException] {
            $writable = $false
        }
    }
    if ($writable) { $writable = Test-WriteAccess $dir }
    if (-not $writable) {
        $left = @(Undo-Import96 $null $createdDestination $dir)
        if ($left.Count -gt 0) { throw [UnauthorizedAccessException]::new("cannot remove the folder created for the copy: $($left -join ', ')") }
        $createdDestination = $false
        if ($Elevated -or (Test-Administrator)) {
            throw [UnauthorizedAccessException]::new("Access to the game folder is denied even as administrator: $dir")
        }
        Write-Host "このフォルダへの書き込みには管理者の許可が必要です。表示される確認画面で「はい」を選んでください。" -ForegroundColor Yellow
        $arguments = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (ConvertTo-QuotedArgument $PSCommandPath),
            "-Mode", $Mode,
            "-GameDir", (ConvertTo-QuotedArgument $dir)
        )
        if ($script:discSource) { $arguments += @("-DiscDir", (ConvertTo-QuotedArgument $script:discSource)) }
        $arguments = ($arguments + "-Elevated") -join " "
        try {
            $process = Start-Process -FilePath "powershell.exe" -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        } catch {
            throw [OperationCanceledException]::new("管理者の許可が得られなかったため中止しました。何も変更していません。")
        }
        $exitCode = $process.ExitCode
        if ($exitCode -eq 0) { Write-Host "管理者のウィンドウで$($action)が完了しました。" -ForegroundColor Green }
        else { Write-Host "管理者のウィンドウで$($action)に失敗しました。そちらに表示された内容を確認してください。" -ForegroundColor Red }
        exit $exitCode
    }

    if ($isInstall) {
        if ($script:discSource) {
            $import = @(& (Join-Path $PSScriptRoot "import96.ps1") -DiscDir $script:discSource -Destination $dir)[-1]
        }
        & (Join-Path $PSScriptRoot "apply.ps1") -InstallPath $dir
    }
    else { & (Join-Path $PSScriptRoot "revert.ps1") -InstallPath $dir }

    Write-Host ""
    if ($isInstall) {
        Write-Host "完了しました。ゲームフォルダの gundam.exe をダブルクリックして起動してください。" -ForegroundColor Green
        Write-Host "(画面の色数の変更や互換モードの設定は不要です)"
        if ($import) {
            Write-Host "ゲームのフォルダ: $dir"
            Write-Host "96版のスタートメニューのショートカットはディスクの gundam.exe を指しているので使わないでください。"
            Write-Host "起動にディスクは不要です。"
        }
    } else {
        Write-Host "完了しました。ゲームはパッチを当てる前の状態に戻りました。" -ForegroundColor Green
    }
    $exitCode = 0
} catch [OperationCanceledException] {
    $left = @(Undo-Import96 $import $createdDestination $dir)
    Write-Host $_.Exception.Message -ForegroundColor Yellow
    if ($left.Count -gt 0) { Write-Host "コピーしたファイルの一部を消せませんでした。手で削除してください: $($left -join ', ')" -ForegroundColor Red }
    $exitCode = 2
} catch {
    $message = $_.Exception.Message
    $left = @(Undo-Import96 $import $createdDestination $dir)
    Write-Host ""
    if ($left.Count -gt 0 -or $message -match 'rollback incomplete') {
        Write-Host "$($action)に失敗しました。コピーしたファイルの一部を消せませんでした。" -ForegroundColor Red
        if ($left.Count -gt 0) { Write-Host "手で削除してください: $($left -join ', ')" -ForegroundColor Red }
    } else {
        Write-Host "$($action)に失敗しました。ゲームのファイルは実行前の状態のままです。" -ForegroundColor Red
    }
    if ($_.Exception -is [IO.DirectoryNotFoundException] -or $_.Exception -is [InvalidOperationException]) {
        Write-Host $message -ForegroundColor Yellow
    } else {
        Write-Host (Get-FriendlyHint $message) -ForegroundColor Yellow
        Write-Host ""
        Write-Host "詳細: $message"
    }
    $exitCode = 1
}
Wait-BeforeClose
exit $exitCode
