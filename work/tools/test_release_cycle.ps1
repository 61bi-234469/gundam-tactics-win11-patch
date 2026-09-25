param(
    [string]$ReleaseZip = "",
    [string]$PackageDirectory = "",
    [string]$TestDirectory = "",
    [switch]$KeepTestDirectory
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceDir = Join-Path $repoRoot "source_exe"
$runRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "run"))
$releaseDir = Join-Path $repoRoot "release\GundamTactics_Win11Patch"
$checksumEntries = @("install.bat", "uninstall.bat", "README.txt", "CHANGELOG.txt", "patch_files/install.ps1", "patch_files/apply.ps1", "patch_files/revert.ps1", "patch_files/QTIM32.dll", "patch_files/CMGR32.dll")
$expectedEntries = @($checksumEntries + "checksums.txt")
$originalSha256 = "38bde2e4513c665d1425fd00203d0000001c5b81bc37899507b6ef7129f238d3"
$patchedSha256 = "607299a2cd7d5aeb1375cd838cd343cd81f9289311cea659d100c700d4035ec4"
$phase4PatchedSha256 = "c693d60b73cbba731dbacbea255e845e97a0dae95cc80812316a5795e4f15942"
$legacyPatchedSha256 = "82c92402ac9c9282992d3c343dbb7c62463a3a5e58569ff845670bf166b867d4"
$runtimeSha256 = "dbbe7e208955c0173d2a41a8873d1ccdacdca96e42948f861768e1dde3afc77f"
$cmgrRuntimeSha256 = "9fc00aece0c9db38b7e7001d261a060567eb035a343f4d833e58fd163e80f9e4"

if ($ReleaseZip -and $PackageDirectory) { throw "use either -ReleaseZip or -PackageDirectory, not both" }
if (-not $TestDirectory) { $TestDirectory = Join-Path $runRoot "rt" }
$testRoot = [IO.Path]::GetFullPath($TestDirectory)
if (-not $testRoot.StartsWith($runRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "test directory must be below run: $testRoot"
}
$shortTestRoot = $null
if (-not (Test-Path -LiteralPath $sourceDir)) { throw "source_exe not found: $sourceDir" }

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-ShortPath([string]$Path) {
    if (-not ([System.Management.Automation.PSTypeName]'GundamTactics.ShortPath'.Type)) {
        Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
namespace GundamTactics {
    public static class ShortPath {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetShortPathName(string longPath, StringBuilder shortPath, uint bufferLength);
        public static string Get(string path) {
            var buffer = new StringBuilder(32768);
            uint length = GetShortPathName(path, buffer, (uint)buffer.Capacity);
            if (length == 0) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return buffer.ToString();
        }
    }
}
"@
    }
    return [GundamTactics.ShortPath]::Get($Path)
}

function Assert-Hash([string]$Path, [string]$Expected, [string]$Label) {
    $actual = Get-Sha256 $Path
    if ($actual -ne $Expected.ToLowerInvariant()) { throw "$Label hash mismatch: $actual; expected $Expected" }
    Write-Host "$Label sha256=$actual"
}

function Assert-NotExists([string]$Path, [string]$Label) {
    if (Test-Path -LiteralPath $Path) { throw "$Label unexpectedly exists: $Path" }
}

function Assert-Exists([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "$Label is missing: $Path" }
}

function Get-LayerValue([string]$ExePath) {
    if ($env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE) {
        $path = $env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE
        if (-not (Test-Path -LiteralPath $path)) { return $null }
        $document = ([IO.File]::ReadAllText($path) | ConvertFrom-Json)
        $property = if ($document.values) { $document.values.PSObject.Properties[$ExePath] } else { $null }
        if ($null -ne $property) { return [string]$property.Value }
        return $null
    }
    $key = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
    if (-not (Test-Path -LiteralPath $key)) { return $null }
    try {
        $property = Get-ItemProperty -LiteralPath $key -Name $ExePath -ErrorAction Stop
        return [string]$property.$ExePath
    } catch [System.Management.Automation.PSArgumentException] {
        return $null
    }
}

function Set-LayerValue([string]$ExePath, [string]$Value) {
    if ($env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE) {
        $path = $env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE
        $document = if (Test-Path -LiteralPath $path) {
            ([IO.File]::ReadAllText($path) | ConvertFrom-Json)
        } else { [pscustomobject]@{ keyExists = $false; values = [pscustomobject]@{} } }
        if (-not $document.values) { $document | Add-Member NoteProperty values ([pscustomobject]@{}) }
        $property = $document.values.PSObject.Properties[$ExePath]
        if ($null -eq $property) {
            $document.values | Add-Member NoteProperty -Name $ExePath -Value $Value
        } else { $property.Value = $Value }
        $document.keyExists = $true
        [IO.File]::WriteAllText($path, ($document | ConvertTo-Json -Depth 5 -Compress) + "`r`n")
        return
    }
    $key = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name $ExePath -Value $Value -PropertyType String -Force | Out-Null
}

function Remove-LayerValue([string]$ExePath) {
    if ($env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE) {
        $path = $env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE
        if (-not (Test-Path -LiteralPath $path)) { return }
        $document = ([IO.File]::ReadAllText($path) | ConvertFrom-Json)
        if ($document.values) { $document.values.PSObject.Properties.Remove($ExePath) }
        if (-not $document.values -or $document.values.PSObject.Properties.Count -eq 0) {
            Remove-Item -LiteralPath $path -Force
        } else {
            [IO.File]::WriteAllText($path, ($document | ConvertTo-Json -Depth 5 -Compress) + "`r`n")
        }
        return
    }
    $key = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
    if (Test-Path -LiteralPath $key) {
        Remove-ItemProperty -LiteralPath $key -Name $ExePath -ErrorAction SilentlyContinue
    }
}

function Assert-LayerValue([string]$ExePath, $Expected, [string]$Label) {
    $actual = Get-LayerValue $ExePath
    if ($null -eq $Expected) {
        if ($null -ne $actual -and $actual -ne "") { throw "$Label AppCompat value mismatch: '$actual'; expected absent" }
    } elseif ($actual -ne $Expected) {
        throw "$Label AppCompat value mismatch: '$actual'; expected '$Expected'"
    }
}

function New-TestCopy([string]$Name) {
    $dir = Join-Path $script:shortTestRoot $Name
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Get-ChildItem -LiteralPath $sourceDir -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $dir -Recurse -Force
    }
    return $dir
}

function Invoke-Apply([string]$Dir, [switch]$Register, [switch]$RunAsAdmin) {
    if ($RunAsAdmin) {
        . $script:applyScript -InstallPath $Dir -RunAsAdmin
    } elseif ($Register) {
        . $script:applyScript -InstallPath $Dir -RegisterAppCompat
    } else {
        . $script:applyScript -InstallPath $Dir
    }
}

function Invoke-Revert([string]$Dir) {
    . $script:revertScript -InstallPath $Dir
}

function Assert-CleanRevert([string]$Dir, [string]$Label) {
    Assert-Hash (Join-Path $Dir "gundam.exe") $originalSha256 "$Label gundam.exe"
    Assert-Hash (Join-Path $Dir "QTIM32.DLL") $runtimeSha256 "$Label QTIM32.DLL"
    Assert-Hash (Join-Path $Dir "CMGR32.DLL") $cmgrRuntimeSha256 "$Label CMGR32.DLL"
    Assert-NotExists (Join-Path $Dir "QTIM32R.DLL") "$Label QTIM32R.DLL"
    Assert-NotExists (Join-Path $Dir "CMGR32R.DLL") "$Label CMGR32R.DLL"
    Assert-NotExists (Join-Path $Dir "gundam.exe.orig") "$Label gundam.exe.orig"
    Assert-NotExists (Join-Path $Dir "gundam_win11patch.state.json") "$Label AppCompat sidecar"
}

$archiveExtracted = $null
$testPackageDir = $null
$testDirCreated = $false

try {
    if ($ReleaseZip) {
        $zipPath = [IO.Path]::GetFullPath($ReleaseZip)
        if (-not (Test-Path -LiteralPath $zipPath)) { throw "release ZIP not found: $zipPath" }
        $archiveExtracted = Join-Path $runRoot ("release_test_package_{0}_{1}" -f $PID, ([guid]::NewGuid().ToString("N")))
        New-Item -ItemType Directory -Path $archiveExtracted -Force | Out-Null
        $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            $entryNames = @($archive.Entries | ForEach-Object { $_.FullName })
            if (@(Compare-Object $expectedEntries $entryNames).Count -ne 0) {
                throw "release ZIP entry allowlist mismatch: $($entryNames -join ', ')"
            }
        } finally { $archive.Dispose() }
        [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $archiveExtracted)
        $testPackageDir = $archiveExtracted
    } elseif ($PackageDirectory) {
        $testPackageDir = [IO.Path]::GetFullPath($PackageDirectory)
    } else {
        $testPackageDir = $releaseDir
    }

    foreach ($entry in $expectedEntries) { Assert-Exists (Join-Path $testPackageDir $entry) "package $entry" }
    $packagePrefix = [IO.Path]::GetFullPath($testPackageDir).TrimEnd('\').Length + 1
    $actualEntries = @(Get-ChildItem -LiteralPath $testPackageDir -Recurse -File -Force |
        ForEach-Object { $_.FullName.Substring($packagePrefix).Replace('\', '/') })
    if (@(Compare-Object $expectedEntries $actualEntries).Count -ne 0) {
        throw "release directory entry allowlist mismatch: $($actualEntries -join ', ')"
    }

    $checksums = @{}
    Get-Content -LiteralPath (Join-Path $testPackageDir "checksums.txt") | ForEach-Object {
        if ($_ -match '^([0-9A-Fa-f]{64})\s{2}(.+)$') { $checksums[$Matches[2]] = $Matches[1].ToLowerInvariant() }
    }
    foreach ($entry in $checksumEntries) {
        if (-not $checksums.ContainsKey($entry)) { throw "checksums.txt is missing $entry" }
        Assert-Hash (Join-Path $testPackageDir $entry) $checksums[$entry] "package $entry"
    }
    $proxySha256 = $checksums["patch_files/QTIM32.dll"]
    $cmgrProxySha256 = $checksums["patch_files/CMGR32.dll"]
    $script:applyScript = Join-Path $testPackageDir "patch_files\apply.ps1"
    $script:revertScript = Join-Path $testPackageDir "patch_files\revert.ps1"
    $script:installScript = Join-Path $testPackageDir "patch_files\install.ps1"

    # Keep the clean-copy transactions on the physical 8.3 short path as in
    # v1.0.12; the long spelling is the long-path acceptance target.
    $shortRunRoot = Get-ShortPath $runRoot
    $testRelative = $testRoot.Substring($runRoot.Length).TrimStart([char[]]@('\', '/'))
    $shortTestRoot = Join-Path $shortRunRoot $testRelative

    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $shortTestRoot -Force | Out-Null
    $testDirCreated = $true
    $env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE = Join-Path $shortTestRoot "appcompat_registry.json"

    Write-Host "--- clean cycle: apply -> revert -> apply -> revert ---"
    $cycle = New-TestCopy "cycle"
    $cycleExe = Join-Path $cycle "gundam.exe"
    Assert-Hash $cycleExe $originalSha256 "cycle initial gundam.exe"
    Assert-Hash (Join-Path $cycle "QTIM32.DLL") $runtimeSha256 "cycle initial QTIM32.DLL"
    Assert-Hash (Join-Path $cycle "CMGR32.DLL") $cmgrRuntimeSha256 "cycle initial CMGR32.DLL"
    Invoke-Apply $cycle
    Assert-Hash $cycleExe $patchedSha256 "cycle first patched gundam.exe"
    Assert-Hash (Join-Path $cycle "QTIM32.DLL") $proxySha256 "cycle first proxy QTIM32.DLL"
    Assert-Hash (Join-Path $cycle "CMGR32.DLL") $cmgrProxySha256 "cycle first proxy CMGR32.DLL"
    Assert-Hash (Join-Path $cycle "QTIM32R.DLL") $runtimeSha256 "cycle first QTIM32R.DLL"
    Assert-Hash (Join-Path $cycle "CMGR32R.DLL") $cmgrRuntimeSha256 "cycle first CMGR32R.DLL"
    Assert-Hash (Join-Path $cycle "gundam.exe.orig") $originalSha256 "cycle first gundam.exe.orig"
    Assert-NotExists (Join-Path $cycle "gundam_win11patch.state.json") "cycle sidecar without AppCompat"
    Invoke-Revert $cycle
    Assert-CleanRevert $cycle "cycle first revert"
    Invoke-Apply $cycle
    Assert-Hash $cycleExe $patchedSha256 "cycle second patched gundam.exe"
    Invoke-Revert $cycle
    Assert-CleanRevert $cycle "cycle final revert"

    Write-Host "--- v1.0.15 install (Phase 4 executable) upgrades from gundam.exe.orig ---"
    $upgrade15 = New-TestCopy "upgrade15"
    $upgrade15Exe = Join-Path $upgrade15 "gundam.exe"
    Invoke-Apply $upgrade15
    [byte[]]$phase4Bytes = [IO.File]::ReadAllBytes($upgrade15Exe)
    [byte[]]$stretchCall = @(0xff,0x15,0x78,0xc2,0x44,0x00)
    [Array]::Copy($stretchCall, 0, $phase4Bytes, 0x678C, $stretchCall.Length)
    [Array]::Clear($phase4Bytes, 0x37980, 76)
    [IO.File]::WriteAllBytes($upgrade15Exe, $phase4Bytes)
    Assert-Hash $upgrade15Exe $phase4PatchedSha256 "upgrade15 v1.0.15 gundam.exe"
    Invoke-Apply $upgrade15
    Assert-Hash $upgrade15Exe $patchedSha256 "upgrade15 patched gundam.exe"
    Assert-Hash (Join-Path $upgrade15 "gundam.exe.orig") $originalSha256 "upgrade15 gundam.exe.orig"
    Invoke-Revert $upgrade15
    Assert-CleanRevert $upgrade15 "upgrade15 final revert"

    Write-Host "--- v1.0.12 install (legacy executable) upgrades from gundam.exe.orig ---"
    $upgrade = New-TestCopy "upgrade"
    $upgradeExe = Join-Path $upgrade "gundam.exe"
    Invoke-Apply $upgrade
    [byte[]]$legacyBytes = [IO.File]::ReadAllBytes($upgradeExe)
    [Array]::Copy($stretchCall, 0, $legacyBytes, 0x678C, $stretchCall.Length)
    [Array]::Clear($legacyBytes, 0x37980, 76)
    [byte[]]$getcwdWrapper = @(0x8b,0x44,0x24,0x08,0x8b,0x4c,0x24,0x04,0x50,0x51,0x6a,0x00,0xe8,0x0f,0x00,0x00,0x00,0x83,0xc4,0x0c,0xc3)
    [Array]::Copy($getcwdWrapper, 0, $legacyBytes, 0x30360, $getcwdWrapper.Length)
    [IO.File]::WriteAllBytes($upgradeExe, $legacyBytes)
    Assert-Hash $upgradeExe $legacyPatchedSha256 "upgrade legacy gundam.exe"
    Invoke-Apply $upgrade
    Assert-Hash $upgradeExe $patchedSha256 "upgrade patched gundam.exe"
    Assert-Hash (Join-Path $upgrade "gundam.exe.orig") $originalSha256 "upgrade gundam.exe.orig"
    Invoke-Revert $upgrade
    Assert-CleanRevert $upgrade "upgrade final revert"

    Write-Host "--- v1.0.14 QTIM32 proxy upgrades in place; revert removes MidiLoop copies ---"
    $previousZip = Join-Path $repoRoot "release\GundamTactics_Win11Patch_v1.0.14.zip"
    if (Test-Path -LiteralPath $previousZip) {
        $proxyUpgrade = New-TestCopy "proxyupgrade"
        Invoke-Apply $proxyUpgrade
        $previousArchive = [IO.Compression.ZipFile]::OpenRead($previousZip)
        try {
            $entry = $previousArchive.GetEntry("patch_files/QTIM32.dll")
            if (-not $entry) { throw "v1.0.14 ZIP has no patch_files/QTIM32.dll" }
            $stream = $entry.Open()
            try {
                $target = [IO.File]::Create((Join-Path $proxyUpgrade "QTIM32.DLL"))
                try { $stream.CopyTo($target) } finally { $target.Dispose() }
            } finally { $stream.Dispose() }
        } finally { $previousArchive.Dispose() }
        Assert-Hash (Join-Path $proxyUpgrade "QTIM32.DLL") "3925975d279d553fe3418e43b2bffce2c401d3eccf46ec6bdf1fb75ea3fdfd6c" "proxyupgrade v1.0.14 QTIM32.DLL"
        Invoke-Apply $proxyUpgrade
        Assert-Hash (Join-Path $proxyUpgrade "QTIM32.DLL") $proxySha256 "proxyupgrade upgraded QTIM32.DLL"
        Assert-Hash (Join-Path $proxyUpgrade "QTIM32R.DLL") $runtimeSha256 "proxyupgrade QTIM32R.DLL"
        $midiLoop = Join-Path $proxyUpgrade "MidiLoop"
        New-Item -ItemType Directory -Path $midiLoop -Force | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $midiLoop "M02gm.mid"), [byte[]](0x4d, 0x54, 0x68, 0x64))
        Invoke-Revert $proxyUpgrade
        Assert-CleanRevert $proxyUpgrade "proxyupgrade final revert"
        Assert-NotExists $midiLoop "proxyupgrade MidiLoop"
    } else {
        Write-Host "SKIP: $previousZip not found"
    }

    Write-Host "--- install.ps1: quoted path with a trailing backslash arrives with a stray quote ---"
    $wizard = New-TestCopy "wizard"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script:installScript -Mode install -GameDir ($wizard + '\"') -Yes
    if ($LASTEXITCODE -ne 0) { throw "install.ps1 failed ($LASTEXITCODE)" }
    Assert-Hash (Join-Path $wizard "gundam.exe") $patchedSha256 "wizard patched gundam.exe"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script:installScript -Mode uninstall -GameDir $wizard -Yes
    if ($LASTEXITCODE -ne 0) { throw "install.ps1 -Mode uninstall failed ($LASTEXITCODE)" }
    Assert-CleanRevert $wizard "wizard final revert"

    Write-Host "--- install.ps1: package extracted inside the game folder is auto-detected ---"
    foreach ($layout in @("flat", "subfolder")) {
        $inside = New-TestCopy "inside_$layout"
        $insidePackage = if ($layout -eq "flat") { $inside } else { Join-Path $inside "GundamTactics_Win11Patch" }
        New-Item -ItemType Directory -Path $insidePackage -Force | Out-Null
        Get-ChildItem -LiteralPath $testPackageDir -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $insidePackage -Recurse -Force
        }
        Assert-Hash (Join-Path $inside "QTIM32.DLL") $runtimeSha256 "inside $layout stock QTIM32.DLL kept"
        Assert-Hash (Join-Path $inside "CMGR32.DLL") $cmgrRuntimeSha256 "inside $layout stock CMGR32.DLL kept"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $insidePackage "patch_files\install.ps1") -Mode install -Yes
        if ($LASTEXITCODE -ne 0) { throw "install.ps1 inside $layout failed ($LASTEXITCODE)" }
        Assert-Hash (Join-Path $inside "gundam.exe") $patchedSha256 "inside $layout patched gundam.exe"
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $insidePackage "patch_files\install.ps1") -Mode uninstall -Yes
        if ($LASTEXITCODE -ne 0) { throw "install.ps1 uninstall inside $layout failed ($LASTEXITCODE)" }
        Assert-CleanRevert $inside "inside $layout final revert"
    }

    Write-Host "--- install.ps1: stock DLLs overwritten by a v1.0.13-style flat copy are refused ---"
    $clobbered = New-TestCopy "clobbered"
    Copy-Item -LiteralPath (Join-Path $testPackageDir "patch_files\QTIM32.dll") -Destination (Join-Path $clobbered "QTIM32.DLL") -Force
    Copy-Item -LiteralPath (Join-Path $testPackageDir "patch_files\CMGR32.dll") -Destination (Join-Path $clobbered "CMGR32.DLL") -Force
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script:installScript -Mode install -GameDir $clobbered -Yes
    if ($LASTEXITCODE -ne 1) { throw "install.ps1 accepted overwritten stock DLLs ($LASTEXITCODE)" }
    Assert-Hash (Join-Path $clobbered "gundam.exe") $originalSha256 "clobbered gundam.exe untouched"
    Assert-NotExists (Join-Path $clobbered "gundam.exe.orig") "clobbered executable backup"

    Write-Host "--- long physical path (over the old 54-byte limit) is accepted ---"
    $null = New-TestCopy "path_long"
    $pathLong = Join-Path $testRoot "path_long"
    Invoke-Apply $pathLong
    Assert-Hash (Join-Path $pathLong "gundam.exe") $patchedSha256 "long path patched gundam.exe"
    Invoke-Revert $pathLong
    Assert-CleanRevert $pathLong "long path final revert"

    if ([Text.Encoding]::Default.CodePage -eq 932) {
        Write-Host "--- path over the 256-byte full-path limit is refused before any mutation ---"
        # 100 double-byte characters: over 256 ANSI/MBCS bytes, under MAX_PATH characters.
        $pathRefused = Join-Path $testRoot ([string]::new([char]0x30AC, 100))
        New-Item -ItemType Directory -Path $pathRefused -Force | Out-Null
        Get-ChildItem -LiteralPath $sourceDir -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $pathRefused -Recurse -Force
        }
        $pathRejected = $false
        try { Invoke-Apply $pathRefused } catch { $pathRejected = $true; Write-Host "expected path refusal: $($_.Exception.Message)" }
        if (-not $pathRejected) { throw "apply unexpectedly accepted an overlong install path" }
        Assert-Hash (Join-Path $pathRefused "gundam.exe") $originalSha256 "path refusal gundam.exe"
        Assert-Hash (Join-Path $pathRefused "QTIM32.DLL") $runtimeSha256 "path refusal QTIM32.DLL"
        Assert-Hash (Join-Path $pathRefused "CMGR32.DLL") $cmgrRuntimeSha256 "path refusal CMGR32.DLL"
        Assert-NotExists (Join-Path $pathRefused "gundam.exe.orig") "path refusal executable backup"
    } else {
        Write-Host "--- skipped 256-byte refusal test: ANSI code page is not 932 ---"
    }

    $nonAnsiName = [string]::new([char]0xD55C, 2)
    $ansi = [Text.Encoding]::Default
    if ($ansi.GetString($ansi.GetBytes($nonAnsiName)) -cne $nonAnsiName) {
        Write-Host "--- path outside the ANSI code page is refused before any mutation ---"
        $pathNonAnsi = Join-Path $testRoot $nonAnsiName
        New-Item -ItemType Directory -Path $pathNonAnsi -Force | Out-Null
        Get-ChildItem -LiteralPath $sourceDir -Force | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $pathNonAnsi -Recurse -Force
        }
        $nonAnsiRejected = $false
        try { Invoke-Apply $pathNonAnsi } catch { $nonAnsiRejected = $true; Write-Host "expected non-ANSI refusal: $($_.Exception.Message)" }
        if (-not $nonAnsiRejected) { throw "apply unexpectedly accepted a path outside the ANSI code page" }
        Assert-Hash (Join-Path $pathNonAnsi "gundam.exe") $originalSha256 "non-ANSI refusal gundam.exe"
        Assert-NotExists (Join-Path $pathNonAnsi "gundam.exe.orig") "non-ANSI refusal executable backup"
    } else {
        Write-Host "--- skipped non-ANSI refusal test: test characters are in the ANSI code page ---"
    }

    Write-Host "--- AppCompat absent: sidecar records absence and revert removes package value ---"
    $compatAbsent = New-TestCopy "compat_absent"
    $compatAbsentExe = Join-Path $compatAbsent "gundam.exe"
    Remove-LayerValue $compatAbsentExe
    Invoke-Apply $compatAbsent -Register
    Assert-LayerValue $compatAbsentExe "~ WIN95" "AppCompat absent after apply"
    $absentState = Get-Content -Raw (Join-Path $compatAbsent "gundam_win11patch.state.json") | ConvertFrom-Json
    if ([bool]$absentState.before.hasValue) { throw "AppCompat absent case sidecar unexpectedly has a value" }
    Invoke-Revert $compatAbsent
    Assert-LayerValue $compatAbsentExe $null "AppCompat absent after revert"
    Assert-CleanRevert $compatAbsent "AppCompat absent final"

    Write-Host "--- AppCompat existing package value: sidecar restores the previous value ---"
    $compatExisting = New-TestCopy "compat_existing"
    $compatExistingExe = Join-Path $compatExisting "gundam.exe"
    Remove-LayerValue $compatExistingExe
    Set-LayerValue $compatExistingExe "~ WIN95 RUNASADMIN"
    Invoke-Apply $compatExisting -Register
    Assert-LayerValue $compatExistingExe "~ WIN95" "AppCompat existing after apply"
    $existingState = Get-Content -Raw (Join-Path $compatExisting "gundam_win11patch.state.json") | ConvertFrom-Json
    if (-not [bool]$existingState.before.hasValue -or [string]$existingState.before.value -ne "~ WIN95 RUNASADMIN") {
        throw "AppCompat existing case did not record its previous value"
    }
    Invoke-Revert $compatExisting
    Assert-LayerValue $compatExistingExe "~ WIN95 RUNASADMIN" "AppCompat existing after revert"
    Assert-CleanRevert $compatExisting "AppCompat existing final"
    Remove-LayerValue $compatExistingExe

    Write-Host "--- unrelated existing AppCompat value without sidecar is refused ---"
    $compatUnknown = New-TestCopy "compat_unknown"
    $compatUnknownExe = Join-Path $compatUnknown "gundam.exe"
    Remove-LayerValue $compatUnknownExe
    Set-LayerValue $compatUnknownExe "~ CUSTOM_USER_VALUE"
    $refused = $false
    try { Invoke-Apply $compatUnknown -Register } catch { $refused = $true; Write-Host "expected refusal: $($_.Exception.Message)" }
    if (-not $refused) { throw "apply unexpectedly overwrote an unrelated AppCompat value" }
    Assert-Hash $compatUnknownExe $originalSha256 "AppCompat refusal gundam.exe"
    Assert-Hash (Join-Path $compatUnknown "QTIM32.DLL") $runtimeSha256 "AppCompat refusal QTIM32.DLL"
    Assert-LayerValue $compatUnknownExe "~ CUSTOM_USER_VALUE" "AppCompat refusal preserved value"
    Assert-NotExists (Join-Path $compatUnknown "gundam.exe.orig") "AppCompat refusal backup"
    Remove-LayerValue $compatUnknownExe

    Write-Host "--- injected mid-transaction failure rolls back files, registry, and sidecar ---"
    $rollback = New-TestCopy "rollback"
    $rollbackExe = Join-Path $rollback "gundam.exe"
    Remove-LayerValue $rollbackExe
    $env:GUNDAM_WIN11PATCH_TEST_FAIL_AFTER_APP_COMPAT = "1"
    $rollbackFailed = $false
    try { Invoke-Apply $rollback -Register } catch { $rollbackFailed = $true; Write-Host "expected rollback failure: $($_.Exception.Message)" }
    Remove-Item Env:GUNDAM_WIN11PATCH_TEST_FAIL_AFTER_APP_COMPAT -ErrorAction SilentlyContinue
    if (-not $rollbackFailed) { throw "failure injection did not fail apply" }
    Assert-Hash $rollbackExe $originalSha256 "rollback gundam.exe"
    Assert-Hash (Join-Path $rollback "QTIM32.DLL") $runtimeSha256 "rollback QTIM32.DLL"
    Assert-Hash (Join-Path $rollback "CMGR32.DLL") $cmgrRuntimeSha256 "rollback CMGR32.DLL"
    Assert-LayerValue $rollbackExe $null "rollback AppCompat value"
    Assert-CleanRevert $rollback "rollback final"

    Write-Host "PASS: ZIP extraction, checksums, QTIM32/CMGR32 apply/revert cycle, v1.0.12 upgrade, v1.0.14 proxy upgrade and MidiLoop cleanup, installer (trailing-backslash path, in-game-folder auto-detect, overwritten-DLL refusal), long-path acceptance, path/non-ANSI refusal, AppCompat absent/present/refusal, and rollback all passed."
} finally {
    Remove-Item Env:GUNDAM_WIN11PATCH_TEST_FAIL_AFTER_APP_COMPAT -ErrorAction SilentlyContinue
    Remove-Item Env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE -ErrorAction SilentlyContinue
    if (-not $KeepTestDirectory -and $testDirCreated -and (Test-Path -LiteralPath $testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
    if ($archiveExtracted -and (Test-Path -LiteralPath $archiveExtracted)) {
        Remove-Item -LiteralPath $archiveExtracted -Recurse -Force
    }
}
