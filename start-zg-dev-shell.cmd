@echo off
setlocal

set "ROOT=%~dp0"
set "MSYS_UCRT=C:\msys64\ucrt64\bin"
set "MSYS_USR=C:\msys64\usr\bin"

if not exist "%MSYS_UCRT%\gcc.exe" (
  echo gcc.exe not found: "%MSYS_UCRT%\gcc.exe"
  echo Run setup-zg-build-env.cmd first.
  exit /b 1
)

set "MSYSTEM=UCRT64"
set "MINGW_CHOST=x86_64-w64-mingw32"
set "MINGW_PREFIX=C:/msys64/ucrt64"
set "MINGW_PACKAGE_PREFIX=mingw-w64-ucrt-x86_64"
set "PATH=%MSYS_UCRT%;%MSYS_USR%;%PATH%"

start "ZG Build Shell" cmd /k "cd /d %ROOT% && echo UCRT64 build environment ready. && echo Use build-zg-inkscape.cmd to build."
