#Requires -Version 5.1
<#
.SYNOPSIS
    Downloads and extracts the prebuilt libmpv (x64) library for CarPlayStation.
.DESCRIPTION
    Fetches mpv-dev-x86_64 from the official shinchiro/mpv-winbuild-cmake project,
    extracts libmpv-2.dll and headers, and optionally syncs it into existing build output folders.
#>

[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$dllPath = Join-Path $scriptDir "libmpv-2.dll"

if ((Test-Path $dllPath) -and -not $Force) {
    $sizeMb = [math]::Round((Get-Item $dllPath).Length / 1MB, 2)
    Write-Host "[OK] libmpv-2.dll already exists ($sizeMb MB) at: $dllPath" -ForegroundColor Green
    Write-Host "Use -Force parameter to re-download if needed." -ForegroundColor Yellow
    exit 0
}

# 1. Locate 7-Zip executable
$7zCandidates = @(
    "7z",
    "$env:ProgramFiles\7-Zip\7z.exe",
    "${env:ProgramFiles(x86)}\7-Zip\7z.exe",
    "$env:LOCALAPPDATA\Programs\7-Zip\7z.exe"
)

$7zPath = $null
foreach ($c in $7zCandidates) {
    if (Get-Command $c -ErrorAction SilentlyContinue) {
        $7zPath = (Get-Command $c).Source
        break
    }
    if (Test-Path $c) {
        $7zPath = $c
        break
    }
}

if (-not $7zPath) {
    Write-Error "7-Zip (7z.exe) not found. Please install 7-Zip from https://www.7-zip.org/ to extract .7z archives."
    exit 1
}

Write-Host "[INFO] Using 7-Zip at: $7zPath" -ForegroundColor Cyan

# 2. Resolve download URL from GitHub API with fallback
$fallbackUrl = "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261008/mpv-dev-x86_64-20261008-git-36bf3d5290.7z"
$downloadUrl = $null

Write-Host "[INFO] Querying latest mpv-dev release from GitHub..." -ForegroundColor Cyan
try {
    $apiResponse = Invoke-RestMethod -Uri "https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest" -Headers @{ "User-Agent" = "CarPlayStation-Downloader" } -TimeoutSec 10
    $asset = $apiResponse.assets | Where-Object { $_.name -like "mpv-dev-x86_64-*.7z" -and $_.name -notlike "*v3*" } | Select-Object -First 1
    if ($asset -and $asset.browser_download_url) {
        $downloadUrl = $asset.browser_download_url
        Write-Host "[INFO] Found asset: $($asset.name)" -ForegroundColor Green
    }
} catch {
    Write-Warning "Could not reach GitHub API (rate limited or offline). Using fallback URL."
}

if (-not $downloadUrl) {
    $downloadUrl = $fallbackUrl
}

# 3. Download archive
$archivePath = Join-Path $scriptDir "mpv-dev.7z"
Write-Host "[INFO] Downloading from: $downloadUrl" -ForegroundColor Cyan

if (Get-Command "curl.exe" -ErrorAction SilentlyContinue) {
    & curl.exe -L "$downloadUrl" -o "$archivePath"
    if ($LASTEXITCODE -ne 0) {
        throw "curl download failed with exit code $LASTEXITCODE"
    }
} else {
    Invoke-WebRequest -Uri $downloadUrl -OutFile $archivePath
}

# 4. Extract archive
Write-Host "[INFO] Extracting $archivePath ..." -ForegroundColor Cyan
& $7zPath x "$archivePath" "-o$scriptDir" -y
if ($LASTEXITCODE -ne 0) {
    throw "7-Zip extraction failed with exit code $LASTEXITCODE"
}

# 5. Clean up temporary archive
if (Test-Path $archivePath) {
    Remove-Item -Force $archivePath
}

# 6. Verify result
if (Test-Path $dllPath) {
    $sizeMb = [math]::Round((Get-Item $dllPath).Length / 1MB, 2)
    Write-Host "[SUCCESS] libmpv-2.dll ready! Size: $sizeMb MB ($dllPath)" -ForegroundColor Green

    # Optional: if build directories exist, copy DLL there as well
    $repoRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
    $outputDirs = @(
        (Join-Path $repoRoot "build\UI\Win32\Release"),
        (Join-Path $repoRoot "build\UI\Win32\Debug"),
        (Join-Path $repoRoot "build\Tests\Release"),
        (Join-Path $repoRoot "build\Tests\Debug")
    )
    foreach ($outDir in $outputDirs) {
        if (Test-Path $outDir) {
            Copy-Item -Path $dllPath -Destination $outDir -Force
            Write-Host "[INFO] Copied libmpv-2.dll to: $outDir" -ForegroundColor Cyan
        }
    }
} else {
    Write-Error "Extraction finished but libmpv-2.dll was not found."
    exit 1
}

