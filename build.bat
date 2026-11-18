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

REM Optional static IPv4 overrides; build.sh validates each address.
set "NETLOG_ENV="
if defined NETLOG_LOCAL_IP set "NETLOG_ENV=%NETLOG_ENV% NETLOG_LOCAL_IP=%NETLOG_LOCAL_IP%"
if defined NETLOG_NETMASK set "NETLOG_ENV=%NETLOG_ENV% NETLOG_NETMASK=%NETLOG_NETMASK%"
if defined NETLOG_GATEWAY set "NETLOG_ENV=%NETLOG_ENV% NETLOG_GATEWAY=%NETLOG_GATEWAY%"
if defined NETLOG_DESTINATION set "NETLOG_ENV=%NETLOG_ENV% NETLOG_DESTINATION=%NETLOG_DESTINATION%"

echo [BUILD] Running build.sh in WSL...
wsl.exe --cd "%WSL_DIR%" env %NETLOG_ENV% bash ./build.sh

if errorlevel 1 (
    echo.
    echo [BUILD] Build failed. See the WSL output above.
    exit /b 1
)

echo.
echo [BUILD] Build completed successfully.
exit /b 0
