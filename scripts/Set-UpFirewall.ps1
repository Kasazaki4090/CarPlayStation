# Adds inbound allow rules for CarPlayStation on every firewall profile.
#
# The hotspot access interface (192.168.137.1) has no connection profile, so
# Windows treats it as Public, while the auto-created rules only cover Private.
# That blocks inbound mDNS (UDP 5353) and the iPhone's RTSP connection (TCP 5000).
#
# Run from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File scripts\Set-UpFirewall.ps1

param(
    [string]$ExePath = (Join-Path $PSScriptRoot "..\build-release\UI\Win32\CarPlayStation.exe")
)

$ErrorActionPreference = "Stop"

$ExePath = (Resolve-Path $ExePath).Path
Write-Host "Firewall rules for: $ExePath" -ForegroundColor Cyan

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "This script must run in an elevated PowerShell (Run as administrator)."
}

# Program-scoped rule: covers mDNS 5353, RTSP 5000, video 7100 and the
# connect-out to the iPhone's control port in one place.
$programRule = "CarPlayStation (all profiles)"
if (Get-NetFirewallRule -DisplayName $programRule -ErrorAction SilentlyContinue) {
    Set-NetFirewallRule -DisplayName $programRule -Profile Any -Enabled True
    Write-Host "Updated: $programRule" -ForegroundColor Green
} else {
    New-NetFirewallRule -DisplayName $programRule `
        -Direction Inbound -Action Allow -Profile Any -Program $ExePath | Out-Null
    Write-Host "Created: $programRule" -ForegroundColor Green
}

# Explicit UDP 5353 rule so mDNS still works if the executable is rebuilt to a
# different path.
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

Write-Host ""
Write-Host "Verify with:" -ForegroundColor Yellow
Write-Host "  Get-NetFirewallRule -DisplayName 'CarPlayStation*' | Format-Table DisplayName,Enabled,Profile,Action"
