<#
.SYNOPSIS
    Removes TempModFilter from the machine and restores the previous
    DiskDrive class filter list.

.DESCRIPTION
    Order matters: first the service, then the registry entry, then the file.
    If a backup of the original UpperFilters value exists (written by
    Install.ps1), it is restored verbatim so that no other filter is lost.

.EXAMPLE
    .\Uninstall.ps1
    .\Uninstall.ps1 -Reboot
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string]$ServiceName = 'TempModFilter',
    [string]$ClassGuid   = '{4D36E967-E325-11CE-BFC1-08002BE10318}',

    [switch]$RestoreBackup,
    [switch]$Reboot,
    [switch]$NoPrompt
)

$ErrorActionPreference = 'Stop'

$classKey   = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$ClassGuid"
$backupDir  = Join-Path $env:ProgramData 'TempModFilter'
$backupFile = Join-Path $backupDir 'UpperFilters.backup.json'
$driverDest = Join-Path $env:SystemRoot "System32\drivers\$ServiceName.sys"

function Write-Step { param([string]$Text) Write-Host "    $Text" }
function Write-Ok   { param([string]$Text) Write-Host "[+] $Text" -ForegroundColor Green }
function Write-Warn { param([string]$Text) Write-Host "[!] $Text" -ForegroundColor Yellow }

Write-Host ''
Write-Host '=== TempModFilter uninstall ===' -ForegroundColor Cyan

if (-not $NoPrompt) {
    Write-Host ''
    Write-Host '  This removes the filter from the DiskDrive class and deletes'
    Write-Host '  the service and the driver file. A reboot is required.'
    Write-Host ''
    $answer = Read-Host 'Type REMOVE to continue'
    if ($answer -ne 'REMOVE') {
        Write-Host '[-] aborted.'
        exit 1
    }
}

# ---------------------------------------------------------------------------
# 1. Service
# ---------------------------------------------------------------------------

$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($service) {

    Write-Step "stopping $ServiceName (may fail while disks still hold it)"

    & sc.exe stop $ServiceName | Out-Null
    Start-Sleep -Seconds 1

    & sc.exe delete $ServiceName | Out-Null
    Start-Sleep -Milliseconds 500

    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        Write-Warn 'service still present; it will disappear after the reboot.'
    } else {
        Write-Ok 'service deleted'
    }
} else {
    Write-Step 'service not present'
}

# ---------------------------------------------------------------------------
# 2. UpperFilters
# ---------------------------------------------------------------------------

$current = Get-ItemProperty -Path $classKey -Name 'UpperFilters' `
                            -ErrorAction SilentlyContinue
$list = @()
if ($current -and $current.UpperFilters) {
    $list = @($current.UpperFilters)
}

if ($RestoreBackup -and (Test-Path $backupFile)) {

    $backup = Get-Content -Path $backupFile -Raw | ConvertFrom-Json

    if ($backup.ClassKey -ne $classKey) {
        Write-Warn "backup was taken for $($backup.ClassKey); not restoring it."
    } else {

        $restored = @($backup.Values | Where-Object { $_ })

        if ($backup.Existed) {
            New-ItemProperty -Path $classKey -Name 'UpperFilters' `
                             -PropertyType MultiString `
                             -Value ([string[]]$restored) -Force | Out-Null
            Write-Ok "UpperFilters restored to: $($restored -join ', ')"
        } else {
            Remove-ItemProperty -Path $classKey -Name 'UpperFilters' `
                                -ErrorAction SilentlyContinue
            Write-Ok 'UpperFilters value removed (it did not exist before)'
        }

        Remove-Item -Path $backupFile -Force -ErrorAction SilentlyContinue
        $list = $restored
    }
}

if ($list -contains $ServiceName) {

    $filtered = @($list | Where-Object { $_ -ne $ServiceName })

    if ($filtered.Count -eq 0) {
        Remove-ItemProperty -Path $classKey -Name 'UpperFilters' `
                            -ErrorAction SilentlyContinue
    } else {
        New-ItemProperty -Path $classKey -Name 'UpperFilters' `
                         -PropertyType MultiString `
                         -Value ([string[]]$filtered) -Force | Out-Null
    }

    Write-Ok "removed from UpperFilters (now: $($filtered -join ', '))"
} else {
    Write-Step "$ServiceName is not in UpperFilters"
}

# ---------------------------------------------------------------------------
# 3. Driver file
# ---------------------------------------------------------------------------

if (Test-Path $driverDest) {
    try {
        Remove-Item -Path $driverDest -Force
        Write-Ok "deleted $driverDest"
    } catch {
        Write-Warn "could not delete $driverDest (in use); delete it after the reboot."
    }
}

Write-Host ''
Write-Host '  Reboot to detach the filter from the storage stack.' -ForegroundColor Yellow
Write-Host ''

if ($Reboot) {
    Write-Host '  Rebooting in 10 seconds (Ctrl+C to cancel)...'
    shutdown.exe /r /t 10 /c "TempModFilter removal"
} else {
    Write-Host '  Run with -Reboot to restart automatically.'
}
