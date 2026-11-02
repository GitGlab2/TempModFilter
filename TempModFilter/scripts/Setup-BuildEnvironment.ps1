<#
.SYNOPSIS
    Automated installer for Visual Studio 2022 Build Tools and Windows Driver Kit (WDK).

.DESCRIPTION
    Installs the minimal required toolchain to compile TempModFilter.sys (KMDF) and FilterClient.exe.
    Requires Administrator privileges and an active internet connection.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\Setup-BuildEnvironment.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

Write-Host "=== TempModFilter: Build Environment Setup ===" -ForegroundColor Cyan

if (-not (Test-Admin)) {
    Write-Host "[!] Administrator privileges required." -ForegroundColor Yellow
    Write-Host "    Relaunching in an elevated PowerShell window..." -ForegroundColor Yellow
    Start-Process powershell -Verb RunAs -ArgumentList "-NoExit -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 0
}

$tempDir = Join-Path $env:TEMP "TempModBuildSetup"
if (-not (Test-Path $tempDir)) {
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
}

# 1. Download & Install Visual Studio Build Tools
Write-Host "`n[1/3] Checking Visual Studio 2022 C++ Tools..." -ForegroundColor Cyan
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$needsVs = $true

if (Test-Path $vswhere) {
    $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop
    if ($found) {
        Write-Host "    [+] Visual Studio with C++ Desktop workload is already installed." -ForegroundColor Green
        $needsVs = $false
    }
}

if ($needsVs) {
    Write-Host "    [*] Downloading Visual Studio Community Installer..." -ForegroundColor Yellow
    $vsInstallerUrl = "https://aka.ms/vs/17/release/vs_community.exe"
    $vsInstallerPath = Join-Path $tempDir "vs_community.exe"
    Invoke-WebRequest -Uri $vsInstallerUrl -OutFile $vsInstallerPath

    Write-Host "    [*] Installing Visual Studio 2022 with C++ Desktop workload..." -ForegroundColor Yellow
    Write-Host "        (This will take several minutes. Please wait...)" -ForegroundColor Gray

    $process = Start-Process -FilePath $vsInstallerPath -ArgumentList `
        "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended --passive --norestart --wait" `
        -Wait -PassThru

    if ($process.ExitCode -eq 0 -or $process.ExitCode -eq 3010) {
        Write-Host "    [+] Visual Studio installation completed successfully." -ForegroundColor Green
    } else {
        Write-Host "    [-] Visual Studio installer exited with code: $($process.ExitCode)" -ForegroundColor Red
    }
}

# 2. Download & Install Windows SDK
Write-Host "`n[2/3] Checking Windows SDK..." -ForegroundColor Cyan
$sdkInclude = "C:\Program Files (x86)\Windows Kits\10\Include"
if (Test-Path $sdkInclude) {
    Write-Host "    [+] Windows SDK is installed." -ForegroundColor Green
} else {
    Write-Host "    [*] Downloading Windows 11 SDK (22621)..." -ForegroundColor Yellow
    $sdkUrl = "https://go.microsoft.com/fwlink/p/?linkid=2196241"
    $sdkPath = Join-Path $tempDir "winsdksetup.exe"
    Invoke-WebRequest -Uri $sdkUrl -OutFile $sdkPath

    Write-Host "    [*] Installing Windows SDK..." -ForegroundColor Yellow
    Start-Process -FilePath $sdkPath -ArgumentList "/quiet /norestart" -Wait
    Write-Host "    [+] Windows SDK installation finished." -ForegroundColor Green
}

# 3. Download & Install Windows Driver Kit (WDK)
Write-Host "`n[3/3] Checking Windows Driver Kit (WDK)..." -ForegroundColor Cyan
$wdkFound = $false
$kitRoots = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'),
    (Join-Path $env:ProgramFiles 'Windows Kits\10')
)
foreach ($root in $kitRoots) {
    $includeRoot = Join-Path $root 'Include'
    if (Test-Path $includeRoot) {
        $wdf = Get-ChildItem -Path $includeRoot -Recurse -Filter 'wdf.h' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($wdf) { $wdkFound = $true; break }
    }
}

if ($wdkFound) {
    Write-Host "    [+] Windows Driver Kit (WDK) is already installed." -ForegroundColor Green
} else {
    Write-Host "    [*] Downloading Windows Driver Kit (WDK 10.0.22621)..." -ForegroundColor Yellow
    $wdkUrl = "https://go.microsoft.com/fwlink/?linkid=2196230"
    $wdkPath = Join-Path $tempDir "wdksetup.exe"
    Invoke-WebRequest -Uri $wdkUrl -OutFile $wdkPath

    Write-Host "    [*] Installing WDK..." -ForegroundColor Yellow
    Start-Process -FilePath $wdkPath -ArgumentList "/quiet /norestart" -Wait
    Write-Host "    [+] WDK installation finished." -ForegroundColor Green
}

Write-Host "`n=== Setup Complete! ===" -ForegroundColor Green
Write-Host "You can now run: powershell -ExecutionPolicy Bypass -File .\scripts\Build.ps1" -ForegroundColor Cyan
