# Package-Release.ps1
param (
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

if (-not $Version) {
    # 自动根据 git commit 数量计算：0.1.<commit_count>
    try {
        $commitCount = (git rev-list --count HEAD).Trim()
        $Version = "v0.1.$commitCount"
    } catch {
        $Version = "v0.1.50"
    }
}

$distRoot = "dist"
$pkgName = "CarPlayStation-$Version-win64"
$pkgDir = Join-Path $distRoot $pkgName
$zipFile = Join-Path $distRoot "$pkgName.zip"

Write-Host "Target Version: $Version"
Write-Host "Output Directory: $pkgDir"
Write-Host "Output Zip: $zipFile"

# 清理旧的目录与 zip
if (Test-Path $pkgDir) {
    Remove-Item -Recurse -Force $pkgDir
}
New-Item -ItemType Directory -Path $pkgDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $pkgDir "scripts") -Force | Out-Null

$binSource = "build\UI\Win32\Release"
$binFiles = @(
    "CarPlayStation.exe",
    "libmpv-2.dll",
    "CH341DLL.DLL",
    "CH341DLLA64.DLL",
    "CH347DLL.DLL",
    "CH347DLLA64.DLL",
    "ftd2xx.dll",
    "libmpsse.dll"
)

foreach ($f in $binFiles) {
    $src = Join-Path $binSource $f
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination $pkgDir -Force
        Write-Host "Copied $f"
    } else {
        Write-Warning "File not found: $src"
    }
}

Copy-Item -Path "scripts\Set-UpFirewall.ps1" -Destination (Join-Path $pkgDir "scripts") -Force
Copy-Item -Path "README.md" -Destination $pkgDir -Force
Copy-Item -Path "README_zh.md" -Destination $pkgDir -Force
Copy-Item -Path "LICENSE" -Destination $pkgDir -Force

if (Test-Path $zipFile) {
    Remove-Item -Force $zipFile
}

Write-Host "Compressing archive to $zipFile..."
Compress-Archive -Path "$pkgDir\*" -DestinationPath $zipFile -CompressionLevel Optimal
Write-Host "Done! Package size: $([math]::Round((Get-Item $zipFile).Length / 1MB, 2)) MB"
