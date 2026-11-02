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
                      -find 'MSBuild\**\Bin\amd64\MSBuild.exe' |
           Select-Object -First 1

if (-not $msbuild) {
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
                          -find 'MSBuild\**\Bin\MSBuild.exe' |
               Select-Object -First 1
}

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
$partialVersion = $null
foreach ($root in $kitRoots) {
    $includeRoot = Join-Path $root 'Include'
    if (-not (Test-Path $includeRoot)) { continue }

    $candidates = Get-ChildItem -Path $includeRoot -Directory -Filter '10.0.*' `
                                -ErrorAction SilentlyContinue |
                   Where-Object { Test-Path (Join-Path $_.FullName 'km\ntddk.h') } |
                   Sort-Object { [version]$_.Name } -Descending

    foreach ($candidate in $candidates) {
        # A usable version needs both halves: the kernel headers from the WDK
        # *and* the user-mode headers from the matching Windows SDK.
        if (Test-Path (Join-Path $candidate.FullName 'um\windows.h')) {
            $wdkVersion = $candidate.Name
            $wdkInclude = $candidate.FullName
            break
        }
        if (-not $partialVersion) { $partialVersion = $candidate.Name }
    }

    if ($wdkVersion) { break }
}

if (-not $wdkVersion) {
    Write-Host ''
    if ($partialVersion) {
        Write-Host "[-] WDK $partialVersion is installed, but the matching Windows SDK is not." -ForegroundColor Red
        Write-Host "    The kernel headers in Include\$partialVersion\km exist, yet"
        Write-Host "    Include\$partialVersion\um\windows.h is missing, so MSBuild cannot"
        Write-Host "    resolve the SDK version and fails with MSB8036."
        Write-Host ''
        Write-Host '    Install the Windows SDK with the same version in the Visual Studio'
        Write-Host '    Installer (Individual components -> Windows SDK), or install a WDK'
        Write-Host '    whose version matches an SDK that is already present:'
        Write-Host '      https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk'
    } else {
        Write-Host '[-] Windows Driver Kit (WDK) not found.' -ForegroundColor Red
        Write-Host '    Install the WDK for your Windows version, then re-run this script:'
        Write-Host '      https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk'
        Write-Host '    The WDK VS extension is what provides the'
        Write-Host '    "WindowsKernelModeDriver10.0" platform toolset.'
    }
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
