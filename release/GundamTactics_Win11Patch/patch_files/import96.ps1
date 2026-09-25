# v1.2.0 - copies the 1996 original edition from its CD into a game folder.
# The 1996 setup.exe runs the game from the CD, and its QuickTime 2.x is
# installed system-wide by qt32.exe. This script lays the game out like the
# 2001 reissue instead: every disc file except the installers, plus the 14
# QuickTime files the reissue ships next to gundam.exe, which are expanded
# from the SZDD blocks inside qt32.exe (nothing is installed into Windows).
# Called by install.ps1 before apply.ps1. Writes nothing outside
# -Destination; on failure everything this run created is removed again.
param(
    [Parameter(Mandatory = $true)][string]$DiscDir,
    [Parameter(Mandatory = $true)][string]$Destination
)

$ErrorActionPreference = "Stop"

# gundam.exe of the 1996 disc is byte-identical to the 2001 reissue.
$ExpectedGameSha256 = "38bde2e4513c665d1425fd00203d0000001c5b81bc37899507b6ef7129f238d3"
$ExpectedQt32Sha256 = "50ba0dd5daae99101fe96ce9cac84a502a2ed14edc5a4074dc944df66b94e76e"
# Offsets of the SZDD headers in that qt32.exe. The expanded files are
# byte-identical to the ones the 2001 reissue installs with the game.
$QuickTimeFiles = @(
    @{ Name = "QTIM32.DLL"; Offset = 0x750c0; Sha256 = "dbbe7e208955c0173d2a41a8873d1ccdacdca96e42948f861768e1dde3afc77f" },
    @{ Name = "CMGR32.DLL"; Offset = 0x480e0; Sha256 = "9fc00aece0c9db38b7e7001d261a060567eb035a343f4d833e58fd163e80f9e4" },
    @{ Name = "MCIQTENU.Q32"; Offset = 0x46264; Sha256 = "3337791a8d0ded6b82b0cf2c8715898af6743f54dd0bef568a0d18de194ab84b" },
    @{ Name = "CVID32.QTC"; Offset = 0x5c2ec; Sha256 = "72b9754464f846dcf92c2338ef9104c927fd259abc0f340b25d16d1af8d23fb5" },
    @{ Name = "DCI32.QTC"; Offset = 0x720d4; Sha256 = "e0f2411c9a0344514bf1bade6d5fad783f02e049adeae78c5daecf425b595ae0" },
    @{ Name = "DHIO32.QTC"; Offset = 0xdbb54; Sha256 = "4352030c071820669bbdddbd2025f0f369e6d0453618743b567a8e54775a7fee" },
    @{ Name = "IV32QT32.QTC"; Offset = 0x6965c; Sha256 = "6c2fad310c35665584332d956044c0e1c80c772505cc7fe2295b3bb52e0ff53b" },
    @{ Name = "JPEG32.QTC"; Offset = 0xc7194; Sha256 = "35c3e43aa8f2b8f59f315e00f821a988c6ba303768f41f14469b41f562082158" },
    @{ Name = "MC32.QTC"; Offset = 0x4ca7c; Sha256 = "d5b94f481741514f05435cb6abe36572468a429461366e6b9fc985e42303df20" },
    @{ Name = "NAVG32.QTC"; Offset = 0x57988; Sha256 = "8135fb867d3dee703fbd06cdca41f205072a82199c71a231b8501888126f9679" },
    @{ Name = "RAW32.QTC"; Offset = 0xa1b38; Sha256 = "e7b60f01838aa243961cbc47b4b835ef97d7484f239144580f7fe88c713fa6c9" },
    @{ Name = "RLE32.QTC"; Offset = 0xa43ec; Sha256 = "b87521c7a23540ab9125065d19d7805a496c67b2572397e4a59eb1770575a3de" },
    @{ Name = "RPZA32.QTC"; Offset = 0xad0ac; Sha256 = "2e03e7affdd62d1d0b0992c3460f6c335f675a9a5cb7cb7befef82b99cdf1b61" },
    @{ Name = "SMC32.QTC"; Offset = 0xbaed4; Sha256 = "04da19a596d455c25f0993c11c7801569441fa1c48a5d06fe2e1119324e7fb38" }
)
# Disc files that belong to the 1996 installer, not to the game.
$SkippedDiscFiles = @("setup.exe", "qt32.exe", "autorun.inf")

if (-not ([System.Management.Automation.PSTypeName]'GundamTactics.Szdd').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Security.Cryptography;
namespace GundamTactics {
    public static class Szdd {
        static readonly byte[] Magic = { 0x53, 0x5A, 0x44, 0x44, 0x88, 0xF0, 0x27, 0x33, 0x41 };
        // MS compress.exe (SZDD, mode 'A'): LZSS with a 4096-byte window
        // pre-filled with spaces and the write position starting at 4080.
        public static byte[] Expand(byte[] data, int offset) {
            if (offset < 0 || offset + 14 > data.Length) throw new InvalidDataException("SZDD header is outside the file");
            for (int k = 0; k < Magic.Length; k++)
                if (data[offset + k] != Magic[k]) throw new InvalidDataException("no SZDD header at offset 0x" + offset.ToString("x"));
            int size = BitConverter.ToInt32(data, offset + 10);
            if (size <= 0 || size > 16 * 1024 * 1024) throw new InvalidDataException("bad SZDD size " + size);
            byte[] output = new byte[size];
            byte[] window = new byte[4096];
            for (int k = 0; k < window.Length; k++) window[k] = 0x20;
            int pos = 4096 - 16, o = 0, i = offset + 14;
            while (o < size) {
                if (i >= data.Length) throw new InvalidDataException("truncated SZDD data");
                int control = data[i++];
                for (int bit = 0; bit < 8 && o < size; bit++) {
                    if ((control & (1 << bit)) != 0) {
                        if (i >= data.Length) throw new InvalidDataException("truncated SZDD data");
                        byte c = data[i++];
                        output[o++] = c; window[pos] = c; pos = (pos + 1) & 4095;
                    } else {
                        if (i + 1 >= data.Length) throw new InvalidDataException("truncated SZDD data");
                        int lo = data[i++], hi = data[i++];
                        int match = lo | ((hi & 0xF0) << 4);
                        int length = (hi & 0x0F) + 3;
                        for (int k = 0; k < length && o < size; k++) {
                            byte c = window[(match + k) & 4095];
                            output[o++] = c; window[pos] = c; pos = (pos + 1) & 4095;
                        }
                    }
                }
            }
            return output;
        }

        // Copies a file and returns the SHA256 of the bytes that were read.
        public static string CopyWithHash(string source, string destination) {
            using (var sha = SHA256.Create())
            using (var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16))
            using (var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 16)) {
                byte[] buffer = new byte[1 << 16];
                int read;
                while ((read = input.Read(buffer, 0, buffer.Length)) > 0) {
                    sha.TransformBlock(buffer, 0, read, null, 0);
                    output.Write(buffer, 0, read);
                }
                sha.TransformFinalBlock(buffer, 0, 0);
                return BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
            }
        }
    }
}
"@
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-BytesSha256([byte[]]$Data) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Data))).Replace("-", "").ToLowerInvariant() }
    finally { $sha.Dispose() }
}

$disc = (Resolve-Path -LiteralPath $DiscDir).Path
$discGame = Join-Path $disc "gundam.exe"
$discQt32 = Join-Path $disc "qt32.exe"
if (-not (Test-Path -LiteralPath $discGame -PathType Leaf) -or -not (Test-Path -LiteralPath $discQt32 -PathType Leaf)) {
    throw "not a 1996 edition disc (gundam.exe and qt32.exe expected): $disc"
}
if ((Get-Sha256 $discGame) -ne $ExpectedGameSha256) { throw "1996 disc gundam.exe is not the expected original." }
[byte[]]$qt32 = [IO.File]::ReadAllBytes($discQt32)
if ((Get-BytesSha256 $qt32) -ne $ExpectedQt32Sha256) { throw "1996 disc qt32.exe is not the expected QuickTime installer." }

$dest = [IO.Path]::GetFullPath($Destination)
if ((Test-Path -LiteralPath $dest) -and -not (Test-Path -LiteralPath $dest -PathType Container)) { throw "destination is not a folder: $dest" }
if (($dest.TrimEnd('\') + '\').StartsWith($disc.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "destination is on the disc: $dest" }
if (Test-Path -LiteralPath (Join-Path $dest "gundam.exe")) { throw "destination already contains gundam.exe: $dest" }

# Plan: top-level disc entries plus the QuickTime files. A destination file
# with identical content (the 1996 setup puts gundam.ico and readme.doc into
# C:\G-TACT) is kept; anything else already there is a conflict.
$topLevel = @(Get-ChildItem -LiteralPath $disc -Force | Where-Object { $SkippedDiscFiles -notcontains $_.Name.ToLowerInvariant() })
$discNames = @($topLevel | ForEach-Object { $_.Name.ToUpperInvariant() })
foreach ($file in $QuickTimeFiles) {
    if ($discNames -contains $file.Name) { throw "unexpected $($file.Name) on the 1996 disc." }
}
$keep = @{}
foreach ($item in $topLevel) {
    $target = Join-Path $dest $item.Name
    if (-not (Test-Path -LiteralPath $target)) { continue }
    if (-not $item.PSIsContainer -and (Test-Path -LiteralPath $target -PathType Leaf) -and (Get-Sha256 $target) -eq (Get-Sha256 $item.FullName)) {
        $keep[$item.Name.ToUpperInvariant()] = $true
        continue
    }
    throw "destination already contains $($item.Name): $dest"
}
foreach ($file in $QuickTimeFiles) {
    $target = Join-Path $dest $file.Name
    if (-not (Test-Path -LiteralPath $target)) { continue }
    if ((Test-Path -LiteralPath $target -PathType Leaf) -and (Get-Sha256 $target) -eq $file.Sha256) { $keep[$file.Name] = $true; continue }
    throw "destination already contains $($file.Name): $dest"
}

$createdDest = $false
if (-not (Test-Path -LiteralPath $dest)) {
    New-Item -ItemType Directory -Path $dest | Out-Null
    $createdDest = $true
}
$stage = Join-Path $dest (".gt96_import_{0}" -f [guid]::NewGuid().ToString("N"))
$moved = New-Object System.Collections.Generic.List[string]
try {
    New-Item -ItemType Directory -Path $stage | Out-Null

    $files = @($topLevel | ForEach-Object {
        if ($_.PSIsContainer) { Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force } else { $_ }
    })
    Write-Host ("Copying {0} files from the 1996 disc {1} ..." -f $files.Count, $disc)
    $count = 0
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($disc.TrimEnd('\').Length).TrimStart('\')
        $target = Join-Path $stage $relative
        $parent = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        $readHash = [GundamTactics.Szdd]::CopyWithHash($file.FullName, $target)
        # CD files carry the read-only attribute; apply.ps1 must be able to replace them.
        [IO.File]::SetAttributes($target, [IO.FileAttributes]::Normal)
        if ((Get-Sha256 $target) -ne $readHash) { throw "copy verification failed: $relative" }
        $count++
        if ($count % 200 -eq 0) { Write-Host ("  {0} / {1}" -f $count, $files.Count) }
        if ($count -eq 100 -and $env:GUNDAM_WIN11PATCH_TEST_FAIL_IMPORT96 -eq "copy") { throw "test failure injected during the 1996 copy" }
    }

    Write-Host "Expanding QuickTime files from qt32.exe ..."
    foreach ($file in $QuickTimeFiles) {
        [byte[]]$data = [GundamTactics.Szdd]::Expand($qt32, $file.Offset)
        if ((Get-BytesSha256 $data) -ne $file.Sha256) { throw "expanded $($file.Name) has the wrong hash." }
        [IO.File]::WriteAllBytes((Join-Path $stage $file.Name), $data)
        if ((Get-Sha256 (Join-Path $stage $file.Name)) -ne $file.Sha256) { throw "written $($file.Name) has the wrong hash." }
    }
    if ($env:GUNDAM_WIN11PATCH_TEST_FAIL_IMPORT96 -eq "expand") { throw "test failure injected after the QuickTime expansion" }

    # Same volume, so each move is a rename. gundam.exe goes last: until then
    # the folder is not recognized as a game folder.
    $entries = @(Get-ChildItem -LiteralPath $stage -Force | Sort-Object { $_.Name -ieq "gundam.exe" }, Name)
    foreach ($entry in $entries) {
        if ($keep.ContainsKey($entry.Name.ToUpperInvariant())) { continue }
        $target = Join-Path $dest $entry.Name
        Move-Item -LiteralPath $entry.FullName -Destination $target
        $moved.Add($target)
        if ($moved.Count -eq 3 -and $env:GUNDAM_WIN11PATCH_TEST_FAIL_IMPORT96 -eq "move") { throw "test failure injected during the 1996 move" }
    }
    Remove-Item -LiteralPath $stage -Recurse -Force
    Write-Host "1996 edition copied to $dest"
    return [pscustomobject]@{ Destination = $dest; Created = $moved.ToArray(); CreatedDestination = $createdDest }
} catch {
    $failure = $_
    $left = New-Object System.Collections.Generic.List[string]
    foreach ($path in @($moved) + $stage) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path -LiteralPath $path) { $left.Add($path) }
    }
    if ($createdDest -and (Test-Path -LiteralPath $dest) -and -not (Get-ChildItem -LiteralPath $dest -Force)) {
        Remove-Item -LiteralPath $dest -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $dest) { $left.Add($dest) }
    }
    if ($left.Count -gt 0) {
        throw ("1996 import rollback incomplete; remove these by hand: {0}. Original error: {1}" -f ($left -join ", "), $failure.Exception.Message)
    }
    throw $failure
}
