# Capture photon-next WebGL 3D with RenderDoc (Chrome GPU process, not Photon.exe).
# Usage: .\web\scripts\renderdoc-capture.ps1

$ErrorActionPreference = "Continue"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
# The web-release preset's binaryDir (CMakePresets.json).
$Web = Join-Path $Root "..\out\build\web-release"
$Html = Join-Path $Web "photon.html"
$Captures = Join-Path $Web "captures"
$Profile = Join-Path $Web "chrome-rdc"
$Chrome = "C:\Program Files\Google\Chrome\Application\chrome.exe"
$Cmd = "C:\Program Files\RenderDoc\renderdoccmd.exe"
$Ui = "C:\Program Files\RenderDoc\qrenderdoc.exe"
$Port = 8080
$Url = "http://127.0.0.1:$Port/photon.html?screen=scene3d"

if (-not (Test-Path $Html)) { throw "photon.html missing. Run: photon build client --ui web" }
if (-not (Test-Path $Chrome)) { throw "Chrome not found at $Chrome" }
if (-not (Test-Path $Cmd)) { throw "RenderDoc not found at $Cmd" }

New-Item -ItemType Directory -Force -Path $Captures, $Profile | Out-Null

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class RdcWin {
  public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
}
"@

function Get-ChromeGpuPid {
    $browser = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
        Where-Object {
            $_.CommandLine -and
            ($_.CommandLine -like "*chrome-rdc*") -and
            ($_.CommandLine -notlike "*--type=*")
        } |
        Select-Object -First 1
    if (-not $browser) { return $null }
    $gpu = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" |
        Where-Object {
            $_.ParentProcessId -eq $browser.ProcessId -and
            $_.CommandLine -and
            ($_.CommandLine -like "*--type=gpu-process*")
        } |
        Select-Object -First 1
    if ($gpu) { return [int]$gpu.ProcessId }
    return $null
}

function Dismiss-GpuDialog {
    param([int]$ProcessId)
    $script:rdcPid = $ProcessId
    $script:rdcHit = $false
    $cb = [RdcWin+EnumProc] {
        param($h, $lp)
        $wp = [uint32]0
        [void][RdcWin]::GetWindowThreadProcessId($h, [ref]$wp)
        if (($wp -eq $script:rdcPid) -and [RdcWin]::IsWindowVisible($h)) {
            $sb = New-Object System.Text.StringBuilder 512
            [void][RdcWin]::GetWindowText($h, $sb, $sb.Capacity)
            Write-Host ("GPU window: '" + $sb.ToString() + "'")
            [void][RdcWin]::ShowWindow($h, 9)
            [void][RdcWin]::SetForegroundWindow($h)
            [void][RdcWin]::PostMessage($h, 0x100, [IntPtr]0x0D, [IntPtr]::Zero)
            [void][RdcWin]::PostMessage($h, 0x101, [IntPtr]0x0D, [IntPtr]::Zero)
            $script:rdcHit = $true
        }
        return $true
    }
    [void][RdcWin]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:rdcHit
}

$listen = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if (-not $listen) {
    Write-Host "Starting static server on :$Port"
    Start-Process -FilePath "python" -ArgumentList "-m","http.server","$Port","--directory",$Web `
        -WorkingDirectory $Web -WindowStyle Hidden | Out-Null
    Start-Sleep -Seconds 1
} else {
    Write-Host ("Already serving on :" + $Port)
}

$before = @(Get-ChildItem $Captures -Filter "*.rdc" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })

$env:RENDERDOC_HOOK_EGL = "0"
$chromeArgs = @(
    "--user-data-dir=$Profile",
    "--gpu-startup-dialog",
    "--disable-direct-composition",
    "--disable-gpu-sandbox",
    "--no-sandbox",
    "--disable-gpu-watchdog",
    "--use-angle=d3d11",
    "--no-first-run",
    "--no-default-browser-check",
    "--disable-extensions",
    "--disable-sync",
    "--window-size=1440,900",
    $Url
)
Write-Host "Launching Chrome GPU-paused for photon-next 3D"
Start-Process -FilePath $Chrome -ArgumentList $chromeArgs | Out-Null

$gpuPid = $null
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 500
    $gpuPid = Get-ChromeGpuPid
    if ($gpuPid) { break }
}
if (-not $gpuPid) {
    throw "Chrome GPU process did not start."
}
Write-Host ("Chrome GPU pid=" + $gpuPid + " - injecting RenderDoc")

$capTemplate = Join-Path $Captures "photon-next"
$injectOut = & $Cmd inject ("--PID=" + $gpuPid) ("--capture-file=" + $capTemplate) --opt-ref-all-resources --opt-capture-callstacks 2>&1 | Out-String
Write-Host $injectOut
if ($injectOut -notmatch "Injecting into PID") {
    throw "renderdoccmd inject failed"
}

Start-Sleep -Milliseconds 500
if (-not (Dismiss-GpuDialog -ProcessId $gpuPid)) {
    Write-Host "No GPU dialog found - sending Enter"
    $wshell = New-Object -ComObject WScript.Shell
    $wshell.SendKeys("{ENTER}")
}

Write-Host "Waiting for photon-next 3D to draw..."
Start-Sleep -Seconds 14

$wshell = New-Object -ComObject WScript.Shell
[void]$wshell.AppActivate("photon")
Start-Sleep -Milliseconds 500
Write-Host "Sending F12 (RenderDoc capture)"
$wshell.SendKeys("{F12}")
Start-Sleep -Seconds 2
$wshell.SendKeys("{PRTSC}")
Start-Sleep -Seconds 5

$newest = Get-ChildItem $Captures -Filter "*.rdc" -ErrorAction SilentlyContinue |
    Where-Object { $before -notcontains $_.FullName } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $newest) {
    $newest = Get-ChildItem $Captures -Filter "*.rdc" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
if (-not $newest) {
    Write-Host ("No .rdc yet under " + $Captures + " - press F12 in Chrome if the RenderDoc overlay is visible.")
} else {
    $mb = [math]::Round($newest.Length / 1MB, 1)
    Write-Host ("Capture: " + $newest.FullName + " (" + $mb + " MB)")
    if (Test-Path $Ui) {
        Start-Process $Ui -ArgumentList $newest.FullName
    }
}

Write-Host ("Done. Chrome GPU pid=" + $gpuPid)
