@echo off
rem WarVK: start Warcraft III with frame evidence on (it is off by default).
rem Put this file beside war3.exe and d3d9.dll and double-click it.
rem When a picture fault shows up, press Ctrl+Shift+C; the files go to WarVK\Evidence.
rem Frame evidence keeps recent frames in video memory (at most 1 GiB, at most
rem an eighth of the card's memory) and costs about 0.4 ms per frame.
rem Start the game normally again to turn it off.
setlocal
set "WARVK_D3D9_EVIDENCE=1"
if not exist "%~dp0war3.exe" (
  echo [WarVK] war3.exe was not found next to this file: %~dp0
  pause
  exit /b 1
)
start "" /D "%~dp0" "%~dp0war3.exe" %*
