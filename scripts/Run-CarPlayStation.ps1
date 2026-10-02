param(
    [switch]$Headless,
    [switch]$Debug
)

$ErrorActionPreference = "Stop"

$exeRelease = Join-Path $PSScriptRoot "..\build-release\UI\Win32\CarPlayStation.exe"
$exeDebug   = Join-Path $PSScriptRoot "..\build-msvc\UI\Win32\CarPlayStation.exe"

$targetExe = $null
if ($Debug -and (Test-Path $exeDebug)) {
    $targetExe = $exeDebug
} elseif (Test-Path $exeRelease) {
    $targetExe = $exeRelease
} elseif (Test-Path $exeDebug) {
    $targetExe = $exeDebug
} else {
    Write-Error "CarPlayStation.exe not found! Please build the project first."
}

$resolvedPath = (Resolve-Path $targetExe).Path
Write-Host "Launching CarPlayStation: $resolvedPath" -ForegroundColor Green

if ($Headless) {
    & $resolvedPath "--headless"
} else {
    Start-Process -FilePath $resolvedPath
    Write-Host "CarPlayStation launched on your desktop." -ForegroundColor Cyan
}
