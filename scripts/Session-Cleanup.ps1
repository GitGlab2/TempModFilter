<#
.SYNOPSIS
    Removes every trace of a TempModFilter session and safely restores the
    machine to its normal state.

.DESCRIPTION
    Idempotent and safe to run at any time, including on a machine where the
    driver was never installed.

    It is called by:
      * the TempModFilter-FinishCleanup scheduled task on the first boot after
        a spoofing session,
      * Uninstall.ps1,
      * the Spoofer-Run.cmd menu ("Clean up now" / "Clean up + restart").

    Boot-safety order matters:
      1. remove the DiskDrive class filter registration, so the next boot can
         never try to load a test-signed driver while test signing is off,
      2. stop/delete the service, delete the .sys (or schedule its deletion
         for the next boot when it is still mapped),
      3. restore the registry identity backup and remove the lab certificate,
      4. only then turn test signing off - and only when the filter is really
         no longer registered.  If the registration survives, test signing
         stays ON, the FinishCleanup task stays registered and retries on the
         next boot.  The machine therefore never becomes unbootable.

.PARAMETER RestoreRegistry
    Restore the values captured by FilterClient.exe (MachineGuid, ProductId,
    hostname, HwProfileGuid, SQM id, InstallDate, MAC NetworkAddress).

.PARAMETER RemoveCertificate
    Remove the "CN=TempModFilter Lab Test" certificate from the machine Root
    and TrustedPublisher stores.

.PARAMETER Quiet
    Log only; no console output.  Used by the scheduled tasks.

.EXAMPLE
    .\Session-Cleanup.ps1 -RestoreRegistry -RemoveCertificate
    .\Session-Cleanup.ps1 -Quiet
#>
[CmdletBinding()]
param(
    [string]$ServiceName = 'TempModFilter',
    [string]$ClassGuid   = '{4D36E967-E325-11CE-BFC1-08002BE10318}',

    [switch]$RestoreRegistry,
    [switch]$RemoveCertificate,
    [switch]$Quiet,
    [switch]$KeepTasks,

    [string]$LogPath
)

$ErrorActionPreference = 'Continue'

# Accept both an elevated administrator and the SYSTEM account (scheduled
# tasks), and refuse everything else.  #Requires -RunAsAdministrator is not
# used here because the cleanup runs from a SYSTEM startup task.
$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isSystem = ($currentIdentity.User.Value -eq 'S-1-5-18')
$isAdmin  = (New-Object Security.Principal.WindowsPrincipal $currentIdentity).IsInRole(
                [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not ($isSystem -or $isAdmin)) {
    Write-Host '[-] Administrator or SYSTEM rights are required.' -ForegroundColor Red
    exit 1
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot
$dataDir    = Join-Path $env:ProgramData 'TempModFilter'
$classKey   = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$ClassGuid"
$driverDest = Join-Path $env:SystemRoot "System32\drivers\$ServiceName.sys"
$stateFile  = Join-Path $dataDir 'session.json'
$upperBak   = Join-Path $dataDir 'UpperFilters.backup.json'
$taskArm    = 'TempModFilter-ArmSession'
$taskFinish = 'TempModFilter-FinishCleanup'

if (-not (Test-Path $dataDir)) {
    New-Item -Path $dataDir -ItemType Directory -Force | Out-Null
}
if (-not $LogPath) {
    $LogPath = Join-Path $dataDir 'cleanup.log'
}

$script:Problems = 0

function Write-Log {
    param(
        [string]$Message,
        [ValidateSet('INFO', 'OK', 'WARN', 'ERR')]
        [string]$Level = 'INFO'
    )
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message
    try { Add-Content -Path $LogPath -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
    if ($Quiet) { return }
    switch ($Level) {
        'OK'   { Write-Host "[+] $Message" -ForegroundColor Green }
        'WARN' { Write-Host "[!] $Message" -ForegroundColor Yellow }
        'ERR'  { Write-Host "[-] $Message" -ForegroundColor Red }
        default { Write-Host "    $Message" }
    }
}

function Find-Client {
    $candidates = @(
        (Join-Path $dataDir 'FilterClient.exe'),
        (Join-Path $scriptRoot 'FilterClient.exe'),
        (Join-Path $projectRoot 'build\manual\FilterClient.exe'),
        (Join-Path $projectRoot 'package\TempModFilter-Release-x64\FilterClient.exe'),
        (Join-Path $projectRoot 'build\Release\x64\user\FilterClient.exe'),
        (Join-Path $projectRoot 'build\Debug\x64\user\FilterClient.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

function Add-PendingDelete {
    # Schedule a file for deletion at the next boot (MOVEFILE_DELAY_UNTIL_REBOOT).
    param([string]$Path)
    try {
        if (-not ('TempMod.NativeMethods' -as [type])) {
            Add-Type -Namespace TempMod -Name NativeMethods -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool MoveFileEx(string lpExistingFileName, string lpNewFileName, int dwFlags);
'@
        }
        return [TempMod.NativeMethods]::MoveFileEx($Path, $null, 4)
    } catch {
        return $false
    }
}

function Get-UpperFilters {
    try {
        $value = (Get-ItemProperty -Path $classKey -Name 'UpperFilters' -ErrorAction Stop).UpperFilters
        return @($value | Where-Object { $_ })
    } catch {
        return @()
    }
}

function Set-UpperFilters {
    param([string[]]$Values)
    if (@($Values).Count -eq 0) {
        Remove-ItemProperty -Path $classKey -Name 'UpperFilters' -ErrorAction SilentlyContinue
    } else {
        New-ItemProperty -Path $classKey -Name 'UpperFilters' `
            -PropertyType MultiString -Value ([string[]]$Values) -Force | Out-Null
    }
}

function Set-TestSigning {
    # Sets testsigning without parsing localized bcdedit output.  The bare
    # form means {current} (see "bcdedit /? /set"), so try the explicit id
    # first and fall back to the bare form.
    param([ValidateSet('on', 'off')][string]$State)
    $out = (& bcdedit.exe /set '{current}' testsigning $State 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0) { return $true }
    $out = (& bcdedit.exe /set testsigning $State 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0) { return $true }
    Write-Log "bcdedit testsigning $State failed: $($out.Trim())" 'ERR'
    return $false
}

Write-Log '=== TempModFilter cleanup ==='

# ---------------------------------------------------------------------------
# 0. Drop the "arm" task.  It is only useful before the session boot; the
#    FinishCleanup task is removed at the very end, and only on success.
# ---------------------------------------------------------------------------
if (-not $KeepTasks) {
    if (Get-ScheduledTask -TaskName $taskArm -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $taskArm -Confirm:$false -ErrorAction SilentlyContinue
        Write-Log "task removed: $taskArm" 'OK'
    }
}

# ---------------------------------------------------------------------------
# 1. DiskDrive class filter registration  (the boot-critical bit)
# ---------------------------------------------------------------------------
$filters = Get-UpperFilters
if ($filters -contains $ServiceName) {
    $newFilters = @($filters | Where-Object { $_ -ne $ServiceName })
    Set-UpperFilters -Values $newFilters
    if (@($newFilters).Count -eq 0) {
        Write-Log "removed $ServiceName from UpperFilters (value is now empty)" 'OK'
    } else {
        Write-Log "removed $ServiceName from UpperFilters (now: $($newFilters -join ', '))" 'OK'
    }
} else {
    Write-Log "$ServiceName is not registered in UpperFilters"
}

# Re-read: this is the only value that decides whether test signing may be
# turned off safely.
$filterStillRegistered = (Get-UpperFilters) -contains $ServiceName

# ---------------------------------------------------------------------------
# 2. Service, driver file
# ---------------------------------------------------------------------------
$service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($service) {
    & sc.exe stop $ServiceName 2>&1 | Out-Null
    Start-Sleep -Milliseconds 800
    $out = & sc.exe delete $ServiceName 2>&1
    Start-Sleep -Milliseconds 500
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        Write-Log "service $ServiceName is still present; it will disappear after the reboot" 'WARN'
    } else {
        Write-Log "service $ServiceName deleted" 'OK'
    }
} else {
    Write-Log "service $ServiceName not present"
}

$filePending = $false
if (Test-Path $driverDest) {
    try {
        Remove-Item -Path $driverDest -Force -ErrorAction Stop
        Write-Log "deleted $driverDest" 'OK'
    } catch {
        if (Add-PendingDelete $driverDest) {
            $filePending = $true
            Write-Log "driver file is mapped; scheduled for deletion at the next boot" 'WARN'
        } else {
            $script:Problems++
            Write-Log "could not delete $driverDest : $($_.Exception.Message)" 'ERR'
        }
    }
} else {
    Write-Log "driver file not present"
}

# ---------------------------------------------------------------------------
# 3. Registry identity + lab certificate
# ---------------------------------------------------------------------------
if ($RestoreRegistry) {
    $client = Find-Client
    if ($client) {
        Write-Log "restoring identity backup via $client"
        $out = & $client restore 2>&1
        foreach ($line in @($out)) { if ($line) { Write-Log "  $line" } }
        if ($LASTEXITCODE -eq 0) {
            Write-Log 'identity backup restored' 'OK'
        } else {
            Write-Log 'identity restore reported a problem (no backup is fine on a fresh session)' 'WARN'
        }
    } else {
        Write-Log 'FilterClient.exe not found; identity backup was not restored' 'WARN'
    }
}

if ($RemoveCertificate) {
    $removed = 0
    foreach ($store in @('Root', 'TrustedPublisher')) {
        $certs = @(Get-ChildItem -Path "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue |
                   Where-Object { $_.Subject -eq 'CN=TempModFilter Lab Test' })
        foreach ($cert in $certs) {
            Remove-Item -Path $cert.PSPath -Force -ErrorAction SilentlyContinue
            $removed++
            Write-Log "removed lab certificate from LocalMachine\$store ($($cert.Thumbprint))" 'OK'
        }
    }
    if ($removed -eq 0) {
        Write-Log 'lab certificate not present in Root/TrustedPublisher'
    }
}

# ---------------------------------------------------------------------------
# 4. Test signing - the safety gate
# ---------------------------------------------------------------------------
$testSigningOff = $false
if ($filterStillRegistered) {
    $script:Problems++
    Write-Log 'test signing kept ON: the filter is still registered for the next boot.' 'WARN'
} else {
    if (Set-TestSigning -State off) {
        $testSigningOff = $true
        Write-Log 'test signing turned OFF (effective after the next reboot)' 'OK'
    } else {
        $script:Problems++
        Write-Log 'could not turn test signing off; it will be retried on the next boot' 'ERR'
    }
}

# ---------------------------------------------------------------------------
# 5. State file + FinishCleanup task
# ---------------------------------------------------------------------------
Remove-Item -Path $stateFile -Force -ErrorAction SilentlyContinue
Remove-Item -Path $upperBak -Force -ErrorAction SilentlyContinue

$fullyClean = (-not $filterStillRegistered) -and $testSigningOff -and
              (-not (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) -and
              (-not (Test-Path $driverDest))

if ($fullyClean -and -not $KeepTasks) {
    if (Get-ScheduledTask -TaskName $taskFinish -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $taskFinish -Confirm:$false -ErrorAction SilentlyContinue
        Write-Log "task removed: $taskFinish" 'OK'
    }
}

if ($fullyClean) {
    Write-Log '=== cleanup complete: machine is back to its normal state ===' 'OK'
    exit 0
}

Write-Log '=== cleanup incomplete: it will retry on the next boot ===' 'WARN'
exit 1
