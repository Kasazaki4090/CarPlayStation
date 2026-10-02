<#
.SYNOPSIS
    One-click manual FFmpeg update for CarPlayStation (x64 / ARM64).

.DESCRIPTION
    Downloads the latest stable release-shared FFmpeg build for the target
    architecture and extracts it into the local version cache
    (Lib/ffmpeg/<arch>/ffmpeg-<version>/), then copies the runtime DLLs into
    the build output directory.

    Sources per architecture:
      x64   -> GyanD/codexffmpeg  (zip, includes headers + import libs)
      arm64 -> tordona/ffmpeg-win-arm64 (7z, runtime-only: bin/ DLLs)

    This is the manual counterpart of cmake/FindOrDownloadFFmpeg.cmake, which
    performs the same download automatically at CMake configure time.

.PARAMETER Version
    FFmpeg version to download (default: 9.0.2).

.PARAMETER Arch
    Target architecture: x64 or arm64 (default: auto-detect from host).

.PARAMETER OutputDir
    Optional build output directory to copy runtime DLLs into.

.EXAMPLE
    .\scripts\Update-FFmpeg.ps1
    .\scripts\Update-FFmpeg.ps1 -Version 9.0.2 -Arch x64 -OutputDir ..\build
#>
[CmdletBinding()]
param(
    [string]$Version = "9.0.2",
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

# Source + archive format differ per architecture:
#   x64   -> GyanD/codexffmpeg  (zip, includes headers + import libs)
#   arm64 -> tordona/ffmpeg-win-arm64 (7z, runtime-only: bin/ DLLs, no headers)
if ($Arch -eq "arm64") {
    $archivePath = Join-Path $repoRoot "Lib\ffmpeg\$Arch\ffmpeg-$Version-full-shared-win-arm64.7z"
    $url = "https://github.com/tordona/ffmpeg-win-arm64/releases/download/$Version/ffmpeg-$Version-full-shared-win-arm64.7z"
}
else {
    $archivePath = Join-Path $repoRoot "Lib\ffmpeg\$Arch\ffmpeg-$Version-full_build-shared.zip"
    $url = "https://github.com/GyanD/codexffmpeg/releases/download/$Version/ffmpeg-$Version-full_build-shared.zip"
}

Write-Host "==> CarPlayStation FFmpeg update"
Write-Host "    Version : $Version"
Write-Host "    Arch    : $Arch"
Write-Host "    Cache   : $cacheDir"

# --- Skip if already present (arch-aware marker) ----------------------------
# x64 ships headers; arm64 is runtime-only, so check the bin/ DLLs instead.
$cachePresent = $false
if ($Arch -eq "arm64") {
    $cachePresent = (Get-ChildItem -Path $binDir -Filter "*.dll" -ErrorAction SilentlyContinue) -ne $null
}
else {
    $cachePresent = Test-Path (Join-Path $includeDir "libavcodec\avcodec.h")
}
if ($cachePresent) {
    Write-Host "    FFmpeg already cached at $cacheDir - nothing to do." -ForegroundColor Green
    return
}

# --- Download ---------------------------------------------------------------
Write-Host "==> Downloading $url"
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
Invoke-WebRequest -Uri $url -OutFile $archivePath -UseBasicParsing

# --- Extract ----------------------------------------------------------------
Write-Host "==> Extracting to $cacheDir"
if ($Arch -eq "arm64") {
    # 7z archives need 7-Zip. Extract with the CWD set to the cache dir
    # (the -o switch is unreliable when driven from a non-interactive shell).
    $sevenZip = "C:\Program Files\7-Zip\7z.exe"
    if (-not (Test-Path $sevenZip)) {
        $sevenZip = "C:\Program Files (x86)\7-Zip\7z.exe"
    }
    if (-not (Test-Path $sevenZip)) {
        throw "7-Zip (7z.exe) not found - cannot extract the arm64 7z archive."
    }
    Push-Location $cacheDir
    try {
        & $sevenZip x $archivePath -y | Out-Null
    }
    finally {
        Pop-Location
    }
    # Flatten a nested top-level folder if the archive nests one.
    $nested = Get-ChildItem -Path $cacheDir -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName "bin") }
    if ($nested) {
        foreach ($dir in $nested) {
            Get-ChildItem -Path $dir.FullName | Move-Item -Destination $cacheDir -Force
            Remove-Item -Path $dir.FullName -Recurse -Force
        }
    }
}
else {
    Expand-Archive -Path $archivePath -DestinationPath $cacheDir -Force
    # Gyan.dev zips nest a top-level folder; flatten if needed.
    $nested = Get-ChildItem -Path $cacheDir -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName "include\libavcodec\avcodec.h") }
    if ($nested) {
        foreach ($dir in $nested) {
            Get-ChildItem -Path $dir.FullName | Move-Item -Destination $cacheDir -Force
            Remove-Item -Path $dir.FullName -Recurse -Force
        }
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
Remove-Item -Path $archivePath -Force -ErrorAction SilentlyContinue

Write-Host "==> FFmpeg update complete." -ForegroundColor Green
