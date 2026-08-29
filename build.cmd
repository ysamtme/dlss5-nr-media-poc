@echo off
setlocal

if not exist "%~dp0third_party\imgui\imgui.cpp" (
    echo ERROR: Dear ImGui submodule is missing. 1>&2
    echo Run: git submodule update --init --depth 1 1>&2
    exit /b 2
)

set "DLSSNR_SDK=%~1"
if not defined DLSSNR_SDK set "DLSSNR_SDK=%NGX_SDK_DIR%"
if not defined DLSSNR_SDK if exist "%~dp0.cache\NVIDIA-DLSS\include\nvsdk_ngx.h" set "DLSSNR_SDK=%~dp0.cache\NVIDIA-DLSS"

if not exist "%DLSSNR_SDK%\include\nvsdk_ngx.h" (
    echo ERROR: official NVIDIA DLSS SDK headers were not found. 1>&2
    echo Usage: build.cmd C:\path\to\NVIDIA-DLSS 1>&2
    echo Or set NGX_SDK_DIR to that directory. 1>&2
    exit /b 2
)

cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 -DNGX_SDK_DIR="%DLSSNR_SDK%"
if errorlevel 1 exit /b %ERRORLEVEL%
cmake --build "%~dp0build" --config Release --parallel
exit /b %ERRORLEVEL%
