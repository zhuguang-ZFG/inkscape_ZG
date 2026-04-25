@echo off
setlocal

set "ROOT=%~dp0"
set "BIN=%ROOT%build-zg\install_dir\bin"
set "MSYS_UCRT=C:\msys64\ucrt64\bin"
set "MSYS_USR=C:\msys64\usr\bin"

if not exist "%BIN%\inkscape.exe" (
  echo inkscape.exe not found: "%BIN%\inkscape.exe"
  exit /b 1
)

set "PATH=%BIN%;%MSYS_UCRT%;%MSYS_USR%;%PATH%"
start "" "%BIN%\inkscape.exe" %*
