@echo off
setlocal
set "DLSSNR_EXE=%~dp0build\Release\dlssnr-image.exe"
if not exist "%DLSSNR_EXE%" (
    echo ERROR: executable not found: "%DLSSNR_EXE%" 1>&2
    exit /b 2
)
pushd "%~dp0"
"%DLSSNR_EXE%" %*
set "DLSSNR_EXIT_CODE=%ERRORLEVEL%"
popd
exit /b %DLSSNR_EXIT_CODE%
