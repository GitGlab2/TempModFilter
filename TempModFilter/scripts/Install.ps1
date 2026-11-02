<#
.SYNOPSIS
    Installs TempModFilter as an upper filter of the DiskDrive setup class.

.DESCRIPTION
    LAB / TEST VM ONLY.

    The script:
      1. refuses to run outside a virtual machine unless -Force is given,
      2. backs up the class UpperFilters value (so Uninstall can restore it
         exactly, instead of deleting somebody else's filter),
      3. copies the driver to %SystemRoot%\System32\drivers,
      4. creates the kernel service,
      5. appends "TempModFilter" to the DiskDrive class UpperFilters,
      6. asks for a reboot (the filter only attaches at device start).

    Two modes:
      Manual (default) - sc.exe + registry, no INF processing. This is the
                         path used for day-to-day lab work.
      Inf              - legacy driver-package install through
                         setupapi!InstallHinfSection (needs a signed .cat
                         for a clean install on a locked-down machine).

.EXAMPLE
    .\Install.ps1
    .\Install.ps1 -DriverPath ..\build\Release\x64\driver\TempModFilter.sys
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [ValidateSet('Manual', 'Inf')]
    [string]$Mode = 'Manual',

    [string]$DriverPath,
    [string]$ServiceName = 'TempModFilter',
    [string]$ClassGuid   = '{4D36E967-E325-11CE-BFC1-08002BE10318}',

    [switch]$Force,
    [switch]$NoPrompt
)

$ErrorActionPreference = 'Stop'

$scriptRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot

$classKey      = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$ClassGuid"
$backupDir     = Join-Path $env:ProgramData 'TempModFilter'
$backupFile    = Join-Path $backupDir 'UpperFilters.backup.json'
$driverDestDir = Join-Path $env:SystemRoot 'System32\drivers'
$driverDest    = Join-Path $driverDestDir "$ServiceName.sys"

function Write-Step    { param([string]$Text) Write-Host "    $Text" }
function Write-Ok      { param([string]$Text) Write-Host "[+] $Text" -ForegroundColor Green }
function Write-Warn    { param([string]$Text) Write-Host "[!] $Text" -ForegroundColor Yellow }
function Write-Fail    { param([string]$Text) Write-Host "[-] $Text" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# Banner + guard rails
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '=============================================================' -ForegroundColor Yellow
Write-Host ' TempModFilter - kernel-mode upper filter (DiskDrive class)' -ForegroundColor Yellow
Write-Host ' LAB / TEST VM ONLY.  Never install this on a production host.' -ForegroundColor Yellow
Write-Host '=============================================================' -ForegroundColor Yellow
Write-Host ''

try {
    $model = (Get-CimInstance -ClassName Win32_ComputerSystem).Model
} catch {
    $model = '<unknown>'
}

$virtualHints = @('Virtual Machine', 'VMware', 'VirtualBox', 'Hyper-V',
                  'KVM', 'QEMU', 'Xen', 'Parallels')

$isVirtual = $false
foreach ($hint in $virtualHints) {
    if ($model -like "*$hint*") { $isVirtual = $true; break }
}

Write-Step "machine model : $model"

if (-not $isVirtual -and -not $Force) {
    Write-Fail 'this does not look like a virtual machine.'
    Write-Host '    The filter sits in the storage path of every disk. A bug in it'
    Write-Host '    can take the whole machine down with a bugcheck.'
    Write-Host '    Use a Hyper-V/VMware guest, or pass -Force if you really mean it.'
    exit 1
}

# Test-signing check --------------------------------------------------------

try {
    $bcd = & bcdedit.exe /enum '{current}' 2>$null
    if ($bcd -match 'testsigning\s+Yes') {
        Write-Step 'test signing  : ON'
    } else {
        Write-Warn 'test signing appears to be OFF.'
        Write-Host '    An unsigned/untrusted driver will be rejected with error 577.'
        Write-Host '    In the VM:  bcdedit /set testsigning on  (then reboot, Secure Boot off)'
    }
} catch {
    Write-Warn 'could not query bcdedit for the test-signing state.'
}

# ---------------------------------------------------------------------------
# Locate the driver binary
# ---------------------------------------------------------------------------

if (-not $DriverPath) {
    $candidate = Get-ChildItem -Path (Join-Path $projectRoot 'build') -Recurse `
                               -Filter "$ServiceName.sys" -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending |
                 Select-Object -First 1

    if (-not $candidate) {
        Write-Fail "no $ServiceName.sys under build\. Run scripts\Build.ps1 first."
        exit 1
    }
    $DriverPath = $candidate.FullName
}

if (-not (Test-Path $DriverPath)) {
    Write-Fail "driver not found: $DriverPath"
    exit 1
}

Write-Step "driver        : $DriverPath"
Write-Step "service       : $ServiceName"
Write-Step "class key     : $classKey"
Write-Step "mode          : $Mode"

# ---------------------------------------------------------------------------
# Confirmation
# ---------------------------------------------------------------------------

if (-not $NoPrompt -and -not $Force) {
    Write-Host ''
    $answer = Read-Host 'Type INSTALL to continue'
    if ($answer -ne 'INSTALL') {
        Write-Fail 'aborted.'
        exit 1
    }
}

# ---------------------------------------------------------------------------
# Back up the current UpperFilters value (once)
# ---------------------------------------------------------------------------

if (-not (Test-Path $backupDir)) {
    New-Item -Path $backupDir -ItemType Directory -Force | Out-Null
}

if (-not (Test-Path $backupFile)) {
    $existing = Get-ItemProperty -Path $classKey -Name 'UpperFilters' `
                                 -ErrorAction SilentlyContinue

    $values = @()
    if ($existing -and $existing.UpperFilters) {
        $values = @($existing.UpperFilters)
    }

    $backup = [pscustomobject]@{
        ClassKey = $classKey
        Existed  = [bool]($existing -and $existing.UpperFilters)
        Values   = @($values)
        SavedAt  = (Get-Date).ToString('o')
    }

    $backup | ConvertTo-Json -Depth 4 | Set-Content -Path $backupFile -Encoding UTF8
    Write-Step "backup saved  : $backupFile"
} else {
    Write-Step "backup exists : $backupFile"
}

# ---------------------------------------------------------------------------
# Manual install: service + registry
# ---------------------------------------------------------------------------

if ($Mode -eq 'Manual') {

    Write-Host ''
    Write-Host '  [1/3] stopping and recreating the service'

    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($service) {
        & sc.exe stop $ServiceName | Out-Null
        Start-Sleep -Milliseconds 500
        & sc.exe delete $ServiceName | Out-Null
        Start-Sleep -Milliseconds 500
    }

    Copy-Item -Path $DriverPath -Destination $driverDest -Force
    Write-Step "copied to     : $driverDest"

    $createOutput = & sc.exe create $ServiceName `
                        binPath= "$driverDest" `
                        type= kernel `
                        start= demand `
                        error= normal `
                        DisplayName= "TempModFilter (lab only)" 2>&1

    if ($LASTEXITCODE -ne 0) {
        Write-Fail "sc.exe create failed:"
        $createOutput | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
    Write-Ok 'kernel service created (start=demand)'

    Write-Host ''
    Write-Host '  [2/3] registering the class upper filter'

    $current = Get-ItemProperty -Path $classKey -Name 'UpperFilters' `
                                -ErrorAction SilentlyContinue

    $list = @()
    if ($current -and $current.UpperFilters) {
        $list = @($current.UpperFilters)
    }

    if ($list -contains $ServiceName) {
        Write-Step "$ServiceName is already registered"
    } else {
        $list += $ServiceName
        New-ItemProperty -Path $classKey -Name 'UpperFilters' `
                         -PropertyType MultiString -Value $list -Force | Out-Null
        Write-Ok "UpperFilters = $($list -join ', ')"
    }

    Write-Host ''
    Write-Host '  [3/3] scheduling'

} else {

    # -----------------------------------------------------------------------
    # INF install
    # -----------------------------------------------------------------------

    $infPath = Join-Path (Split-Path -Parent $DriverPath) 'FilterDriver.inf'

    if (-not (Test-Path $infPath)) {
        $infPath = Join-Path $projectRoot 'driver\FilterDriver.inf'
    }
    if (-not (Test-Path $infPath)) {
        Write-Fail 'FilterDriver.inf not found.'
        exit 1
    }

    Copy-Item -Path $DriverPath -Destination $driverDest -Force

    Write-Host ''
    Write-Step "installing    : $infPath"
    & rundll32.exe 'setupapi.dll,InstallHinfSection' DefaultInstall 132 $infPath

    Start-Sleep -Seconds 3

    if (-not (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) {
        Write-Warn 'the service was not created by the INF install.'
        Write-Host '    Check C:\Windows\INF\setupapi.dev.log, or use -Mode Manual.'
    } else {
        Write-Ok 'driver package installed'
    }
}

# ---------------------------------------------------------------------------
# Verify + reboot
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '  verification'

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($service) {
    Write-Step "service state : $($service.Status)"
} else {
    Write-Warn 'service not present'
}

$filterList = (Get-ItemProperty -Path $classKey -Name 'UpperFilters' `
                                -ErrorAction SilentlyContinue).UpperFilters
Write-Step "UpperFilters  : $(@($filterList) -join ', ')"

Write-Host ''
Write-Host '  A reboot is required: the filter is attached when the disk'
Write-Host '  devices are started, not when the service is created.'
Write-Host ''
Write-Ok 'done. Reboot the VM, then run scripts\Test-RoundTrip.ps1'
Write-Host ''
Write-Host '  To roll back at any time:' -ForegroundColor Cyan
Write-Host '    .\scripts\Uninstall.ps1 -Reboot'
Write-Host ''
