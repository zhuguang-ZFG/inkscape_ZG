param(
    [string]$Port = "COM3",
    [int]$Baud = 115200,
    [int]$TimeoutMs = 1500,
    [string]$LogPath,
    [switch]$Summary,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if (-not $LogPath) {
    $LogPath = Join-Path (Get-Location) ("serial-smoke-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
}

$scriptTemplate = @'
import sys
import time
from pathlib import Path

import serial

port = r'''__PORT__'''
baud = __BAUD__
timeout_s = __TIMEOUT_S__
log_path = Path(r'''__LOG_PATH__''')

def read_for(deadline, ser, chunks):
    while time.time() < deadline:
        waiting = ser.in_waiting
        if waiting:
            data = ser.read(waiting)
            if data:
                chunks.append(data)
        else:
            time.sleep(0.05)

with serial.Serial(port, baudrate=baud, timeout=timeout_s, write_timeout=timeout_s) as ser:
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b"\r\n")
    ser.flush()
    chunks = []
    read_for(time.time() + timeout_s, ser, chunks)
    ser.write(b"$$\n")
    ser.flush()
    read_for(time.time() + timeout_s, ser, chunks)

raw = b"".join(chunks)
text = raw.decode("utf-8", errors="replace")
log_path.write_text(text, encoding="utf-8")
sys.stdout.write(text)
'@

$script = $scriptTemplate.
    Replace("__PORT__", $Port).
    Replace("__BAUD__", [string]$Baud).
    Replace("__TIMEOUT_S__", [string]([double]$TimeoutMs / 1000.0)).
    Replace("__LOG_PATH__", $LogPath)

$commandText = "@'`n$script`n'@ | python -"

if ($DryRun) {
    Write-Output "LogPath=$LogPath"
    Write-Output $commandText
    exit 0
}

${text} = @"
$script
"@ | python -

if (-not $Summary) {
    return
}

$lines = ($text -split "`r?`n") | Where-Object { $_ -ne "" }
$versionLine = $lines | Where-Object { $_ -match "Grbl_ESP32 Ver|^Grbl " } | Select-Object -First 1
$machineLine = $lines | Where-Object { $_ -match "Using machine:" } | Select-Object -First 1
$penModeLine = $lines | Where-Object { $_ -match "Pen control via" } | Select-Object -First 1

$settings = @{}
foreach ($line in $lines) {
    if ($line -match '^\$(\d+)=([^\r\n]+)$') {
        $settings[$matches[1]] = $matches[2]
    }
}

$summaryLines = @()
$summaryLines += "=== SERIAL SMOKE SUMMARY ==="
$summaryLines += "Port: $Port"
$summaryLines += "LogPath: $LogPath"
if ($versionLine) { $summaryLines += "Version: $versionLine" }
if ($machineLine) { $summaryLines += "Machine: $machineLine" }
if ($penModeLine) { $summaryLines += "PenMode: $penModeLine" }

foreach ($key in @("1", "80", "81", "112", "122")) {
    if ($settings.ContainsKey($key)) {
        $summaryLines += ('$' + $key + '=' + $settings[$key])
    }
}

if ($settings.Count -eq 0) {
    $summaryLines += "No settings parsed from response."
}

$summaryLines += "=== END SUMMARY ==="
Write-Output ($summaryLines -join [Environment]::NewLine)
