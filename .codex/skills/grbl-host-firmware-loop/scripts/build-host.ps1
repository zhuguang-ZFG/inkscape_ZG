param(
    [switch]$Reconfigure,
    [switch]$ConfigureOnly,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$skillDir = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $skillDir))
$buildScript = Join-Path $repoRoot "build-zg-inkscape.cmd"

if (-not (Test-Path $buildScript)) {
    throw "Host build script not found: $buildScript"
}

$args = @()
if ($Reconfigure) {
    $args += "--reconfigure"
}
if ($ConfigureOnly) {
    $args += "--configure-only"
}

$commandText = "& `"$buildScript`""
if ($args.Count -gt 0) {
    $commandText += " " + ($args -join " ")
}

if ($DryRun) {
    Write-Output $commandText
    exit 0
}

& $buildScript @args
