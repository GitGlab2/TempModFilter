<#
.SYNOPSIS
    Builds TempModFilter.sys (KMDF) and FilterClient.exe from the WDK project.

.DESCRIPTION
    Requires Visual Studio 2022 with the "Desktop development with C++"
    workload AND the Windows Driver Kit (WDK) VS extension.  The script
    locates MSBuild with vswhere and fails with instructions when the WDK is
    missing instead of throwing a wall of MSBuild errors.

.EXAMPLE
    .\Build.ps1
    .\Build.ps1 -Configuration Release
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$scriptRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot
$solution    = Join-Path $projectRoot 'TempModFilter.sln'

Write-Host ''
Write-Host '=== TempModFilter build ===' -ForegroundColor Cyan
Write-Host "    project     : $projectRoot"
Write-Host "    configuration: $Configuration|$Platform"

# ---------------------------------------------------------------------------
# 1. Locate MSBuild
# ---------------------------------------------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path $vswhere)) {
    Write-Host ''
    Write-Host '[-] Visual Studio was not found (vswhere.exe is missing).' -ForegroundColor Red
    Write-Host '    Install Visual Studio 2022 + "Desktop development with C++".'
    exit 1
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
                      -find 'MSBuild\**\Bin\MSBuild.exe' |
           Select-Object -First 1

if (-not $msbuild) {
    Write-Host '[-] MSBuild was not found inside the Visual Studio installation.' -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# 2. Locate the WDK (kernel headers + KMDF)
# ---------------------------------------------------------------------------

$kitRoots = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'),
    (Join-Path $env:ProgramFiles 'Windows Kits\10')
)

$wdkVersion = $null
$wdkInclude = $null
foreach ($root in $kitRoots) {
    $includeRoot = Join-Path $root 'Include'
    if (-not (Test-Path $includeRoot)) { continue }

    $candidate = Get-ChildItem -Path $includeRoot -Directory -Filter '10.0.*' `
                              -ErrorAction SilentlyContinue |
                 Where-Object { Test-Path (Join-Path $_.FullName 'km\ntddk.h') } |
                 Sort-Object { [version]$_.Name } -Descending |
                 Select-Object -First 1

    if ($candidate) {
        $wdkVersion = $candidate.Name
        $wdkInclude = $candidate.FullName
        break
    }
}

if (-not $wdkVersion) {
    Write-Host ''
    Write-Host '[-] Windows Driver Kit (WDK) not found.' -ForegroundColor Red
    Write-Host '    Install the WDK for your Windows version, then re-run this script:'
    Write-Host '      https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk'
    Write-Host '    The WDK VS extension is what provides the'
    Write-Host '    "WindowsKernelModeDriver10.0" platform toolset.'
    exit 1
}

Write-Host "    msbuild     : $msbuild"
Write-Host "    wdk version : $wdkVersion"
Write-Host "    wdk headers : $wdkInclude"

# ---------------------------------------------------------------------------
# 3. Build
# ---------------------------------------------------------------------------

if ($Clean) {
    $buildDir = Join-Path $projectRoot 'build'
    if (Test-Path $buildDir) {
        Write-Host '    cleaning    : build\'
        Remove-Item -Path $buildDir -Recurse -Force
    }
}

$arguments = @(
    $solution
    "/m"
    "/v:minimal"
    "/p:Configuration=$Configuration"
    "/p:Platform=$Platform"
    "/p:WindowsTargetPlatformVersion=$wdkVersion"
)

Write-Host ''
& $msbuild $arguments
$exitCode = $LASTEXITCODE

# ---------------------------------------------------------------------------
# 4. Report
# ---------------------------------------------------------------------------

$driverOut = Join-Path $projectRoot "build\$Configuration\$Platform\driver"
$clientOut = Join-Path $projectRoot "build\$Configuration\$Platform\user"

Write-Host ''
if ($exitCode -ne 0) {
    Write-Host "[-] build failed (msbuild exit code $exitCode)" -ForegroundColor Red
    exit $exitCode
}

Get-ChildItem -Path $driverOut -Filter '*.sys' -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "[+] driver : $($_.FullName)" -ForegroundColor Green }

Get-ChildItem -Path $clientOut -Filter '*.exe' -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "[+] client : $($_.FullName)" -ForegroundColor Green }

Write-Host ''
Write-Host 'Next steps:'
Write-Host '  1. .\scripts\Sign-Test.ps1        (test-signing certificate, lab VM only)'
Write-Host '  2. .\scripts\Install.ps1          (then reboot)'
Write-Host '  3. .\scripts\Test-RoundTrip.ps1'
Write-Host ''
