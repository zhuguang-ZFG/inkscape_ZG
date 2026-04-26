@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "PROJECT_DIR=%ROOT%inkscape"
set "BUILD_DIR=%PROJECT_DIR%\build-zg"
set "MSYS_ROOT=C:\msys64"
set "MSYS_BASH=%MSYS_ROOT%\usr\bin\bash.exe"
set "MSYS_UCRT=%MSYS_ROOT%\ucrt64\bin"
set "PACKAGE_FILE=%ROOT%msys2-ucrt64-packages.txt"
set "DO_UPGRADE=0"
set "DO_CONFIGURE=1"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--upgrade" (
  set "DO_UPGRADE=1"
  shift
  goto :parse_args
)
if /I "%~1"=="--no-configure" (
  set "DO_CONFIGURE=0"
  shift
  goto :parse_args
)
echo Unknown option: %~1
echo Usage: %~nx0 [--upgrade] [--no-configure]
exit /b 1

:args_done
if not exist "%MSYS_BASH%" (
  echo MSYS2 bash not found: "%MSYS_BASH%"
  echo Install MSYS2 to C:\msys64 first, then rerun this script.
  exit /b 1
)

if not exist "%PACKAGE_FILE%" (
  echo Package file not found: "%PACKAGE_FILE%"
  exit /b 1
)

echo Ensuring MSYS2 UCRT64 build packages are installed...
if "%DO_UPGRADE%"=="1" (
  "%MSYS_BASH%" -lc "pacman -Syu --noconfirm"
  if errorlevel 1 exit /b %ERRORLEVEL%
)

set "PACKAGE_FILE_WIN=%PACKAGE_FILE%"
"%MSYS_BASH%" -lc "package_file=$(cygpath -u \"$PACKAGE_FILE_WIN\"); pacman -S --needed --noconfirm $(grep -v '^[[:space:]]*$' \"$package_file\" | tr '\r\n' '  ')"
if errorlevel 1 exit /b %ERRORLEVEL%

if "%DO_CONFIGURE%"=="0" (
  echo Package installation finished.
  exit /b 0
)

echo Configuring build directory...
call "%ROOT%build-zg-inkscape.cmd" --reconfigure --configure-only
exit /b %ERRORLEVEL%
