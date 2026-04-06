@echo off
setlocal

rem Always switch to the repository root so MinGW is not launched from a
rem PowerShell \\?\ extended-path working directory.
cd /d "%~dp0"

rem Keep the imgui Meson shim in sync without dirtying the upstream submodule.
if not exist "subprojects\imgui\meson.build" (
  copy /y "subprojects\packagefiles\imgui\meson.build" "subprojects\imgui\meson.build" >nul
)

if "%~1"=="" (
  ninja -C build32 src/d3d9/d3d9.dll -j8
) else (
  ninja -C build32 %*
)

exit /b %errorlevel%
