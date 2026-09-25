# v1.0.16 - install.bat / uninstall.bat から呼ばれる対話インストーラー。
# 実際のファイル操作は apply.ps1 / revert.ps1 が行う。
param(
    [ValidateSet("install", "uninstall")][string]$Mode = "install",
    [string]$GameDir = "",
    [switch]$Yes,
    [switch]$Elevated
)

$ErrorActionPreference = "Stop"
$packageRoot = Split-Path -Parent $PSScriptRoot
$isInstall = $Mode -eq "install"
$action = if ($isInstall) { "パッチの適用" } else { "パッチの解除(元に戻す)" }

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

function Resolve-GameFolder {
    if ($GameDir) {
        $candidate = ConvertTo-CleanPath $GameDir
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $candidate = Split-Path -Parent $candidate }
        return $candidate
    }
    # ZIP の中身をゲームフォルダ(またはその中のサブフォルダ)に展開した場合
    foreach ($candidate in @($packageRoot, (Split-Path -Parent $packageRoot))) {
        if (Test-GameFolder $candidate) { return $candidate }
    }
    # 元のインストーラーの既定フォルダ
    if (Test-GameFolder "C:\G-TACT") { return "C:\G-TACT" }
    Write-Host "ゲームのフォルダを自動で見つけられませんでした。フォルダ選択の画面から選んでください。"
    return Select-GameFolderDialog
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
        return "ゲームのフォルダのパスが長すぎます。C:\G-TACT のような短いフォルダへゲームのフォルダごと移動してから、もう一度実行してください。"
    }
    if ($Message -match "cannot represent") {
        return "フォルダ名に、このパッチで扱えない文字(韓国語・絵文字など)が含まれています。英数字か日本語だけの名前のフォルダへ移動してから、もう一度実行してください。"
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
        return "フォルダへの書き込みが許可されていません。管理者として実行するか、ゲームを C:\G-TACT のような書き込めるフォルダへ移してください。"
    }
    return "予期しないエラーです。下の詳細メッセージを添えて作者へお知らせください。"
}

function Wait-BeforeClose {
    if ($Elevated -and -not $Yes) { [void](Read-Host "Enter キーを押すとこのウィンドウを閉じます") }
}

$exitCode = 1
try {
    Write-Title "ガンダムタクティクス Windows 11 互換パッチ v1.0.16: $action"

    $dir = Resolve-GameFolder
    if (-not $dir) { throw [OperationCanceledException]::new("フォルダが選ばれませんでした。") }
    if (-not (Test-GameFolder $dir)) {
        $need = if ($isInstall) { "gundam.exe" } else { "gundam.exe.orig(このパッチを当てた印)" }
        throw [IO.DirectoryNotFoundException]::new("選ばれたフォルダに $need がありません: $dir")
    }
    $dir = (Resolve-Path -LiteralPath $dir).Path
    Write-Host "対象フォルダ: $dir"

    $running = @(Get-Process -Name gundam -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw [InvalidOperationException]::new("ゲームが起動しています。ゲームを終了してから、もう一度実行してください。")
    }

    if (-not $Yes -and -not $Elevated) {
        $answer = Read-Host "このフォルダに$($action)を行います。よろしいですか? (Y/N)"
        if ($answer -notmatch '^\s*[yYｙＹ]') { throw [OperationCanceledException]::new("中止しました。何も変更していません。") }
    }

    if (-not (Test-WriteAccess $dir)) {
        if ($Elevated -or (Test-Administrator)) {
            throw [UnauthorizedAccessException]::new("Access to the game folder is denied even as administrator: $dir")
        }
        Write-Host "このフォルダへの書き込みには管理者の許可が必要です。表示される確認画面で「はい」を選んでください。" -ForegroundColor Yellow
        $arguments = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (ConvertTo-QuotedArgument $PSCommandPath),
            "-Mode", $Mode,
            "-GameDir", (ConvertTo-QuotedArgument $dir),
            "-Elevated"
        ) -join " "
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

    if ($isInstall) { & (Join-Path $PSScriptRoot "apply.ps1") -InstallPath $dir }
    else { & (Join-Path $PSScriptRoot "revert.ps1") -InstallPath $dir }

    Write-Host ""
    if ($isInstall) {
        Write-Host "完了しました。ゲームフォルダの gundam.exe をダブルクリックして起動してください。" -ForegroundColor Green
        Write-Host "(画面の色数の変更や互換モードの設定は不要です)"
    } else {
        Write-Host "完了しました。ゲームはパッチを当てる前の状態に戻りました。" -ForegroundColor Green
    }
    $exitCode = 0
} catch [OperationCanceledException] {
    Write-Host $_.Exception.Message -ForegroundColor Yellow
    $exitCode = 2
} catch {
    $message = $_.Exception.Message
    Write-Host ""
    Write-Host "$($action)に失敗しました。ゲームのファイルは実行前の状態のままです。" -ForegroundColor Red
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
