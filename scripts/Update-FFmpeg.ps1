<#
.SYNOPSIS
    One-click manual FFmpeg update for CarPlayStation (x64 / ARM64).

.DESCRIPTION
    Downloads the latest stable release-shared FFmpeg ZIP (Gyan.dev / BtbN),
    extracts avcodec / avutil and headers into the local version cache
    (Lib/ffmpeg/<arch>/ffmpeg-<version>/), and copies the runtime DLLs into
    the build output directory.

    This is the manual counterpart of cmake/FindOrDownloadFFmpeg.cmake, which
    performs the same download automatically at CMake configure time.

.PARAMETER Version
    FFmpeg version to download (default: 7.1).

.PARAMETER Arch
    Target architecture: x64 or arm64 (default: auto-detect from host).

.PARAMETER OutputDir
    Optional build output directory to copy runtime DLLs into.

.EXAMPLE
    .\scripts\Update-FFmpeg.ps1
    .\scripts\Update-FFmpeg.ps1 -Version 7.1 -Arch x64 -OutputDir ..\build
#>
[CmdletBinding()]
param(
    [string]$Version = "7.1",
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

# --- Auto-detect architecture ---------------------------------------------
if ([string]::IsNullOrWhiteSpace($Arch)) {
    $machine = $env:PROCESSOR_ARCHITECTURE
    if ($machine -eq "ARM64") {
        $Arch = "arm64"
    }
    else {
        $Arch = "x64"
    }
}

# --- Resolve paths ----------------------------------------------------------
$repoRoot   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$cacheDir   = Join-Path $repoRoot "Lib\ffmpeg\$Arch\ffmpeg-$Version"
$includeDir = Join-Path $cacheDir "include"
$binDir     = Join-Path $cacheDir "bin"
$zipPath    = Join-Path $repoRoot "Lib\ffmpeg\$Arch\ffmpeg-$Version-$Arch-shared.zip"

$url = "https://github.com/GyanD/codexffmpeg/releases/download/$Version/ffmpeg-$Version-$Arch-shared.zip"

Write-Host "==> CarPlayStation FFmpeg update"
Write-Host "    Version : $Version"
Write-Host "    Arch    : $Arch"
Write-Host "    Cache   : $cacheDir"

# --- Skip if already present ------------------------------------------------
if (Test-Path (Join-Path $includeDir "libavcodec\avcodec.h")) {
    Write-Host "    FFmpeg already cached at $cacheDir - nothing to do." -ForegroundColor Green
    return
}

# --- Download ---------------------------------------------------------------
Write-Host "==> Downloading $url"
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing

# --- Extract ----------------------------------------------------------------
Write-Host "==> Extracting to $cacheDir"
Expand-Archive -Path $zipPath -DestinationPath $cacheDir -Force

# Gyan.dev zips nest a top-level folder; flatten if needed.
$nested = Get-ChildItem -Path $cacheDir -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "include\libavcodec\avcodec.h") }
if ($nested) {
    foreach ($dir in $nested) {
        Get-ChildItem -Path $dir.FullName | Move-Item -Destination $cacheDir -Force
        Remove-Item -Path $dir.FullName -Recurse -Force
    }
}

# --- Copy runtime DLLs to output dir (optional) -----------------------------
if (-not [string]::IsNullOrWhiteSpace($OutputDir)) {
    if (Test-Path $binDir) {
        Write-Host "==> Copying runtime DLLs to $OutputDir"
        Copy-Item -Path (Join-Path $binDir "*.dll") -Destination $OutputDir -Force
    }
}

# --- Cleanup ----------------------------------------------------------------
Remove-Item -Path $zipPath -Force -ErrorAction SilentlyContinue

Write-Host "==> FFmpeg update complete." -ForegroundColor Green
