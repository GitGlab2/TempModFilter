<#
.SYNOPSIS
    Removes TempModFilter and returns the machine to its normal state.

.DESCRIPTION
    Thin wrapper around Session-Cleanup.ps1, which is the single place that
    knows how to remove the filter safely:

      * unregisters the disk class filter (boot-critical),
      * deletes the service and the driver file,
      * removes the lab certificate,
      * restores the identity backup (MachineGuid, ProductId, hostname, ...),
      * turns test signing OFF - but only after the filter is really gone.

    If anything is still in use, test signing is kept ON and the
    TempModFilter-FinishCleanup task retries on the next boot.  The machine
    can never be left in the "test signing off + filter still registered"
    state that caused the unbootable system.

.EXAMPLE
    .\Uninstall.ps1
    .\Uninstall.ps1 -Reboot
    .\Uninstall.ps1 -KeepIdentity
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string]$ServiceName = 'TempModFilter',
    [string]$ClassGuid   = '{4D36E967-E325-11CE-BFC1-08002BE10318}',

    # Kept for compatibility with older releases.  The cleanup always removes
    # only our own UpperFilters entry and never deletes somebody else's filter.
    [switch]$RestoreBackup,

    # Do not restore the registry identity backup; only remove the driver.
    [switch]$KeepIdentity,

    [switch]$Reboot,
    [switch]$NoPrompt
)

$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$cleanup    = Join-Path $scriptRoot 'Session-Cleanup.ps1'

Write-Host ''
Write-Host '=== TempModFilter uninstall ===' -ForegroundColor Cyan

if (-not (Test-Path $cleanup)) {
    Write-Host "[-] Session-Cleanup.ps1 not found next to this script." -ForegroundColor Red
    Write-Host '    Run the cleanup from the release package that contains it.' -ForegroundColor Red
    exit 1
}

if (-not $NoPrompt) {
    Write-Host ''
    Write-Host '  This removes the filter from the DiskDrive class, deletes the'
    Write-Host '  service and the driver file, restores the identity backup and'
    Write-Host '  switches test signing off.  A reboot is required.'
    Write-Host ''
    $answer = Read-Host 'Type REMOVE to continue'
    if ($answer -ne 'REMOVE') {
        Write-Host '[-] aborted.'
        exit 1
    }
}

$arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', $cleanup,
    '-ServiceName', $ServiceName,
    '-ClassGuid', $ClassGuid,
    '-RemoveCertificate'
)
if (-not $KeepIdentity) { $arguments += '-RestoreRegistry' }

& powershell.exe @arguments
$exitCode = $LASTEXITCODE
if ($null -eq $exitCode) { $exitCode = 1 }

Write-Host ''
if ($exitCode -eq 0) {
    Write-Host '[+] machine is clean. Reboot to detach the filter from RAM.' -ForegroundColor Green
} else {
    Write-Host '[!] cleanup has leftovers; it will retry automatically on the next boot.' -ForegroundColor Yellow
}

if ($Reboot) {
    Write-Host '  Rebooting in 10 seconds (Ctrl+C to cancel)...'
    shutdown.exe /r /t 10 /c "TempModFilter removal"
} else {
    Write-Host '  Run with -Reboot to restart automatically.'
}

exit $exitCode
