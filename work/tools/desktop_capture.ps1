param(
    [string]$GameDir = "run\GT01",
    [string]$OutDir = "work\analysis\screenshots\capture",
    [double[]]$Times = @(2.0, 2.5, 3.0, 3.5, 4.0),
    [hashtable]$Env = @{}
)
# Capture the game window from the composed desktop (CopyFromScreen) at given
# times after launch. Window-DC capture cannot see QuickTime/DCI direct draws
# or the real on-screen orientation, so use this for visual A/B.
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if (-not ([System.Management.Automation.PSTypeName]'U.W').Type) {
    Add-Type -MemberDefinition '[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); public struct RECT { public int L, T, R, B; }' -Name W -Namespace U
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$saved = @{}
foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, "Process"); [Environment]::SetEnvironmentVariable($k, $Env[$k], "Process") }
try {
    $p = Start-Process -FilePath (Join-Path $GameDir "gundam.exe") -WorkingDirectory $GameDir -PassThru
    $t0 = Get-Date
    foreach ($t in $Times) {
        $d = $t - ((Get-Date) - $t0).TotalSeconds
        if ($d -gt 0) { Start-Sleep -Milliseconds ([int]($d * 1000)) }
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $g.Dispose()
        $r = New-Object U.W+RECT
        $p.Refresh()
        $h = $p.MainWindowHandle
        $name = "t{0:00.0}.png" -f $t
        if ($h -ne 0 -and [U.W]::GetWindowRect($h, [ref]$r) -and ($r.R - $r.L) -gt 0) {
            $rect = New-Object System.Drawing.Rectangle $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T)
            $crop = $bmp.Clone($rect, $bmp.PixelFormat)
            $crop.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
            $crop.Dispose()
        } else {
            $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
        }
        $bmp.Dispose()
        Write-Host "captured $name"
    }
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], "Process") }
}
