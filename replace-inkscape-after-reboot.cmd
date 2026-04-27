@echo off
set SRC=D:\GIT\inkscape_ZG\inkscape\build-zg\bin\inkscape.exe
set DST=D:\GIT\inkscape_ZG\inkscape\build-zg\install_dir\bin\inkscape.exe
copy /Y "%SRC%" "%DST%"
start "" "%DST%"
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\RunOnce" /v ReplaceInkscapeZG /f >nul 2>nul
