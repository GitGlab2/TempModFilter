<#
.SYNOPSIS
    End-to-end check: does the filter really modify the buffer in RAM, and
    does the modification disappear as soon as it is disarmed?

.DESCRIPTION
    Runs FilterClient.exe "demo", which:
        1. reads the serial number of \\.\PhysicalDriveN,
        2. arms a redaction policy for IOCTL_STORAGE_QUERY_PROPERTY,
        3. reads the serial again  -> it is masked,
        4. clears the policy,
        5. reads the serial again  -> the original value is back.

    Nothing is written to the disk at any point: steps 3 and 5 prove that the
    change only ever existed in the returned buffer.

.EXAMPLE
    .\Test-RoundTrip.ps1
    .\Test-RoundTrip.ps1 -Drive 1 -Configuration Release
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [int]$Drive = 0,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [string]$ServiceName = 'TempModFilter',
    [string]$ClassGuid   = '{4D36E967-E325-11CE-BFC1-08002BE10318}'
)

$ErrorActionPreference = 'Stop'

$scriptRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot

function Write-Step { param([string]$Text) Write-Host "    $Text" }
function Write-Ok   { param([string]$Text) Write-Host "[+] $Text" -ForegroundColor Green }
function Write-Fail { param([string]$Text) Write-Host "[-] $Text" -ForegroundColor Red }

Write-Host ''
Write-Host '=== TempModFilter round-trip test ===' -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# 1. Preconditions
# ---------------------------------------------------------------------------

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Fail "the $ServiceName service does not exist. Run scripts\Install.ps1, then reboot."
    exit 1
}

Write-Step "service state : $($service.Status)"

$classKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$ClassGuid"
$filters  = (Get-ItemProperty -Path $classKey -Name 'UpperFilters' `
                              -ErrorAction SilentlyContinue).UpperFilters

if (@($filters) -notcontains $ServiceName) {
    Write-Fail "$ServiceName is not registered as a DiskDrive upper filter."
    exit 1
}

Write-Step "UpperFilters  : $(@($filters) -join ', ')"

# The filter only attaches when the disk devices start, i.e. after a reboot.
$driverFile = Join-Path $env:SystemRoot "System32\drivers\$ServiceName.sys"
if (-not (Test-Path $driverFile)) {
    Write-Fail "$driverFile is missing. Re-run scripts\Install.ps1."
    exit 1
}

Write-Step "driver file   : $driverFile"

# ---------------------------------------------------------------------------
# 2. Client
# ---------------------------------------------------------------------------

$client = Join-Path $projectRoot "build\$Configuration\x64\user\FilterClient.exe"

if (-not (Test-Path $client)) {
    $found = Get-ChildItem -Path (Join-Path $projectRoot 'build') -Recurse `
                           -Filter 'FilterClient.exe' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending |
             Select-Object -First 1
    if ($found) { $client = $found.FullName }
}

if (-not (Test-Path $client)) {
    Write-Fail 'FilterClient.exe not found. Run scripts\Build.ps1 first.'
    exit 1
}

Write-Step "client        : $client"
Write-Step "target disk   : \\.\PhysicalDrive$Drive"

# ---------------------------------------------------------------------------
# 3. Run
# ---------------------------------------------------------------------------

Write-Host ''
& $client status

Write-Host ''
& $client demo --drive $Drive
$exitCode = $LASTEXITCODE

Write-Host ''
& $client stats

Write-Host ''
if ($exitCode -ne 0) {
    Write-Fail 'round-trip test FAILED'
    exit $exitCode
}

Write-Ok 'round-trip test PASSED'
Write-Host ''
Write-Host '  The serial was masked while the policy was armed and came back'
Write-Host '  unchanged after clearing it. Nothing was persisted anywhere.'
Write-Host ''
