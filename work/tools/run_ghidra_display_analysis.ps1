param(
    [string]$GhidraRoot = $env:GHIDRA_INSTALL_DIR,
    [string]$Executable = (Join-Path $PSScriptRoot '..\..\source_exe_01\gundam.exe'),
    [string]$ProjectDirectory = (Join-Path $PSScriptRoot '..\analysis\ghidra_phase3'),
    [string]$ProjectName = 'GundamTacticsPhase3'
)

$ErrorActionPreference = 'Stop'
$headless = Join-Path $GhidraRoot 'support\analyzeHeadless.bat'
if (-not (Test-Path -LiteralPath $headless)) {
    Write-Error "Ghidra headless not found: $headless"
    exit 2
}

$scriptDirectory = Join-Path $PSScriptRoot '..\analysis\ghidra_scripts'
$output = Join-Path $PSScriptRoot '..\analysis\display_xrefs.raw.txt'
New-Item -ItemType Directory -Force -Path $ProjectDirectory | Out-Null
$ghidraUserRoot = Join-Path $PSScriptRoot '..\analysis\ghidra_user'
New-Item -ItemType Directory -Force -Path $ghidraUserRoot | Out-Null
$env:USERPROFILE = (Resolve-Path -LiteralPath $ghidraUserRoot).Path
$env:APPDATA = Join-Path $env:USERPROFILE 'AppData\Roaming'
$env:LOCALAPPDATA = Join-Path $env:USERPROFILE 'AppData\Local'
New-Item -ItemType Directory -Force -Path $env:APPDATA, $env:LOCALAPPDATA | Out-Null
$javaHome = 'C:\Program Files\Java\jdk-26'
if (Test-Path -LiteralPath (Join-Path $javaHome 'bin\java.exe')) {
    $env:JAVA_HOME = $javaHome
    $env:Path = (Join-Path $javaHome 'bin') + ';' + $env:Path
}

$ErrorActionPreference = 'Continue'
& $headless $ProjectDirectory $ProjectName `
    -import (Resolve-Path -LiteralPath $Executable) `
    -scriptPath (Resolve-Path -LiteralPath $scriptDirectory) `
    -postScript DisplayXrefs.java `
    -deleteProject 2>&1 | Tee-Object -FilePath $output
$ghidraExitCode = $LASTEXITCODE
if ($ghidraExitCode -ne 0) { exit $ghidraExitCode }
Write-Output "display xrefs raw output: $output"
