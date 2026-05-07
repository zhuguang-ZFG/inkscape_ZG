@echo off
setlocal

set "ROOT=%~dp0"
set "APP_DIR=%ROOT%inkscape\build-zg\bin"
set "RUNTIME_DIR=%ROOT%inkscape\build-zg\install_dir\bin"
set "DATA_DIR=%ROOT%inkscape\build-zg\install_dir\share"
set "SHARED_DIR=%ROOT%inkscape\build-zg\share"
set "PROFILE_DIR=%ROOT%inkscape\build-zg\profile-zg"
set "MSYS_UCRT64_BIN=C:\msys64\ucrt64\bin"
set "MSYS_USR_BIN=C:\msys64\usr\bin"

if exist "%MSYS_UCRT64_BIN%\libstdc++-6.dll" set "PATH=%MSYS_UCRT64_BIN%;%PATH%"
if exist "%MSYS_USR_BIN%\msys-2.0.dll" set "PATH=%MSYS_USR_BIN%;%PATH%"
set "PATH=%APP_DIR%;%RUNTIME_DIR%;%PATH%"
set "INKSCAPE_DATADIR=%DATA_DIR%"
set "INKSCAPE_PROFILE_DIR=%PROFILE_DIR%"

if not exist "%PROFILE_DIR%" mkdir "%PROFILE_DIR%"

powershell -NoProfile -ExecutionPolicy Bypass ^
  "$prefsPath = Join-Path '%PROFILE_DIR%' 'preferences.xml';" ^
  "$sharedDir = [System.IO.Path]::GetFullPath('%SHARED_DIR%');" ^
  "if (Test-Path $prefsPath) { [xml]$xml = Get-Content -Path $prefsPath; } else { $xml = New-Object xml; $null = $xml.AppendChild($xml.CreateXmlDeclaration('1.0','UTF-8',$null)); $root = $xml.CreateElement('inkscape'); $root.SetAttribute('version','1'); $null = $xml.AppendChild($root); }" ^
  "$root = $xml.inkscape; if (-not $root) { throw 'preferences.xml missing inkscape root'; }" ^
  "$options = $root.SelectSingleNode(\"group[@id='options']\"); if (-not $options) { $options = $xml.CreateElement('group'); $options.SetAttribute('id','options'); $null = $root.AppendChild($options); }" ^
  "$resources = $options.SelectSingleNode(\"group[@id='resources']\"); if (-not $resources) { $resources = $xml.CreateElement('group'); $resources.SetAttribute('id','resources'); $null = $options.AppendChild($resources); }" ^
  "$shared = $resources.SelectSingleNode(\"group[@id='sharedpath']\"); if (-not $shared) { $shared = $xml.CreateElement('group'); $shared.SetAttribute('id','sharedpath'); $null = $resources.AppendChild($shared); }" ^
  "$shared.SetAttribute('value', $sharedDir);" ^
  "$xml.Save($prefsPath);"

start "" "%APP_DIR%\inkscape.exe"
