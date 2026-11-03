# Adds inbound allow rules for CarPlayStation on every firewall profile.
#
# The hotspot access interface (192.168.137.1) has no connection profile, so
# Windows treats it as Public, while the auto-created rules only cover Private.
# That blocks inbound mDNS (UDP 5353) and the iPhone's RTSP connection (TCP 5000).

param(
    [string]$ExePath = ""
)

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "Requesting administrative privileges..." -ForegroundColor Yellow
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($ExePath) {
        $argList += " -ExePath `"$ExePath`""
    }
    Start-Process powershell -ArgumentList $argList -Verb RunAs
    exit 0
}

# Auto-detect CarPlayStation.exe if not specified
if (-not $ExePath -or -not (Test-Path $ExePath)) {
    $candidates = @(
        (Join-Path $PSScriptRoot "CarPlayStation.exe"),
        (Join-Path $PSScriptRoot "..\CarPlayStation.exe"),
        (Join-Path $PSScriptRoot "..\build\UI\Win32\Release\CarPlayStation.exe"),
        (Join-Path $PSScriptRoot "..\build-release\UI\Win32\CarPlayStation.exe"),
        (Join-Path $PSScriptRoot "..\build\UI\Win32\CarPlayStation.exe"),
        (Join-Path $PSScriptRoot "..\dist\CarPlayStation.exe")
    )
    foreach ($cand in $candidates) {
        if (Test-Path $cand) {
            $ExePath = (Resolve-Path $cand).Path
            break
        }
    }
}

if ($ExePath -and (Test-Path $ExePath)) {
    $ExePath = (Resolve-Path $ExePath).Path
    Write-Host "Configuring firewall rules for: $ExePath" -ForegroundColor Cyan

    # Program-scoped rule: covers mDNS 5353, RTSP 5000, video 7100 and the
    # connect-out to the iPhone's control port in one place.
    $programRule = "CarPlayStation (all profiles)"
    if (Get-NetFirewallRule -DisplayName $programRule -ErrorAction SilentlyContinue) {
        Set-NetFirewallRule -DisplayName $programRule -Profile Any -Enabled True -Program $ExePath
        Write-Host "Updated: $programRule" -ForegroundColor Green
    } else {
        New-NetFirewallRule -DisplayName $programRule `
            -Direction Inbound -Action Allow -Profile Any -Program $ExePath | Out-Null
        Write-Host "Created: $programRule" -ForegroundColor Green
    }
} else {
    Write-Host "CarPlayStation.exe not found at specified or default paths. Skipping program rule." -ForegroundColor Yellow
}

# Explicit UDP 5353 rule so mDNS still works if the executable is rebuilt to a different path.
$mdnsRule = "CarPlayStation mDNS (UDP 5353)"
if (-not (Get-NetFirewallRule -DisplayName $mdnsRule -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $mdnsRule `
        -Direction Inbound -Action Allow -Profile Any `
        -Protocol UDP -LocalPort 5353 | Out-Null
    Write-Host "Created: $mdnsRule" -ForegroundColor Green
} else {
    Set-NetFirewallRule -DisplayName $mdnsRule -Profile Any -Enabled True
    Write-Host "Updated: $mdnsRule" -ForegroundColor Green
}

# Explicit TCP 5000 rule for RTSP
$rtspRule = "CarPlayStation RTSP (TCP 5000)"
if (-not (Get-NetFirewallRule -DisplayName $rtspRule -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $rtspRule `
        -Direction Inbound -Action Allow -Profile Any `
        -Protocol TCP -LocalPort 5000 | Out-Null
    Write-Host "Created: $rtspRule" -ForegroundColor Green
} else {
    Set-NetFirewallRule -DisplayName $rtspRule -Profile Any -Enabled True
    Write-Host "Updated: $rtspRule" -ForegroundColor Green
}

# Explicit UDP 7000-7100 rule for RTP Media
$rtpRule = "CarPlayStation RTP Media (UDP 7000-7100)"
if (-not (Get-NetFirewallRule -DisplayName $rtpRule -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $rtpRule `
        -Direction Inbound -Action Allow -Profile Any `
        -Protocol UDP -LocalPort 7000-7100 | Out-Null
    Write-Host "Created: $rtpRule" -ForegroundColor Green
} else {
    Set-NetFirewallRule -DisplayName $rtpRule -Profile Any -Enabled True
    Write-Host "Updated: $rtpRule" -ForegroundColor Green
}

Write-Host ""
Write-Host "Firewall rules configured successfully." -ForegroundColor Cyan
Write-Host "Verify with:" -ForegroundColor Yellow
Write-Host "  Get-NetFirewallRule -DisplayName 'CarPlayStation*' | Format-Table DisplayName,Enabled,Profile,Action"
