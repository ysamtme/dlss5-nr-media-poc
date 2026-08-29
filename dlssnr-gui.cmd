@echo off
setlocal
set "DLSSNR_GUI=%~dp0build\Release\dlssnr-gui.exe"
if not exist "%DLSSNR_GUI%" (
    echo ERROR: GUI executable not found: "%DLSSNR_GUI%" 1>&2
    exit /b 2
)
pushd "%~dp0"
start "" "%DLSSNR_GUI%"
set "DLSSNR_EXIT_CODE=%ERRORLEVEL%"
popd
exit /b %DLSSNR_EXIT_CODE%
