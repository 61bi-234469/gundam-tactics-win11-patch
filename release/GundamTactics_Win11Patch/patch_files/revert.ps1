# v1.1.0
param([string]$InstallPath = "")

$ErrorActionPreference = "Stop"
$ExpectedOriginalSha256 = "38bde2e4513c665d1425fd00203d0000001c5b81bc37899507b6ef7129f238d3"
$ExpectedRuntimeSha256 = "dbbe7e208955c0173d2a41a8873d1ccdacdca96e42948f861768e1dde3afc77f"
$ExpectedProxySha256 = "d3ec190e097a4a377164c985f0d314fed1b73678460fb8116a35e5fb4c815489"
# QTIM32.DLL proxies of v1.0.10-v1.0.16, which this revert also removes.
$KnownPreviousProxySha256 = @(
    "ce481563d035c5ba09883b7e00967ba73557969366657f31a00b7afa0c710af2",
    "08e39f76c954248d1f92541e9c4f0b9a46604d97ed1187469164377f0e7397b1",
    "3aaab62914d904198bf22472614532032adc438bbdd412eaa62af18ef701abf6",
    "3925975d279d553fe3418e43b2bffce2c401d3eccf46ec6bdf1fb75ea3fdfd6c"
)
$ExpectedCmgrRuntimeSha256 = "9fc00aece0c9db38b7e7001d261a060567eb035a343f4d833e58fd163e80f9e4"
$ExpectedCmgrProxySha256 = "db6456350aefe2e35114b183458457d88c6e7e8974f29863bd5b83b2be3a964d"

function Ensure-AtomicFileType {
    if (-not ([System.Management.Automation.PSTypeName]'GundamTactics.AtomicFile'.Type)) {
        Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace GundamTactics {
    public static class AtomicFile {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool MoveFileEx(string existingFileName,
            string newFileName, uint flags);
        public static void Move(string source, string destination, bool replace) {
            const uint MOVEFILE_REPLACE_EXISTING = 0x1;
            const uint MOVEFILE_WRITE_THROUGH = 0x8;
            uint flags = MOVEFILE_WRITE_THROUGH |
                (replace ? MOVEFILE_REPLACE_EXISTING : 0);
            if (!MoveFileEx(source, destination, flags)) {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "MoveFileEx failed");
            }
        }
    }
}
"@
    }
}
Ensure-AtomicFileType

# A quoted path that ends in a backslash reaches -File as ...\" (the
# backslash escapes the quote), so strip stray quotes and trailing separators.
function ConvertTo-CleanInstallPath([string]$Path) {
    $clean = $Path.Trim().Trim('"').Trim()
    if ($clean.Length -gt 3) { $clean = $clean.TrimEnd([char[]]@('\', '/')) }
    return $clean
}

function Get-GameDirectory {
    if ($InstallPath) { return (Resolve-Path -LiteralPath (ConvertTo-CleanInstallPath $InstallPath)).Path }
    $candidate = (Get-Location).Path
    if (Test-Path -LiteralPath (Join-Path $candidate "gundam.exe.orig")) { return $candidate }
    throw "InstallPath is required when the current directory does not contain gundam.exe.orig."
}
function Get-Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function New-TempPath([string]$Directory, [string]$Suffix) { return Join-Path $Directory (".{0}.{1}.{2}.tmp" -f [IO.Path]::GetFileNameWithoutExtension($Suffix), $PID, [guid]::NewGuid().ToString("N")) }
function Stage-Bytes([string]$Destination, [byte[]]$Data, [string]$ExpectedHash) {
    $temp = New-TempPath ([IO.Path]::GetDirectoryName($Destination)) ([IO.Path]::GetFileName($Destination))
    try {
        [IO.File]::WriteAllBytes($temp, $Data)
        if ((Get-Sha256 $temp) -ne $ExpectedHash) { throw "staged hash mismatch: $Destination" }
        return $temp
    } catch {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
        throw
    }
}
function Get-FileSnapshot([string]$Path) {
    if (Test-Path -LiteralPath $Path) { return [pscustomobject]@{ Path = $Path; Exists = $true; Bytes = [IO.File]::ReadAllBytes($Path) } }
    return [pscustomobject]@{ Path = $Path; Exists = $false; Bytes = $null }
}
function Restore-FileSnapshot($Snapshot) {
    if ($Snapshot.Exists) {
        $hash = [Security.Cryptography.SHA256]::Create().ComputeHash($Snapshot.Bytes)
        $expected = ([BitConverter]::ToString($hash)).Replace("-", "").ToLowerInvariant()
        $temp = Stage-Bytes $Snapshot.Path $Snapshot.Bytes $expected
        try { [GundamTactics.AtomicFile]::Move($temp, $Snapshot.Path, $true) }
        finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue } }
    } elseif (Test-Path -LiteralPath $Snapshot.Path) { Remove-Item -LiteralPath $Snapshot.Path -Force }
}
function Get-LayerState([string]$KeyPath, [string]$Name) {
    if ($env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE) {
        $testPath = $env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE
        if (-not (Test-Path -LiteralPath $testPath)) {
            return [pscustomobject]@{ KeyExists = $false; HasValue = $false; Value = $null }
        }
        $document = ([IO.File]::ReadAllText($testPath) | ConvertFrom-Json)
        $property = if ($document.values) { $document.values.PSObject.Properties[$Name] } else { $null }
        return [pscustomobject]@{
            KeyExists = [bool]$document.keyExists
            HasValue = $null -ne $property
            Value = if ($null -ne $property) { [string]$property.Value } else { $null }
        }
    }
    $keyExists = Test-Path -LiteralPath $KeyPath
    if (-not $keyExists) { return [pscustomobject]@{ KeyExists = $false; HasValue = $false; Value = $null } }
    try {
        $property = Get-ItemProperty -LiteralPath $KeyPath -Name $Name -ErrorAction Stop
        return [pscustomobject]@{ KeyExists = $true; HasValue = $true; Value = [string]$property.$Name }
    } catch [System.Management.Automation.PSArgumentException] { return [pscustomobject]@{ KeyExists = $true; HasValue = $false; Value = $null } }
}
function Restore-LayerState([string]$KeyPath, [string]$Name, $State) {
    if ($env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE) {
        $testPath = $env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE
        $document = if (Test-Path -LiteralPath $testPath) {
            ([IO.File]::ReadAllText($testPath) | ConvertFrom-Json)
        } else {
            [pscustomobject]@{ keyExists = $false; values = [pscustomobject]@{} }
        }
        if (-not $document.values) { $document | Add-Member NoteProperty values ([pscustomobject]@{}) }
        $property = $document.values.PSObject.Properties[$Name]
        if ($State.HasValue) {
            if ($null -eq $property) {
                $document.values | Add-Member NoteProperty -Name $Name -Value ([string]$State.Value)
            } else { $property.Value = [string]$State.Value }
        } elseif ($null -ne $property) {
            $document.values.PSObject.Properties.Remove($Name)
        }
        $document.keyExists = [bool]$State.KeyExists
        if (-not $document.keyExists -and $document.values.PSObject.Properties.Count -eq 0) {
            if (Test-Path -LiteralPath $testPath) { Remove-Item -LiteralPath $testPath -Force }
        } else {
            [IO.File]::WriteAllText($testPath, ($document | ConvertTo-Json -Depth 5 -Compress) + "`r`n")
        }
        return
    }
    if ($State.HasValue) {
        New-Item -Path $KeyPath -Force | Out-Null
        New-ItemProperty -Path $KeyPath -Name $Name -Value $State.Value -PropertyType String -Force | Out-Null
    } elseif (Test-Path -LiteralPath $KeyPath) { Remove-ItemProperty -LiteralPath $KeyPath -Name $Name -ErrorAction SilentlyContinue }
}

function Test-PackageLayer([string]$Value) {
    return $Value -eq "~ WIN95" -or $Value -eq "~ WIN95 RUNASADMIN"
}

function Test-LayerStateEqual($Left, $Right) {
    if ([bool]$Left.HasValue -ne [bool]$Right.HasValue) { return $false }
    if (-not $Left.HasValue) { return $true }
    return [string]$Left.Value -eq [string]$Right.Value
}

function Read-LayerSidecar([string]$Path, [string]$ExePath) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $document = ([IO.File]::ReadAllText($Path) | ConvertFrom-Json)
        if ([int]$document.schema -ne 1 -or
            [string]$document.exe -ne $ExePath -or
            $null -eq $document.before) {
            throw "invalid AppCompat sidecar schema or executable path"
        }
        $hasValue = [bool]$document.before.hasValue
        if ($hasValue -and $null -eq $document.before.value) {
            throw "AppCompat sidecar marks a missing value as present"
        }
        return [pscustomobject]@{
            KeyExists = [bool]$document.before.keyExists
            HasValue = $hasValue
            Value = if ($hasValue) { [string]$document.before.value } else { $null }
        }
    } catch {
        throw "invalid AppCompat sidecar '$Path': $($_.Exception.Message)"
    }
}

$gameDir = Get-GameDirectory
$exe = Join-Path $gameDir "gundam.exe"
$backup = Join-Path $gameDir "gundam.exe.orig"
$qtim = Join-Path $gameDir "QTIM32.dll"
$qtimReal = Join-Path $gameDir "QTIM32R.dll"
$cmgr = Join-Path $gameDir "CMGR32.dll"
$cmgrReal = Join-Path $gameDir "CMGR32R.dll"
$sidecar = Join-Path $gameDir "gundam_win11patch.state.json"
$layersKey = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
if (-not (Test-Path -LiteralPath $exe) -and -not (Test-Path -LiteralPath $backup)) { throw "neither gundam.exe nor backup was found: $gameDir" }
if (-not (Test-Path -LiteralPath $backup)) { throw "backup not found: $backup" }
if ((Get-Sha256 $backup) -ne $ExpectedOriginalSha256) { throw "gundam.exe.orig is not the expected original." }

$snapshots = @((Get-FileSnapshot $exe), (Get-FileSnapshot $backup), (Get-FileSnapshot $qtim), (Get-FileSnapshot $qtimReal), (Get-FileSnapshot $cmgr), (Get-FileSnapshot $cmgrReal), (Get-FileSnapshot $sidecar))
$layerBefore = Get-LayerState $layersKey $exe
$layerSidecar = Read-LayerSidecar $sidecar $exe
if ($layerSidecar) {
    if ($layerBefore.HasValue -and -not (Test-PackageLayer $layerBefore.Value) -and
        -not (Test-LayerStateEqual $layerBefore $layerSidecar)) {
        throw "current AppCompat value is not package-owned; refusing to overwrite it during revert."
    }
    $layerRestore = $layerSidecar
} else {
    if ($layerBefore.HasValue -and -not (Test-PackageLayer $layerBefore.Value)) {
        throw "current AppCompat value has no sidecar and is not a package value; refusing to remove it."
    }
    $layerRestore = [pscustomobject]@{ KeyExists = $false; HasValue = $false; Value = $null }
}
$staged = @()
$committed = $false
try {
    $exeStage = Stage-Bytes $exe ([IO.File]::ReadAllBytes($backup)) $ExpectedOriginalSha256
    $staged += $exeStage

    $qtimStage = $null
    $qtimHash = if (Test-Path -LiteralPath $qtim) { Get-Sha256 $qtim } else { $null }
    $realHash = if (Test-Path -LiteralPath $qtimReal) { Get-Sha256 $qtimReal } else { $null }
    if ($realHash -ne $ExpectedRuntimeSha256) { throw "QTIM32R.DLL is not a verified runtime backup." }
    if ($qtimHash -and $qtimHash -ne $ExpectedProxySha256 -and $qtimHash -ne $ExpectedRuntimeSha256 -and $KnownPreviousProxySha256 -notcontains $qtimHash) { throw "QTIM32.DLL is neither the known proxy nor the original runtime." }
    $qtimStage = Stage-Bytes $qtim ([IO.File]::ReadAllBytes($qtimReal)) $ExpectedRuntimeSha256
    $staged += $qtimStage

    $cmgrStage = $null
    $cmgrHash = if (Test-Path -LiteralPath $cmgr) { Get-Sha256 $cmgr } else { $null }
    $cmgrRealHash = if (Test-Path -LiteralPath $cmgrReal) { Get-Sha256 $cmgrReal } else { $null }
    if ($cmgrRealHash -eq $ExpectedCmgrRuntimeSha256) {
        if ($cmgrHash -and $cmgrHash -ne $ExpectedCmgrProxySha256 -and $cmgrHash -ne $ExpectedCmgrRuntimeSha256) { throw "CMGR32.DLL is neither the known proxy nor the original runtime." }
        $cmgrStage = Stage-Bytes $cmgr ([IO.File]::ReadAllBytes($cmgrReal)) $ExpectedCmgrRuntimeSha256
        $staged += $cmgrStage
    } elseif ($cmgrHash -eq $ExpectedCmgrRuntimeSha256 -and $null -eq $cmgrRealHash) {
        # Allow reverting a v1.0.1 installation, which predates the CMGR companion.
    } else { throw "CMGR32R.DLL is not a verified runtime backup." }

    [GundamTactics.AtomicFile]::Move($exeStage, $exe, $true)
    [GundamTactics.AtomicFile]::Move($qtimStage, $qtim, $true)
    if ($cmgrStage) { [GundamTactics.AtomicFile]::Move($cmgrStage, $cmgr, $true) }
    if ((Test-Path -LiteralPath $qtimReal)) { Remove-Item -LiteralPath $qtimReal -Force }
    if ((Test-Path -LiteralPath $cmgrReal)) { Remove-Item -LiteralPath $cmgrReal -Force }

    Restore-LayerState $layersKey $exe $layerRestore
    if ((Get-Sha256 $exe) -ne $ExpectedOriginalSha256 -or (Get-Sha256 $qtim) -ne $ExpectedRuntimeSha256 -or (Get-Sha256 $cmgr) -ne $ExpectedCmgrRuntimeSha256) { throw "restore verification failed." }
    Remove-Item -LiteralPath $backup -Force -ErrorAction Stop
    if (Test-Path -LiteralPath $sidecar) { Remove-Item -LiteralPath $sidecar -Force -ErrorAction Stop }
    if (Test-Path -LiteralPath $backup) { throw "verified backup was not removed." }
    if (Test-Path -LiteralPath $sidecar) { throw "AppCompat sidecar was not removed." }
    $committed = $true
    Write-Host "Original executable, QTIM32.DLL, and CMGR32.DLL restored; verified patch backups and AppCompat sidecar removed."
    # Loop-point copies of Sound\*.MID written by the v1.0.15+ proxy.
    $midiLoop = Join-Path $gameDir "MidiLoop"
    if (Test-Path -LiteralPath $midiLoop -PathType Container) {
        try {
            Get-ChildItem -LiteralPath $midiLoop -File -Force |
                Where-Object { $_.Name -match '\.(mid|rmi)(\.\d+\.tmp)?$' } |
                Remove-Item -Force -ErrorAction Stop
            if (@(Get-ChildItem -LiteralPath $midiLoop -Force).Count -eq 0) { Remove-Item -LiteralPath $midiLoop -Force -ErrorAction Stop }
        } catch { Write-Warning "could not remove the generated MidiLoop folder: $($_.Exception.Message)" }
    }
} catch {
    try {
        foreach ($snapshot in $snapshots) { Restore-FileSnapshot $snapshot }
        Restore-LayerState $layersKey $exe $layerBefore
    } catch { throw "revert failed and rollback also failed: $($_.Exception.Message)" }
    throw
} finally {
    foreach ($temp in $staged) { if ($temp -and (Test-Path -LiteralPath $temp)) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue } }
}
