param(
    [string]$GameDir = "run\GT01",
    [string]$OutDir = "work\analysis\screenshots\probe_run",
    # Sequence of steps: "wait:<sec>", "click:<fx>,<fy>" (window-relative 0..1), "snap:<name>", "key:<SendKeys>", "show:<SW_ code>" (3 maximize, 6 minimize, 9 restore)
    [string[]]$Steps = @("wait:5", "click:0.5,0.5", "wait:3", "snap:menu", "click:0.5,0.58", "wait:4", "snap:newgame_4s", "wait:6", "snap:newgame_10s", "wait:10", "snap:newgame_20s"),
    [switch]$Trace,
    [ValidateSet("desktop", "window")][string]$Capture = "desktop"
)
# Drive the game with mouse clicks and capture the real screen (CopyFromScreen)
# so QuickTime/DCI direct draws and orientation are visible. Prints a proxy
# trace summary when -Trace is set.
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
if (-not ([System.Management.Automation.PSTypeName]'U.G').Type) {
    Add-Type -MemberDefinition '[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r); [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr FindWindow(string cls, string title); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y); [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e); public struct RECT { public int L, T, R, B; }' -Name G -Namespace U
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
# System.Drawing resolves relative paths against the .NET current directory, not $PWD.
$OutDir = (Resolve-Path $OutDir).Path
$traceFile = Join-Path $GameDir "qtim_compat_trace.log"
if ($Trace) { Remove-Item $traceFile -ErrorAction SilentlyContinue; [Environment]::SetEnvironmentVariable("QTIM_COMPAT_TRACE", "1", "Process") }
function Get-GameHwnd($p) {
    $hwnd = [U.G]::FindWindow('Gundam', 'Gundam Tactics')
    if ($hwnd -ne [IntPtr]::Zero) { return $hwnd }
    $p.Refresh()
    if ($p.MainWindowHandle -ne 0) { return $p.MainWindowHandle }
    return [IntPtr]::Zero
}
function Get-Rect($p) {
    $r = New-Object U.G+RECT
    $hwnd = Get-GameHwnd $p
    if ($hwnd -ne [IntPtr]::Zero -and [U.G]::GetWindowRect($hwnd, [ref]$r)) { return $r }
    return $null
}
if (-not ([System.Management.Automation.PSTypeName]'U.PW').Type) {
    Add-Type -MemberDefinition '[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);' -Name PW -Namespace U
}
if (-not ([System.Management.Automation.PSTypeName]'U.SW').Type) {
    Add-Type -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);' -Name SW -Namespace U
}
if (-not ([System.Management.Automation.PSTypeName]'U.FG').Type) {
    Add-Type -MemberDefinition '[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();' -Name FG -Namespace U
}
function Snap($p, $name) {
    try { SnapInner $p $name } catch { Write-Host ("snap {0} failed: {1}" -f $name, $_.Exception.Message) }
}
function SnapInner($p, $name) {
    if ($Capture -eq "window") {
        $r = Get-Rect $p
        if ($r) {
            $w = $r.R - $r.L; $h = $r.B - $r.T
            $bmp = New-Object System.Drawing.Bitmap $w, $h
            $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
            [U.PW]::PrintWindow((Get-GameHwnd $p), $hdc, 2) | Out-Null
            $g.ReleaseHdc($hdc); $g.Dispose()
            $bmp.Save((Join-Path $OutDir "$name.png")); $bmp.Dispose()
        }
        Write-Host ("snap(window) {0} alive={1}" -f $name, (-not $p.HasExited)); return
    }
    # The desktop grab would include whatever covers the game window, so only
    # capture while the game is the foreground window.
    $hwnd = Get-GameHwnd $p
    [U.G]::SetForegroundWindow($hwnd) | Out-Null; Start-Sleep -Milliseconds 300
    if ([U.FG]::GetForegroundWindow() -ne $hwnd) { Write-Host ("snap {0} skipped: game window is not in the foreground" -f $name); return }
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size); $g.Dispose()
    $r = Get-Rect $p
    if ($r) {
        $rect = New-Object System.Drawing.Rectangle $r.L, $r.T, ($r.R - $r.L), ($r.B - $r.T)
        $rect.Intersect($b)
        if ($rect.Width -gt 0 -and $rect.Height -gt 0) { $c = $bmp.Clone($rect, $bmp.PixelFormat); $c.Save((Join-Path $OutDir "$name.png")); $c.Dispose() }
        else { $bmp.Save((Join-Path $OutDir "$name.png")) }
    } else { $bmp.Save((Join-Path $OutDir "$name.png")) }
    $bmp.Dispose()
    Write-Host ("snap {0} alive={1}" -f $name, (-not $p.HasExited))
}
# Drags the bottom-right resize corner by (dx, dy) pixels over about a second
# with the real mouse, then reports the window size before and after.
function DragCorner($p, $dx, $dy) {
    $r = Get-Rect $p; if (-not $r) { return }
    $x = $r.R - 4; $y = $r.B - 4
    [U.G]::SetForegroundWindow((Get-GameHwnd $p)) | Out-Null
    [U.G]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 100
    $t0 = Get-Date
    [U.G]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero)
    for ($i = 1; $i -le 10; $i++) {
        Start-Sleep -Milliseconds 100
        [U.G]::SetCursorPos($x + [int]($dx * $i / 10), $y + [int]($dy * $i / 10)) | Out-Null
    }
    [U.G]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 300
    $a = Get-Rect $p
    Write-Host ("drag {0}x{1} -> {2}x{3} in {4:N0} ms" -f ($r.R - $r.L), ($r.B - $r.T), ($a.R - $a.L), ($a.B - $a.T), ((Get-Date) - $t0).TotalMilliseconds)
}
function ClickAt($p, $fx, $fy) {
    $r = Get-Rect $p; if (-not $r) { return }
    $hwnd = Get-GameHwnd $p
    $client = New-Object U.G+RECT
    if ($hwnd -eq [IntPtr]::Zero -or -not [U.G]::GetClientRect($hwnd, [ref]$client)) { return }
    $cx = [int]($client.R * $fx); $cy = [int]($client.B * $fy)
    $x = [int]($r.L + ($r.R - $r.L) * $fx); $y = [int]($r.T + ($r.B - $r.T) * $fy)
    [U.G]::SetForegroundWindow($hwnd) | Out-Null
    [U.G]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 200
    $lparam = [IntPtr](($cy -shl 16) -bor ($cx -band 0xffff))
    [U.G]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero)
    [U.G]::PostMessage($hwnd, 0x0200, [IntPtr]::Zero, $lparam) | Out-Null
    [U.G]::PostMessage($hwnd, 0x0201, [IntPtr]1, $lparam) | Out-Null
    Start-Sleep -Milliseconds 100
    [U.G]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero)
    [U.G]::PostMessage($hwnd, 0x0202, [IntPtr]::Zero, $lparam) | Out-Null
}
$p = Start-Process -FilePath (Join-Path $GameDir "gundam.exe") -WorkingDirectory $GameDir -PassThru
if ($Trace) { [Environment]::SetEnvironmentVariable("QTIM_COMPAT_TRACE", $null, "Process") }
try {
    foreach ($s in $Steps) {
        $kind, $arg = $s -split ":", 2
        switch ($kind) {
            "wait"  { Start-Sleep -Milliseconds ([int]([double]$arg * 1000)) }
            "click" { $fx, $fy = $arg -split ","; ClickAt $p ([double]$fx) ([double]$fy) }
            "snap"  { Snap $p $arg }
            "show"  { [U.SW]::ShowWindow((Get-GameHwnd $p), [int]$arg) | Out-Null }
            "drag"  { $dx, $dy = $arg -split ","; DragCorner $p ([int]$dx) ([int]$dy) }
            "key"   { [U.G]::SetForegroundWindow((Get-GameHwnd $p)) | Out-Null; [System.Windows.Forms.SendKeys]::SendWait($arg) }
        }
    }
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Seconds 1
}
if ($Trace -and (Test-Path $traceFile)) {
    "--- trace summary"
    Get-Content $traceFile | ForEach-Object { ($_ -split "`t")[1] } | Group-Object | Sort-Object Count -Descending | Select-Object -First 20 Count, Name | Format-Table -AutoSize
    "--- movie paths"
    Select-String -Path $traceFile -Pattern 'map_movie' | ForEach-Object { $_.Line } | Select-Object -First 20
}
