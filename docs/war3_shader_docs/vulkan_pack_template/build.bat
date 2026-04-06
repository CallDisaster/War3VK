@echo off
setlocal

where glslc >nul 2>nul
if errorlevel 1 (
  echo glslc not found. Install the Vulkan SDK or add glslc to PATH.
  exit /b 1
)

pushd %~dp0

glslc composite.frag -o composite.spv
if errorlevel 1 goto :fail
glslc final.frag -o final.spv
if errorlevel 1 goto :fail
glslc shadow_receiver.frag -o shadow_receiver.spv
if errorlevel 1 goto :fail

echo ShaderPack SPIR-V files generated.
popd
exit /b 0

:fail
echo Build failed. Check glslc output.
popd
exit /b 1
