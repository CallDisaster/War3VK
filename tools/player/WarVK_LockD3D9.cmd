@echo off
rem ============================================================
rem  WarVK：锁定本目录下的 d3d9.dll
rem  用法：把本文件放在 war3.exe 和 d3d9.dll 所在的目录，双击运行。
rem  效果：任何程序（包括对战平台、清理工具）都不能删除、改名或覆盖
rem        这个 d3d9.dll；游戏照常读取和加载它。
rem  更新或删除 d3d9.dll 之前，先运行 WarVK_UnlockD3D9.cmd。
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

title WarVK 锁定 d3d9.dll
if not exist "%TARGET%" (
  echo [WarVK] 这个目录里没有 d3d9.dll：
  echo   %DIR%
  echo 请把本文件放到 war3.exe 和 d3d9.dll 所在的目录再运行。
  goto done_fail
)

echo [WarVK] 正在锁定：%TARGET%
rem 先去掉旧的限制，保证重复运行结果一致。
icacls "%TARGET%" /remove:d *S-1-1-0 >nul 2>&1
icacls "%DIR%." /remove:d *S-1-1-0 >nul 2>&1

rem 文件本身：禁止删除、写入、追加、改属性（读取和加载不受影响）。
icacls "%TARGET%" /deny *S-1-1-0:(DE,WD,AD,WA,WEA) >nul
if errorlevel 1 goto lock_error
rem 所在目录：禁止“经目录删除子文件”，否则管理员仍可绕过文件上的限制删除它。
rem 只作用于这个目录本身，不继承；其他文件照常可以删除和更新。
icacls "%DIR%." /deny *S-1-1-0:(DC) >nul
if errorlevel 1 goto lock_error

rem 自检：试着改名，改名成功说明锁定无效。
ren "%TARGET%" "d3d9.dll.warvk-locktest" >nul 2>&1
if exist "%DIR%d3d9.dll.warvk-locktest" (
  ren "%DIR%d3d9.dll.warvk-locktest" "d3d9.dll" >nul 2>&1
  echo [WarVK] 自检失败：d3d9.dll 仍然可以被改名，锁定没有生效。
  goto done_fail
)

echo.
echo [WarVK] 已锁定。对战平台等程序无法再删除、改名或覆盖 d3d9.dll。
echo 要更新或删除 d3d9.dll，请先运行 WarVK_UnlockD3D9.cmd。
echo 如果平台在启动时直接结束游戏进程，或者检测到文件删不掉就拒绝启动，
echo 锁定文件解决不了，需要换平台或在平台设置里关闭对应的检查。
goto done_ok

:lock_error
echo [WarVK] 设置权限失败（icacls 返回错误）。请确认磁盘是 NTFS 格式，并用管理员身份运行。
goto done_fail

:done_ok
if not defined WARVK_NO_PAUSE pause
exit /b 0

:done_fail
if not defined WARVK_NO_PAUSE pause
exit /b 1
