# Drive the recompiled client's window with real input (the game reads
# DirectInput, so this has to be genuine SendInput to a focused window).
#   powershell -File tools/input.ps1 click 522 525      # window-relative pixels (as in screenshot.ps1)
#   powershell -File tools/input.ps1 key W 2000          # hold a key (virtual-key name) for N ms
#   powershell -File tools/input.ps1 type "hello"        # type text, then nothing else
#   powershell -File tools/input.ps1 tap Return          # press and release one key
param([string]$Action, [string]$A, [string]$B)
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public class I {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  public struct RECT { public int L, T, R, B; }
}
"@
[I]::SetProcessDPIAware() | Out-Null
$p = Get-Process mxo_final, mxo_final_trace -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "no client window"; exit 1 }
$h = $p.MainWindowHandle
[I]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object I+RECT
[I]::GetWindowRect($h, [ref]$r) | Out-Null

# DirectInput keyboards read scan codes, so send the scan code with each key.
function Key([byte]$vk, [int]$holdMs) {
  $sc = [byte][I]::MapVirtualKey($vk, 0)
  [I]::keybd_event($vk, $sc, 0, [IntPtr]::Zero)
  Start-Sleep -Milliseconds $holdMs
  [I]::keybd_event($vk, $sc, 2, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 60
}
function Vk([string]$name) { [byte][System.Windows.Forms.Keys]::$name }

switch ($Action) {
  "click" {
    [I]::SetCursorPos($r.L + [int]$A, $r.T + [int]$B) | Out-Null
    Start-Sleep -Milliseconds 150
    [I]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
    [I]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero)
  }
  "key"  { Key (Vk $A) ([int]$B) }
  "tap"  { Key (Vk $A) 80 }
  "type" {
    foreach ($c in $A.ToCharArray()) {
      if ($c -eq ' ') { Key 0x20 50; continue }
      if ($c -match '[A-Za-z]') { Key ([byte][char]$c.ToString().ToUpper()) 50; continue }
      if ($c -match '[0-9]')    { Key ([byte][char]$c) 50; continue }
      if ($c -eq '/') { Key 0xBF 50; continue }
      if ($c -eq '.') { Key 0xBE 50; continue }
    }
  }
}
"$Action done"
