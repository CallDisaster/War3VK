param(
  [Parameter(Mandatory=$true)][int]$TargetPid,
  [Parameter(Mandatory=$true)][string]$ActionsPath,
  [string]$StatusPath = ""
)

$ErrorActionPreference = "Stop"
trap {
  if ($StatusPath) {
    Set-Content -LiteralPath $StatusPath -Value ("ERROR:" + $_.Exception.Message) -Encoding UTF8
  }
  exit 1
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class War3DesktopInputPlan {
  public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

  [StructLayout(LayoutKind.Sequential)]
  public struct RECT {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
  }

  [StructLayout(LayoutKind.Sequential)]
  public struct POINT {
    public int X;
    public int Y;
  }

  [DllImport("user32.dll")]
  public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
  [DllImport("user32.dll")]
  public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll")]
  public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")]
  public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")]
  public static extern bool BringWindowToTop(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")]
  public static extern IntPtr SetActiveWindow(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern IntPtr SetFocus(IntPtr hwnd);
  [DllImport("kernel32.dll")]
  public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")]
  public static extern bool AttachThreadInput(uint from, uint to, bool attach);
  [DllImport("user32.dll")]
  public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extraInfo);
  [DllImport("user32.dll")]
  public static extern uint MapVirtualKey(uint code, uint mapType);
  [DllImport("user32.dll")]
  public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")]
  public static extern IntPtr SendMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")]
  public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);
  [DllImport("user32.dll")]
  public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")]
  public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);
  [DllImport("user32.dll")]
  public static extern int GetSystemMetrics(int index);
  [DllImport("user32.dll")]
  public static extern IntPtr GetThreadDesktop(uint threadId);
  [DllImport("user32.dll", SetLastError=true)]
  public static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint desiredAccess);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern bool GetUserObjectInformation(
    IntPtr handle, int index, System.Text.StringBuilder info, uint length, out uint needed);
  [DllImport("user32.dll")]
  public static extern bool CloseDesktop(IntPtr desktop);

  public static string DesktopName(IntPtr desktop) {
    if (desktop == IntPtr.Zero) return "";
    uint needed;
    GetUserObjectInformation(desktop, 2, null, 0, out needed);
    if (needed == 0) return "";
    var value = new System.Text.StringBuilder((int)(needed / 2 + 2));
    if (!GetUserObjectInformation(desktop, 2, value,
        (uint)(value.Capacity * 2), out needed)) return "";
    return value.ToString();
  }

  public static IntPtr FindLargestWindow(uint targetPid) {
    IntPtr best = IntPtr.Zero;
    long bestArea = 0;
    EnumWindows((hwnd, unused) => {
      uint pid;
      RECT rect;
      GetWindowThreadProcessId(hwnd, out pid);
      if (pid == targetPid && IsWindowVisible(hwnd) && GetWindowRect(hwnd, out rect)) {
        long area = Math.Max(0, rect.Right - rect.Left) * (long)Math.Max(0, rect.Bottom - rect.Top);
        if (area > bestArea) {
          bestArea = area;
          best = hwnd;
        }
      }
      return true;
    }, IntPtr.Zero);
    return best;
  }

  public static bool PulseKey(IntPtr hwnd, byte vk, int holdMs) {
    byte scan = (byte)(MapVirtualKey(vk, 0) & 0xFF);
    keybd_event(vk, scan, 0, UIntPtr.Zero);
    bool down = PostMessage(hwnd, 0x0100, (IntPtr)vk, (IntPtr)(1 | (scan << 16)));
    Thread.Sleep(Math.Max(20, holdMs));
    keybd_event(vk, scan, 0x0002, UIntPtr.Zero);
    bool up = PostMessage(hwnd, 0x0101, (IntPtr)vk,
      (IntPtr)(1 | (scan << 16) | (1 << 30) | unchecked((int)0x80000000)));
    return down && up;
  }

  public static bool ClickClient(IntPtr hwnd, int x, int y, bool right, int holdMs, int count) {
    RECT client;
    if (!GetClientRect(hwnd, out client)) return false;
    int width = Math.Max(1, client.Right - client.Left);
    int height = Math.Max(1, client.Bottom - client.Top);
    x = Math.Max(0, Math.Min(width - 1, x));
    y = Math.Max(0, Math.Min(height - 1, y));
    int packed = (y << 16) | (x & 0xFFFF);
    uint downMessage = right ? 0x0204u : 0x0201u;
    uint upMessage = right ? 0x0205u : 0x0202u;
    IntPtr downState = right ? (IntPtr)0x0002 : (IntPtr)0x0001;
    uint downFlag = right ? 0x0008u : 0x0002u;
    uint upFlag = right ? 0x0010u : 0x0004u;
    bool nativeInputReady = BringWindowToTop(hwnd);
    nativeInputReady = (SetForegroundWindow(hwnd) || GetForegroundWindow() == hwnd) &&
      nativeInputReady;
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    Thread.Sleep(45);
    bool ok = true;
    try {
      POINT screenPoint = new POINT { X = x, Y = y };
      nativeInputReady = nativeInputReady && ClientToScreen(hwnd, ref screenPoint);
      if (nativeInputReady) {
        int screenWidth = width;
        int screenHeight = height;
        uint absoluteX = (uint)Math.Max(0, Math.Min(65535,
          (int)Math.Round(screenPoint.X * 65535.0 / Math.Max(1, screenWidth - 1))));
        uint absoluteY = (uint)Math.Max(0, Math.Min(65535,
          (int)Math.Round(screenPoint.Y * 65535.0 / Math.Max(1, screenHeight - 1))));
        // 1.27a 的隔离桌面输入只稳定接受绝对鼠标包。右下 -> 左上
        // 先把 DirectInput 的累积光标归一化，再发送目标绝对坐标。
        mouse_event(0x0001u | 0x8000u, 65535u, 65535u, 0, UIntPtr.Zero);
        Thread.Sleep(35);
        mouse_event(0x0001u | 0x8000u, 0u, 0u, 0, UIntPtr.Zero);
        Thread.Sleep(35);
        mouse_event(0x0001u | 0x8000u, absoluteX, absoluteY, 0, UIntPtr.Zero);
        Thread.Sleep(90);
      }
      for (int i = 0; i < Math.Max(1, count); ++i) {
        ok = PostMessage(hwnd, 0x0200, IntPtr.Zero, (IntPtr)packed) && ok;
        if (nativeInputReady) mouse_event(downFlag, 0, 0, 0, UIntPtr.Zero);
        // DirectInput 负责更新 War3 内部光标；实际按钮激活仍由窗口过程
        // 消费 WM_*BUTTON 消息。同步 SendMessage 保证窗口过程能在注入的
        // 原生按钮仍处于按下状态时处理 down/up，避免队列调度造成丢击。
        SendMessage(hwnd, downMessage, downState, (IntPtr)packed);
        Thread.Sleep(Math.Max(20, holdMs));
        if (nativeInputReady) mouse_event(upFlag, 0, 0, 0, UIntPtr.Zero);
        SendMessage(hwnd, upMessage, IntPtr.Zero, (IntPtr)packed);
        Thread.Sleep(45);
      }
    } finally { }
    return ok && nativeInputReady;
  }
}
"@

[void][War3DesktopInputPlan]::SetProcessDPIAware()
$actions = Get-Content -Raw -LiteralPath $ActionsPath | ConvertFrom-Json
$hwnd = [War3DesktopInputPlan]::FindLargestWindow([uint32]$TargetPid)
if ($hwnd -eq [IntPtr]::Zero) {
  throw "target window not found"
}

[uint32]$actualPid = 0
$targetThread = [War3DesktopInputPlan]::GetWindowThreadProcessId($hwnd, [ref]$actualPid)
$currentThread = [War3DesktopInputPlan]::GetCurrentThreadId()
$helperDesktop = [War3DesktopInputPlan]::GetThreadDesktop($currentThread)
$inputDesktop = [War3DesktopInputPlan]::OpenInputDesktop(0, $false, [uint32]0x0001)
$attached = $false
$top = $false
$foreground = $false
$ok = $true
$executed = 0
try {
  if ($helperDesktop -eq [IntPtr]::Zero -or $inputDesktop -eq [IntPtr]::Zero) {
    throw "unable to resolve input desktops"
  }
  $helperDesktopName = [War3DesktopInputPlan]::DesktopName($helperDesktop)
  $inputDesktopName = [War3DesktopInputPlan]::DesktopName($inputDesktop)
  if (-not $helperDesktopName -or -not $inputDesktopName) {
    throw "unable to prove desktop identities"
  }
  if (-not [String]::Equals(
      $helperDesktopName, $inputDesktopName,
      [StringComparison]::OrdinalIgnoreCase)) {
    throw "non-input desktop injection is forbidden"
  }

  $attached = [War3DesktopInputPlan]::AttachThreadInput($currentThread, $targetThread, $true)
  $top = [War3DesktopInputPlan]::BringWindowToTop($hwnd)
  $foreground = [War3DesktopInputPlan]::SetForegroundWindow($hwnd)
  [void][War3DesktopInputPlan]::SetActiveWindow($hwnd)
  [void][War3DesktopInputPlan]::SetFocus($hwnd)
  Start-Sleep -Milliseconds 80

  foreach ($action in $actions) {
    $kind = [string]$action.type
    if ($kind -eq "sleep") {
      Start-Sleep -Milliseconds ([Math]::Max(0, [int]$action.ms))
    } elseif ($kind -eq "key") {
      $ok = [War3DesktopInputPlan]::PulseKey(
        $hwnd,
        [byte]$action.vk,
        [Math]::Max(20, [int]$action.holdMs)
      ) -and $ok
    } elseif ($kind -eq "click") {
      $right = ([string]$action.button).ToLowerInvariant() -eq "right"
      $ok = [War3DesktopInputPlan]::ClickClient(
        $hwnd,
        [int]$action.x,
        [int]$action.y,
        $right,
        [Math]::Max(20, [int]$action.holdMs),
        [Math]::Max(1, [int]$action.count)
      ) -and $ok
    } else {
      throw ("unsupported input action type: " + $kind)
    }
    $executed += 1
  }
} finally {
  if ($attached) {
    [void][War3DesktopInputPlan]::AttachThreadInput($currentThread, $targetThread, $false)
  }
  if ($inputDesktop -ne [IntPtr]::Zero) {
    [void][War3DesktopInputPlan]::CloseDesktop($inputDesktop)
  }
}

$result = "hwnd=" + $hwnd.ToInt64() + ";actions=" + $executed +
  ";ok=" + $ok + ";attached=" + $attached + ";top=" + $top +
  ";foreground=" + $foreground + ";desktopMatched=True"
if ($StatusPath) {
  Set-Content -LiteralPath $StatusPath -Value ("OK:" + $result) -Encoding UTF8
}
Write-Output ("OK:" + $result)
if (-not $ok) {
  exit 2
}
