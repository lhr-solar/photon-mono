param([int]$GpuPid = 35700)

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class RdcWin2 {
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

$script:hit = $false
$cb = [RdcWin2+EnumProc] {
    param($h, $lp)
    $wp = [uint32]0
    [void][RdcWin2]::GetWindowThreadProcessId($h, [ref]$wp)
    if (($wp -eq $GpuPid) -and [RdcWin2]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 512
        [void][RdcWin2]::GetWindowText($h, $sb, $sb.Capacity)
        Write-Host ("WIN pid=" + $wp + " title='" + $sb.ToString() + "'")
        [void][RdcWin2]::ShowWindow($h, 9)
        [void][RdcWin2]::SetForegroundWindow($h)
        [void][RdcWin2]::PostMessage($h, 0x100, [IntPtr]0x0D, [IntPtr]::Zero)
        [void][RdcWin2]::PostMessage($h, 0x101, [IntPtr]0x0D, [IntPtr]::Zero)
        $script:hit = $true
    }
    return $true
}
[void][RdcWin2]::EnumWindows($cb, [IntPtr]::Zero)
$w = New-Object -ComObject WScript.Shell
$w.SendKeys("{ENTER}")
Write-Host ("dismissed hit=" + $script:hit)
