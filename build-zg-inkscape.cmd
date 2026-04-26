@echo off
setlocal

set "ROOT=%~dp0"
set "PROJECT_DIR=%ROOT%inkscape"
set "BUILD_DIR=%PROJECT_DIR%\build-zg"
set "MSYS_UCRT=C:\msys64\ucrt64\bin"
set "MSYS_USR=C:\msys64\usr\bin"
set "CMAKE_EXE=%MSYS_UCRT%\cmake.exe"
set "NINJA_EXE=%MSYS_UCRT%\ninja.exe"
set "RECONFIGURE=0"
set "CONFIGURE_ONLY=0"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--reconfigure" (
  set "RECONFIGURE=1"
  shift
  goto :parse_args
)
if /I "%~1"=="--configure-only" (
  set "CONFIGURE_ONLY=1"
  shift
  goto :parse_args
)
goto :args_done

:args_done

if not exist "%MSYS_UCRT%\gcc.exe" (
  echo gcc.exe not found: "%MSYS_UCRT%\gcc.exe"
  echo Run setup-zg-build-env.cmd first.
  exit /b 1
)

if not exist "%CMAKE_EXE%" (
  echo cmake.exe not found: "%CMAKE_EXE%"
  echo Run setup-zg-build-env.cmd first.
  exit /b 1
)

if not exist "%NINJA_EXE%" (
  echo ninja.exe not found: "%NINJA_EXE%"
  echo Run setup-zg-build-env.cmd first.
  exit /b 1
)

set "MSYSTEM=UCRT64"
set "MINGW_CHOST=x86_64-w64-mingw32"
set "MINGW_PREFIX=C:/msys64/ucrt64"
set "MINGW_PACKAGE_PREFIX=mingw-w64-ucrt-x86_64"
set "PATH=%MSYS_UCRT%;%MSYS_USR%;%PATH%"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

if "%RECONFIGURE%"=="1" del /f /q "%BUILD_DIR%\CMakeCache.txt" >nul 2>nul

if "%RECONFIGURE%"=="1" goto :configure
if not exist "%BUILD_DIR%\build.ninja" goto :configure
goto :build

:configure
echo Configuring build directory...
"%CMAKE_EXE%" -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe ^
  -DCMAKE_INSTALL_PREFIX="%BUILD_DIR%\install_dir" ^
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if errorlevel 1 exit /b %ERRORLEVEL%
if "%CONFIGURE_ONLY%"=="1" exit /b 0

:build
if "%~1"=="" (
  "%NINJA_EXE%" -C "%BUILD_DIR%" inkscape
) else (
  "%NINJA_EXE%" -C "%BUILD_DIR%" %*
)

exit /b %ERRORLEVEL%
