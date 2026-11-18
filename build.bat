@echo off
setlocal

REM Build VexOS inside WSL from the directory containing this file.
where wsl.exe >nul 2>nul
if errorlevel 1 (
    echo [BUILD] Windows Subsystem for Linux (WSL) is required.
    echo [BUILD] Install WSL and a Linux distribution, then try again.
    exit /b 1
)

for /f "delims=" %%I in ('wsl.exe wslpath -u "%~dp0." 2^>nul') do set "WSL_DIR=%%I"
if not defined WSL_DIR (
    echo [BUILD] Could not resolve this folder inside WSL.
    exit /b 1
)

echo [BUILD] Running build.sh in WSL...
if defined NETLOG_DESTINATION (
    wsl.exe --cd "%WSL_DIR%" env NETLOG_DESTINATION="%NETLOG_DESTINATION%" bash ./build.sh
) else (
    wsl.exe --cd "%WSL_DIR%" bash ./build.sh
)

if errorlevel 1 (
    echo.
    echo [BUILD] Build failed. See the WSL output above.
    exit /b 1
)

echo.
echo [BUILD] Build completed successfully.
exit /b 0
