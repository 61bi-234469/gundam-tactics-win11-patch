# v1.0.16
param(
    [string]$InstallPath = "",
    [switch]$RegisterAppCompat,
    [switch]$RunAsAdmin
)

$ErrorActionPreference = "Stop"

$ExpectedOriginalSha256 = "38bde2e4513c665d1425fd00203d0000001c5b81bc37899507b6ef7129f238d3"
$ExpectedPatchedSha256 = "607299a2cd7d5aeb1375cd838cd343cd81f9289311cea659d100c700d4035ec4"
# Older patched executables, upgraded by rebuilding from gundam.exe.orig:
# v1.0.2-v1.0.12 (no Phase 4 path fix) and v1.0.13-v1.0.15 (no Phase 5 stretch mode).
$KnownLegacyPatchedSha256 = @(
    "82c92402ac9c9282992d3c343dbb7c62463a3a5e58569ff845670bf166b867d4",
    "c693d60b73cbba731dbacbea255e845e97a0dae95cc80812316a5795e4f15942"
)
# Phase 4 opens assets as ".\<relative>", so only the full path limit remains:
# directory + separator + longest asset + NUL must fit in 256 ANSI/MBCS bytes
# (Windows MAX_PATH, kept within QuickTime's 255-byte path strings).
$MaxFullPathBytes = 256
$ExpectedRuntimeSha256 = "dbbe7e208955c0173d2a41a8873d1ccdacdca96e42948f861768e1dde3afc77f"
$ExpectedProxySha256 = "ce481563d035c5ba09883b7e00967ba73557969366657f31a00b7afa0c710af2"
# QTIM32.DLL proxies of v1.0.10-v1.0.14; applying over one of them upgrades it.
$KnownPreviousProxySha256 = @(
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
    if (Test-Path -LiteralPath (Join-Path $candidate "gundam.exe")) { return $candidate }
    throw "InstallPath is required when the current directory does not contain gundam.exe."
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-GamePathBudget([string]$Directory) {
    $encoding = [Text.Encoding]::Default
    $base = $Directory
    $root = [IO.Path]::GetPathRoot($base)
    if ($base -ne $root) { $base = $base.TrimEnd([char[]]@('\', '/')) }
    $physicalBase = (Get-Item -LiteralPath $Directory -Force).FullName
    $physicalRoot = [IO.Path]::GetPathRoot($physicalBase)
    if ($physicalBase -ne $physicalRoot) { $physicalBase = $physicalBase.TrimEnd([char[]]@('\', '/')) }
    # The package may be extracted inside the game folder; its own files are
    # not game assets.  Skip patch_files\ always, and the package folder when
    # it is a subfolder rather than the game folder itself.
    $packageRoots = @([IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\')
    $packageParent = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
    if (-not (Test-Path -LiteralPath (Join-Path $packageParent "gundam.exe"))) { $packageRoots += $packageParent + '\' }
    function Test-GameOpenedAsset([IO.FileInfo]$File) {
        foreach ($packageRoot in $packageRoots) {
            if ($File.FullName.StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase)) { return $false }
        }
        $name = $File.Name.ToLowerInvariant()
        if ($File.Extension -ieq ".url") { return $false }
        if ($name -in @(
            "gundam.exe.orig", "gundam_win11patch.state.json",
            "qtim_compat.ini", "qtim_compat_trace.log",
            "cmgr_compat_trace.log", "qtim_gdi_trace.log"
        )) { return $false }
        if ($name.EndsWith(".tmp") -or $name.EndsWith(".orig")) { return $false }
        return $true
    }
    $files = @(Get-ChildItem -LiteralPath $Directory -Recurse -File -Force |
        Where-Object { Test-GameOpenedAsset $_ })
    if ($files.Count -eq 0) { throw "no game files found below: $Directory" }

    $longestBytes = -1
    $longestRelative = $null
    foreach ($file in $files) {
        $full = [IO.Path]::GetFullPath($file.FullName)
        $relative = $full.Substring($physicalBase.Length).TrimStart([char[]]@('\', '/'))
        $relative = $relative.Replace('/', '\')
        $relativeBytes = $encoding.GetByteCount($relative)
        if ($relativeBytes -gt $longestBytes) {
            $longestBytes = $relativeBytes
            $longestRelative = $relative
        }
    }
    $directoryBytes = $encoding.GetByteCount($base)
    $totalBytes = $directoryBytes + 1 + $longestBytes + 1
    return [pscustomobject]@{
        DirectoryBytes = $directoryBytes
        LongestRelative = $longestRelative
        LongestRelativeBytes = $longestBytes
        TotalBytes = $totalBytes
        MaximumDirectoryBytes = $MaxFullPathBytes - 1 - $longestBytes - 1
    }
}

function Assert-GamePathAnsi([string]$Directory) {
    # QuickTime resolves ".\Movie\..." through the ANSI current directory; a
    # character outside the system code page turns into '?' and every movie
    # is silently skipped (verified with a Hangul folder on code page 932).
    $encoding = [Text.Encoding]::Default
    $physical = (Get-Item -LiteralPath $Directory -Force).FullName
    foreach ($candidate in @($Directory, $physical)) {
        if ($encoding.GetString($encoding.GetBytes($candidate)) -cne $candidate) {
            throw ("install path contains characters that the system ANSI code page " +
                "({0}) cannot represent: '{1}'. Move the game to a folder whose name " +
                "uses only characters of that code page, such as C:\G-TACT.") -f
                $encoding.CodePage, $candidate
        }
    }
}

function Assert-GamePathBudget([string]$Directory) {
    Assert-GamePathAnsi $Directory
    $budget = Get-GamePathBudget $Directory
    if ($budget.TotalBytes -gt $MaxFullPathBytes) {
        throw (("install path is too long: " +
            "directory={0} bytes, longest relative path='{1}' ({2} bytes), " +
            "full path including separator and NUL={3} bytes (limit $MaxFullPathBytes). " +
            "Maximum allowed install-directory length is {4} ANSI/MBCS bytes; " +
            "move the game to a shorter folder such as C:\G-TACT.") -f
            $budget.DirectoryBytes, $budget.LongestRelative,
            $budget.LongestRelativeBytes, $budget.TotalBytes,
            $budget.MaximumDirectoryBytes)
    }
}

function New-TempPath([string]$Directory, [string]$Suffix) {
    return Join-Path $Directory (".{0}.{1}.{2}.tmp" -f
        [IO.Path]::GetFileNameWithoutExtension($Suffix), $PID,
        [guid]::NewGuid().ToString("N"))
}

function Stage-Bytes([string]$Destination, [byte[]]$Data, [string]$ExpectedHash) {
    $directory = [IO.Path]::GetDirectoryName($Destination)
    $temp = New-TempPath $directory ([IO.Path]::GetFileName($Destination))
    try {
        [IO.File]::WriteAllBytes($temp, $Data)
        if ((Get-Sha256 $temp) -ne $ExpectedHash) { throw "staged hash mismatch: $Destination" }
        return $temp
    } catch {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
        throw
    }
}

function Assert-Bytes([byte[]]$Data, [int]$Offset, [byte[]]$Expected, [string]$Name) {
    if ($Offset -lt 0 -or $Offset + $Expected.Length -gt $Data.Length) { throw "$Name is outside the executable." }
    for ($i = 0; $i -lt $Expected.Length; $i++) {
        if ($Data[$Offset + $i] -ne $Expected[$i]) {
            $actual = ($Data[$Offset..($Offset + $Expected.Length - 1)] | ForEach-Object { $_.ToString("x2") }) -join " "
            $want = ($Expected | ForEach-Object { $_.ToString("x2") }) -join " "
            throw "unexpected bytes at $Name (offset 0x$('{0:x}' -f $Offset)): $actual; expected $want"
        }
    }
}

function Set-Bytes([byte[]]$Data, [int]$Offset, [byte[]]$Replacement) {
    for ($i = 0; $i -lt $Replacement.Length; $i++) { $null = $Data[$Offset + $i] = $Replacement[$i] }
}

function Assert-Zero([byte[]]$Data, [int]$Offset, [int]$Length, [string]$Name) {
    if ($Offset -lt 0 -or $Offset + $Length -gt $Data.Length) { throw "$Name is outside the executable." }
    for ($i = 0; $i -lt $Length; $i++) {
        if ($Data[$Offset + $i] -ne 0) { throw "$Name is not an empty code cave; refusing to overwrite it." }
    }
}

function Convert-Hex([string]$Text) {
    [byte[]]$result = @($Text -split "\s+" | Where-Object { $_ } | ForEach-Object { [Convert]::ToByte($_, 16) })
    return $result
}

function Get-PatchedExecutableBytes([byte[]]$Data) {
    Assert-Bytes $Data 0xEFA2 (Convert-Hex "74 1e") "VA 0x0040FBA2 color-depth branch"
    Assert-Bytes $Data 0x20921 (Convert-Hex "57 ff 15 dc c2 44 00 57 8b 2d 48 c3 44 00 ff d5") "VA 0x00421521 WAVEHDR store"
    Assert-Bytes $Data 0x2096C (Convert-Hex "a1 8c dc 43 00 85 c0 74 1d 50 ff 15 dc c2 44 00 a1 8c dc 43 00 50 ff 15 48 c3 44 00 c7 05 8c dc 43 00 00 00 00 00") "VA 0x0042156C cleanup hook"
    Assert-Zero $Data 0x37820 77 "VA 0x00438420 cleanup code cave"
    Assert-Bytes $Data 0x20A00 (Convert-Hex "83 ec 24 8b 0d d8 dd 43 00 85 c9 56 0f 84 82 00 00 00 83 3d 50 a1 44 00 08 75 79") "VA 0x00421600 MIDI poll hook"
    Assert-Zero $Data 0x37880 70 "VA 0x00438480 MIDI code cave"
    Assert-Bytes $Data 0xC7AA (Convert-Hex "6a 00 68 98 b2 43 00") "VA 0x0040D3AA movie NULL guard"

    Set-Bytes $Data 0xEFA2 (Convert-Hex "eb 1e")
    Set-Bytes $Data 0x20921 (Convert-Hex "89 3d 90 dc 43 00 90 90 90 90 90 90 90 90 90 90")
    Set-Bytes $Data 0x2096C (Convert-Hex "e9 af 6e 01 00 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90")
    Set-Bytes $Data 0x37820 (Convert-Hex "a1 90 dc 43 00 85 c0 74 1d 50 ff 15 dc c2 44 00 a1 90 dc 43 00 50 ff 15 48 c3 44 00 c7 05 90 dc 43 00 00 00 00 00 a1 8c dc 43 00 85 c0 74 1d 50 ff 15 dc c2 44 00 a1 8c dc 43 00 50 ff 15 48 c3 44 00 c7 05 8c dc 43 00 00 00 00 00 c3")
    Set-Bytes $Data 0x20A00 (Convert-Hex "e9 7b 6e 01 00 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90 90")
    Set-Bytes $Data 0x37880 (Convert-Hex "83 ec 24 56 8b 0d d8 dd 43 00 85 c9 74 33 83 3d 50 a1 44 00 08 75 2a e8 74 3b fd ff 8b 15 64 f6 43 00 3b c2 74 1b 3b c2 72 07 8d 52 06 3b c2 72 10 a3 64 f6 43 00 8b 0d d8 dd 43 00 e9 5a 91 fe ff 5e 83 c4 24 c3")
    Set-Bytes $Data 0xC7AA (Convert-Hex "5f 5e c3 90 90 90")
    return ,$Data
}

function Get-Phase3DisplayExecutableBytes([byte[]]$Data, [string]$Part = "b") {
    $normalized = $Part.ToLowerInvariant()
    if ($normalized -notin @("a", "b")) { throw "unknown Phase 3 part: $Part" }

    if ($normalized -eq "a") {
        # 不採用(実画面 A/B で PUSH 層反転)。診断呼出し専用。
        Assert-Bytes $Data 0xED4F (Convert-Hex "f7 d8 a3 c8 9a 44 00") "VA 0x0040F94F part a"
        Set-Bytes $Data 0xED4F (Convert-Hex "90 90")
    }
    if ($normalized -eq "b") {
        # Accepted Phase 3(b): FUN_004090F0 copied BMI biHeight = -416.
        Assert-Bytes $Data 0x8683 (Convert-Hex "ff 15 78 c2 44 00") "VA 0x00409283 part b call"
        Assert-Zero $Data 0x37950 22 "VA 0x00438550 Phase 3 part b code cave"
        Set-Bytes $Data 0x8683 (Convert-Hex "e9 c8 f2 02 00 90")
        Set-Bytes $Data 0x37950 (Convert-Hex "8b 4c 24 28 c7 41 08 60 fe ff ff ff 15 78 c2 44 00 e9 23 0d fd ff")
    }
    return ,$Data
}

function Get-PathFixExecutableBytes([byte[]]$Data) {
    # Phase 4: FUN_00430F60 (the game's getcwd wrapper) stores "." and returns
    # the buffer, so assets are opened as ".\<relative>" and the install
    # directory no longer has to fit gundam.exe's 80-byte path buffers.
    Assert-Bytes $Data 0x30360 (Convert-Hex "8b 44 24 08 8b 4c 24 04 50 51 6a 00 e8 0f 00 00 00 83 c4 0c c3") "VA 0x00430F60 getcwd wrapper"
    Set-Bytes $Data 0x30360 (Convert-Hex "8b 44 24 04 66 c7 00 2e 00 c3 cc cc cc cc cc cc cc cc cc cc cc")
    return ,$Data
}

function Get-StretchModeExecutableBytes([byte[]]$Data) {
    # Phase 5: FUN_00407200 (full-screen BMP loader) sets COLORONCOLOR before
    # StretchDIBits.  The default BLACKONWHITE ANDs palette indices when the
    # 587-pixel-wide HERBOR\MUSA.DOC is shrunk to 576, drawing vertical lines.
    # SetStretchBltMode is not imported: the cave resolves it via
    # GetModuleHandleA("GDI32.DLL") + GetProcAddress, then jumps to StretchDIBits.
    Assert-Bytes $Data 0x678C (Convert-Hex "ff 15 78 c2 44 00") "VA 0x0040738C BMP loader StretchDIBits call"
    Assert-Zero $Data 0x37980 76 "VA 0x00438580 Phase 5 code cave"
    Set-Bytes $Data 0x678C (Convert-Hex "e8 ef 11 03 00 90")
    Set-Bytes $Data 0x37980 (Convert-Hex "68 b0 85 43 00 ff 15 1c c3 44 00 85 c0 74 18 68 ba 85 43 00 50 ff 15 60 c3 44 00 85 c0 74 08 6a 03 ff 74 24 08 ff d0 ff 25 78 c2 44 00 cc cc cc 47 44 49 33 32 2e 44 4c 4c 00 53 65 74 53 74 72 65 74 63 68 42 6c 74 4d 6f 64 65 00")
    return ,$Data
}

function Get-FullPatchedExecutableBytes([byte[]]$Original) {
    [byte[]]$patched = Get-PatchedExecutableBytes $Original
    $patched = Get-Phase3DisplayExecutableBytes $patched "b"
    $patched = Get-PathFixExecutableBytes $patched
    $patched = Get-StretchModeExecutableBytes $patched
    return ,$patched
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
    } catch [System.Management.Automation.PSArgumentException] {
        return [pscustomobject]@{ KeyExists = $true; HasValue = $false; Value = $null }
    }
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

function Set-LayerValue([string]$KeyPath, [string]$Name, [string]$Value) {
    if ($env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE) {
        $testPath = $env:GUNDAM_WIN11PATCH_TEST_REGISTRY_FILE
        $document = if (Test-Path -LiteralPath $testPath) {
            ([IO.File]::ReadAllText($testPath) | ConvertFrom-Json)
        } else { [pscustomobject]@{ keyExists = $false; values = [pscustomobject]@{} } }
        if (-not $document.values) { $document | Add-Member NoteProperty values ([pscustomobject]@{}) }
        $property = $document.values.PSObject.Properties[$Name]
        if ($null -eq $property) {
            $document.values | Add-Member NoteProperty -Name $Name -Value $Value
        } else { $property.Value = $Value }
        $document.keyExists = $true
        [IO.File]::WriteAllText($testPath, ($document | ConvertTo-Json -Depth 5 -Compress) + "`r`n")
        return
    }
    New-Item -Path $KeyPath -Force | Out-Null
    New-ItemProperty -Path $KeyPath -Name $Name -Value $Value -PropertyType String -Force | Out-Null
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

function New-LayerSidecarBytes([string]$ExePath, $State) {
    $document = [ordered]@{
        schema = 1
        exe = $ExePath
        before = [ordered]@{
            keyExists = [bool]$State.KeyExists
            hasValue = [bool]$State.HasValue
            value = if ($State.HasValue) { [string]$State.Value } else { $null }
        }
        packageValues = @("~ WIN95", "~ WIN95 RUNASADMIN")
    }
    $json = $document | ConvertTo-Json -Depth 5 -Compress
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    return $utf8.GetBytes($json + "`r`n")
}

$gameDir = Get-GameDirectory
$exe = Join-Path $gameDir "gundam.exe"
$backup = Join-Path $gameDir "gundam.exe.orig"
$qtim = Join-Path $gameDir "QTIM32.dll"
$qtimReal = Join-Path $gameDir "QTIM32R.dll"
$cmgr = Join-Path $gameDir "CMGR32.dll"
$cmgrReal = Join-Path $gameDir "CMGR32R.dll"
$proxy = Join-Path $PSScriptRoot "QTIM32.dll"
$cmgrProxy = Join-Path $PSScriptRoot "CMGR32.dll"
$sidecar = Join-Path $gameDir "gundam_win11patch.state.json"
$layersKey = "HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
if (-not (Test-Path -LiteralPath $exe)) { throw "gundam.exe not found: $exe" }
if (-not (Test-Path -LiteralPath $proxy) -or (Get-Sha256 $proxy) -ne $ExpectedProxySha256) { throw "verified release proxy is missing or has the wrong hash." }
if (-not (Test-Path -LiteralPath $cmgrProxy) -or (Get-Sha256 $cmgrProxy) -ne $ExpectedCmgrProxySha256) { throw "verified release CMGR32 proxy is missing or has the wrong hash." }
Assert-GamePathBudget $gameDir

$snapshots = @((Get-FileSnapshot $exe), (Get-FileSnapshot $backup), (Get-FileSnapshot $qtim), (Get-FileSnapshot $qtimReal), (Get-FileSnapshot $cmgr), (Get-FileSnapshot $cmgrReal), (Get-FileSnapshot $sidecar))
$layerBefore = Get-LayerState $layersKey $exe
$layerSidecar = Read-LayerSidecar $sidecar $exe
if ($layerSidecar) {
    $currentIsPackage = -not $layerBefore.HasValue -or (Test-PackageLayer $layerBefore.Value)
    if (-not $currentIsPackage -and -not (Test-LayerStateEqual $layerBefore $layerSidecar)) {
        throw "existing AppCompat value is not the package value recorded by the sidecar; refusing to overwrite."
    }
} elseif ($layerBefore.HasValue -and -not (Test-PackageLayer $layerBefore.Value)) {
    throw "existing AppCompat value has no sidecar and is not a package value; refusing to overwrite."
}
$wantsAppCompat = $RegisterAppCompat -or $RunAsAdmin
$staged = @()
$committed = $false
try {
    $exeHash = Get-Sha256 $exe
    $exeBytes = [IO.File]::ReadAllBytes($exe)
    if ((Test-Path -LiteralPath $backup) -and (Get-Sha256 $backup) -ne $ExpectedOriginalSha256) { throw "existing gundam.exe.orig is not the expected original." }
    if ($exeHash -ne $ExpectedOriginalSha256 -and $exeHash -ne $ExpectedPatchedSha256 -and $KnownLegacyPatchedSha256 -notcontains $exeHash) { throw "gundam.exe is neither the expected original nor a known patched build." }
    if ($exeHash -ne $ExpectedOriginalSha256 -and -not (Test-Path -LiteralPath $backup)) { throw "patched gundam.exe has no verified original backup." }

    $exeBackupStage = $null; $exeStage = $null
    if ($exeHash -eq $ExpectedOriginalSha256) {
        if (-not (Test-Path -LiteralPath $backup)) { $exeBackupStage = Stage-Bytes $backup $exeBytes $ExpectedOriginalSha256; $staged += $exeBackupStage }
        [byte[]]$patched = Get-FullPatchedExecutableBytes $exeBytes
        $exeStage = Stage-Bytes $exe $patched $ExpectedPatchedSha256; $staged += $exeStage
    } elseif ($KnownLegacyPatchedSha256 -contains $exeHash) {
        # Upgrade from v1.0.2-v1.0.15: rebuild from the verified original backup.
        [byte[]]$patched = Get-FullPatchedExecutableBytes ([IO.File]::ReadAllBytes($backup))
        $exeStage = Stage-Bytes $exe $patched $ExpectedPatchedSha256; $staged += $exeStage
    }

    $qtimStage = $null; $qtimRealStage = $null
    $qtimHash = if (Test-Path -LiteralPath $qtim) { Get-Sha256 $qtim } else { $null }
    $realHash = if (Test-Path -LiteralPath $qtimReal) { Get-Sha256 $qtimReal } else { $null }
    if ($qtimHash -eq $ExpectedProxySha256) {
        if ($realHash -ne $ExpectedRuntimeSha256) { throw "QTIM32.DLL is already the patch proxy but QTIM32R.DLL (the original QuickTime backup) is missing or modified. The original QTIM32.DLL was probably overwritten by copying the patch files straight into the game folder; restore QTIM32.DLL and CMGR32.DLL from the game CD (or reinstall the game) and run the patch again." }
    } elseif ($qtimHash -and $KnownPreviousProxySha256 -contains $qtimHash) {
        if ($realHash -ne $ExpectedRuntimeSha256) { throw "QTIM32.DLL is an older patch proxy but QTIM32R.DLL (the original QuickTime backup) is missing or modified; restore QTIM32.DLL and CMGR32.DLL from the game CD (or reinstall the game) and run the patch again." }
        $qtimStage = Stage-Bytes $qtim ([IO.File]::ReadAllBytes($proxy)) $ExpectedProxySha256; $staged += $qtimStage
    } elseif ($qtimHash -eq $ExpectedRuntimeSha256) {
        if ($realHash -and $realHash -ne $ExpectedRuntimeSha256) { throw "QTIM32R.DLL exists but is not the verified runtime." }
        if (-not $realHash) { $qtimRealStage = Stage-Bytes $qtimReal ([IO.File]::ReadAllBytes($qtim)) $ExpectedRuntimeSha256; $staged += $qtimRealStage }
        $qtimStage = Stage-Bytes $qtim ([IO.File]::ReadAllBytes($proxy)) $ExpectedProxySha256; $staged += $qtimStage
    } elseif ($null -eq $qtimHash -and $realHash -eq $ExpectedRuntimeSha256) {
        $qtimStage = Stage-Bytes $qtim ([IO.File]::ReadAllBytes($proxy)) $ExpectedProxySha256; $staged += $qtimStage
    } else { throw "QTIM32.DLL/QTIM32R.DLL is not a recognized stock or proxy layout." }

    $cmgrStage = $null; $cmgrRealStage = $null
    $cmgrHash = if (Test-Path -LiteralPath $cmgr) { Get-Sha256 $cmgr } else { $null }
    $cmgrRealHash = if (Test-Path -LiteralPath $cmgrReal) { Get-Sha256 $cmgrReal } else { $null }
    if ($cmgrHash -eq $ExpectedCmgrProxySha256) {
        if ($cmgrRealHash -ne $ExpectedCmgrRuntimeSha256) { throw "CMGR32.DLL is already the patch proxy but CMGR32R.DLL (the original backup) is missing or modified. The original CMGR32.DLL was probably overwritten by copying the patch files straight into the game folder; restore QTIM32.DLL and CMGR32.DLL from the game CD (or reinstall the game) and run the patch again." }
    } elseif ($cmgrHash -eq $ExpectedCmgrRuntimeSha256) {
        if ($cmgrRealHash -and $cmgrRealHash -ne $ExpectedCmgrRuntimeSha256) { throw "CMGR32R.DLL exists but is not the verified runtime." }
        if (-not $cmgrRealHash) { $cmgrRealStage = Stage-Bytes $cmgrReal ([IO.File]::ReadAllBytes($cmgr)) $ExpectedCmgrRuntimeSha256; $staged += $cmgrRealStage }
        $cmgrStage = Stage-Bytes $cmgr ([IO.File]::ReadAllBytes($cmgrProxy)) $ExpectedCmgrProxySha256; $staged += $cmgrStage
    } elseif ($null -eq $cmgrHash -and $cmgrRealHash -eq $ExpectedCmgrRuntimeSha256) {
        $cmgrStage = Stage-Bytes $cmgr ([IO.File]::ReadAllBytes($cmgrProxy)) $ExpectedCmgrProxySha256; $staged += $cmgrStage
    } else { throw "CMGR32.DLL/CMGR32R.DLL is not a recognized stock or proxy layout." }

    $sidecarStage = $null
    if ($wantsAppCompat -and -not $layerSidecar) {
        $sidecarBytes = New-LayerSidecarBytes $exe $layerBefore
        $sidecarHash = ([BitConverter]::ToString(([Security.Cryptography.SHA256]::Create()).ComputeHash($sidecarBytes))).Replace("-", "").ToLowerInvariant()
        $sidecarStage = Stage-Bytes $sidecar $sidecarBytes $sidecarHash
        $staged += $sidecarStage
    }

    if ($exeBackupStage) { [GundamTactics.AtomicFile]::Move($exeBackupStage, $backup, $false) }
    if ($exeStage) { [GundamTactics.AtomicFile]::Move($exeStage, $exe, $true) }
    if ($qtimRealStage) { [GundamTactics.AtomicFile]::Move($qtimRealStage, $qtimReal, $false) }
    if ($qtimStage) { [GundamTactics.AtomicFile]::Move($qtimStage, $qtim, $true) }
    if ($cmgrRealStage) { [GundamTactics.AtomicFile]::Move($cmgrRealStage, $cmgrReal, $false) }
    if ($cmgrStage) { [GundamTactics.AtomicFile]::Move($cmgrStage, $cmgr, $true) }

    if ($sidecarStage) { [GundamTactics.AtomicFile]::Move($sidecarStage, $sidecar, $true) }

    if ($wantsAppCompat) {
        $layer = if ($RunAsAdmin) { "~ WIN95 RUNASADMIN" } else { "~ WIN95" }
        Set-LayerValue $layersKey $exe $layer
    }
    if ($env:GUNDAM_WIN11PATCH_TEST_FAIL_AFTER_APP_COMPAT -eq "1") {
        throw "test failure injected after AppCompat commit"
    }
    $committed = $true
    Write-Host "v1.0.16 apply committed atomically: $gameDir"
    if ($wantsAppCompat) { Write-Host "AppCompat pre-state saved in $sidecar" }
} catch {
    try {
        foreach ($snapshot in $snapshots) { Restore-FileSnapshot $snapshot }
        Restore-LayerState $layersKey $exe $layerBefore
    } catch { throw "apply failed and rollback also failed: $($_.Exception.Message)" }
    throw
} finally {
    foreach ($temp in $staged) { if ($temp -and (Test-Path -LiteralPath $temp)) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue } }
}

if ($committed) { Write-Host "QTIM32/CMGR32 companion proxies deployed; trace is OFF unless explicitly enabled." }
