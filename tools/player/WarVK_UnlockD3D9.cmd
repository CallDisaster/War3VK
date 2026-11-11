@echo off
rem ============================================================
rem  WarVK：解除 WarVK_LockD3D9.cmd 加的锁定
rem  用法：放在 war3.exe 所在目录，双击运行。之后可以正常更新或删除 d3d9.dll。
rem ============================================================
setlocal EnableExtensions DisableDelayedExpansion
set "DIR=%~dp0"
set "TARGET=%~dp0d3d9.dll"
set "WARVK_SELF=%~f0"

fltmc >nul 2>&1
if errorlevel 1 (
  echo [WarVK] 需要管理员权限，正在请求提升……
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:WARVK_SELF -Verb RunAs"
  exit /b
)

title WarVK 解锁 d3d9.dll
echo [WarVK] 正在解锁：%DIR%
icacls "%DIR%." /remove:d *S-1-1-0 >nul 2>&1
if exist "%TARGET%" (
  icacls "%TARGET%" /remove:d *S-1-1-0 >nul 2>&1
  attrib -R "%TARGET%" >nul 2>&1
)
echo [WarVK] 已解锁，现在可以更新或删除 d3d9.dll。
if not defined WARVK_NO_PAUSE pause
exit /b 0
