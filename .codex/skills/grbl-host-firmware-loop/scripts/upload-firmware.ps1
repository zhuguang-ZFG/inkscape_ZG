param(
    [string]$Port = "COM3",
    [string]$Environment = "release",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$skillDir = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $skillDir))
$firmwareRoot = Join-Path $repoRoot "Grbl_Esp32"

if (-not (Test-Path (Join-Path $firmwareRoot "platformio.ini"))) {
    throw "Firmware root not found or missing platformio.ini: $firmwareRoot"
}

$commandText = "platformio run -e $Environment -t upload --upload-port $Port"

if ($DryRun) {
    Write-Output "Set-Location `"$firmwareRoot`""
    Write-Output $commandText
    exit 0
}

Push-Location $firmwareRoot
try {
    platformio run -e $Environment -t upload --upload-port $Port
}
finally {
    Pop-Location
}
