param(
  [Parameter(Mandatory=$true)][int]$TargetPid,
  [int]$VirtualKey = 32,
  [int]$HoldMs = 70,
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
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class War3DesktopKey {
  public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

  [StructLayout(LayoutKind.Sequential)]
  public struct RECT {
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
  }

  [DllImport("user32.dll")]
  public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll")]
  public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")]
  public static extern bool BringWindowToTop(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern IntPtr SetActiveWindow(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern IntPtr SetFocus(IntPtr hwnd);
  [DllImport("user32.dll")]
  public static extern IntPtr GetForegroundWindow();
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

  public static IntPtr FindLargestWindow(uint targetPid) {
    IntPtr best = IntPtr.Zero;
    long bestArea = 0;
    EnumWindows((hwnd, unused) => {
      uint pid;
      GetWindowThreadProcessId(hwnd, out pid);
      RECT rect;
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

  public static string Pulse(uint targetPid, byte vk, int holdMs) {
    IntPtr hwnd = FindLargestWindow(targetPid);
    if (hwnd == IntPtr.Zero) throw new InvalidOperationException("target window not found");
    uint targetThread;
    GetWindowThreadProcessId(hwnd, out targetPid);
    targetThread = GetWindowThreadProcessId(hwnd, out targetPid);
    uint currentThread = GetCurrentThreadId();
    bool attached = AttachThreadInput(currentThread, targetThread, true);
    bool top = BringWindowToTop(hwnd);
    bool foreground = SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    Thread.Sleep(80);

    byte scan = (byte)(MapVirtualKey(vk, 0) & 0xFF);
    keybd_event(vk, scan, 0, UIntPtr.Zero);
    PostMessage(hwnd, 0x0100, (IntPtr)vk, (IntPtr)(1 | (scan << 16)));
    Thread.Sleep(Math.Max(20, holdMs));
    keybd_event(vk, scan, 0x0002, UIntPtr.Zero);
    PostMessage(hwnd, 0x0101, (IntPtr)vk,
      (IntPtr)(1 | (scan << 16) | (1 << 30) | unchecked((int)0x80000000)));
    Thread.Sleep(50);
    IntPtr active = GetForegroundWindow();
    if (attached) AttachThreadInput(currentThread, targetThread, false);
    return "hwnd=" + hwnd.ToInt64() + ";attached=" + attached +
      ";top=" + top + ";foreground=" + foreground +
      ";active=" + active.ToInt64();
  }
}
"@

$result = [War3DesktopKey]::Pulse([uint32]$TargetPid, [byte]$VirtualKey, $HoldMs)
if ($StatusPath) {
  Set-Content -LiteralPath $StatusPath -Value ("OK:" + $result) -Encoding UTF8
}
Write-Output ("OK:" + $result)
