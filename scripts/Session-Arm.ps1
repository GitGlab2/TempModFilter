<#
.SYNOPSIS
    Turns a freshly booted spoofing session into a one-boot session.

.DESCRIPTION
    Registered by Install.ps1 -Temporary and executed at startup (as SYSTEM).
    The class filter has already attached to the disk devices at this point,
    so the driver keeps working for the current boot even after we do this:

      1. remove "TempModFilter" from the DiskDrive class UpperFilters value -
         the NEXT boot must not try to load a test-signed driver after test
         signing is off,
      2. turn test signing OFF in the BCD store - effective on the next boot,
      3. register TempModFilter-FinishCleanup for the next startup, which
         deletes the service, the .sys and the certificate, restores the
         registry identity backup and retries any leftover step.

    Result: one boot of spoofing, and any restart after it is clean - even if
    the machine loses power or bugchecks during the session.

    If the filter did not load, nothing is attached, so the script retries on
    the next boot; after three failed boots it drops the session anyway so the
    machine can never stay in test mode.  It never turns test signing off while
    the filter is still registered.
#>
[CmdletBinding()]
param(
    [string]$ServiceName = 'TempModFilter',
    [string]$ClassGuid   = '{4D36E967-E325-11CE-BFC1-08002BE10318}',
    [int]$TimeoutSeconds = 90
)

$ErrorActionPreference = 'Continue'

# Accept both an elevated administrator and the SYSTEM account (the startup
# task runs as SYSTEM).
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
$stateFile  = Join-Path $dataDir 'session.json'
$logFile    = Join-Path $dataDir 'session.log'
$taskArm    = 'TempModFilter-ArmSession'
$taskFinish = 'TempModFilter-FinishCleanup'

if (-not (Test-Path $dataDir)) {
    New-Item -Path $dataDir -ItemType Directory -Force | Out-Null
}

function Write-Log {
    param(
        [string]$Message,
        [ValidateSet('INFO', 'OK', 'WARN', 'ERR')]
        [string]$Level = 'INFO'
    )
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message
    try { Add-Content -Path $logFile -Value $line -Encoding UTF8 -ErrorAction Stop } catch { }
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

function Get-UpperFilters {
    try {
        $value = (Get-ItemProperty -Path $classKey -Name 'UpperFilters' -ErrorAction Stop).UpperFilters
        return @($value | Where-Object { $_ })
    } catch {
        return @()
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

Write-Log '=== TempModFilter session arm ==='

# ---------------------------------------------------------------------------
# 0. Is there anything to arm at all?
# ---------------------------------------------------------------------------
if (-not ((Get-UpperFilters) -contains $ServiceName)) {
    Write-Log 'filter is not registered - no session to arm; removing the arm task'
    Unregister-ScheduledTask -TaskName $taskArm -Confirm:$false -ErrorAction SilentlyContinue
    exit 0
}

# The cleanup script must exist before anything is changed; otherwise this
# boot would be armed without a way to finish the cleanup.
$cleanup = Join-Path $scriptRoot 'Session-Cleanup.ps1'
if (-not (Test-Path $cleanup)) {
    Write-Log "Session-Cleanup.ps1 not found next to this script ($scriptRoot); leaving everything as-is" 'ERR'
    exit 1
}

# ---------------------------------------------------------------------------
# 1. Wait until the filter is really loaded for this boot
# ---------------------------------------------------------------------------
$client = Find-Client
$loaded = $false
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

Write-Log "waiting up to $TimeoutSeconds s for $ServiceName to load ..."
while ((Get-Date) -lt $deadline) {
    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($service -and $service.Status -eq 'Running') {
        $loaded = $true
        break
    }
    if ($client) {
        & $client status *> $null
        if ($LASTEXITCODE -eq 0) {
            $loaded = $true
            break
        }
    }
    Start-Sleep -Seconds 5
}

if (-not $loaded) {
    # The filter never attached, so this boot cannot spoof anything.  Retry a
    # couple of boots (the driver may simply be slow), but never leave the
    # machine sitting in test mode forever: after 3 failed boots we remove the
    # session the normal way (registration first, then test signing, then the
    # finish-cleanup task).
    $attemptFile = Join-Path $dataDir 'arm-attempts.txt'
    $attempts = 0
    if (Test-Path $attemptFile) {
        $attempts = [int](Get-Content $attemptFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    }
    $attempts++
    try { Set-Content -Path $attemptFile -Value $attempts -Encoding ASCII } catch { }

    if ($attempts -ge 3) {
        Write-Log "filter not loaded after $attempts boots; removing the session so the machine does not stay in test mode" 'WARN'
        # Fall through: nothing is attached, so dropping the registration now
        # is safe and the cleanup below still reverts everything on the next
        # boot.
    } else {
        Write-Log "filter is not loaded (attempt $attempts of 3); retrying on the next boot" 'WARN'
        # Keep the task registered - the session may simply not have started yet.
        exit 1
    }
} else {
    Write-Log 'filter is loaded for this boot' 'OK'

    # Give PnP a moment to finish starting the remaining disks while the filter
    # is still registered.  The boot disk is already up; this just widens the
    # window for secondary disks.  The delay runs before user logon.
    Start-Sleep -Seconds 10
}

# ---------------------------------------------------------------------------
# 2. Remove the class filter registration.  The current device stack stays
#    intact (UpperFilters is only read when a device is started), so the
#    driver keeps serving this boot.
# ---------------------------------------------------------------------------
$filters = Get-UpperFilters
if ($filters -contains $ServiceName) {
    $newFilters = @($filters | Where-Object { $_ -ne $ServiceName })
    if (@($newFilters).Count -eq 0) {
        Remove-ItemProperty -Path $classKey -Name 'UpperFilters' -ErrorAction SilentlyContinue
    } else {
        New-ItemProperty -Path $classKey -Name 'UpperFilters' `
            -PropertyType MultiString -Value ([string[]]$newFilters) -Force | Out-Null
    }
    Write-Log "removed $ServiceName from UpperFilters (next boot will not load it)" 'OK'
}
if ((Get-UpperFilters) -contains $ServiceName) {
    Write-Log 'could not remove the filter registration; aborting without touching test signing' 'ERR'
    exit 1
}

# ---------------------------------------------------------------------------
# 3. Turn test signing off for the next boot
# ---------------------------------------------------------------------------
if (Set-TestSigning -State off) {
    Write-Log 'test signing turned OFF (this boot stays test-signed; the next one is clean)' 'OK'
} else {
    Write-Log 'could not turn test signing off' 'ERR'
    # The filter registration is already gone, so rebooting is still safe;
    # put both back so the arm task can retry on the next boot.
    New-ItemProperty -Path $classKey -Name 'UpperFilters' -PropertyType MultiString `
        -Value ([string[]](@(Get-UpperFilters) + $ServiceName)) -Force | Out-Null
    Set-TestSigning -State on | Out-Null
    exit 1
}

# ---------------------------------------------------------------------------
# 4. Schedule the finish cleanup for the next boot
# ---------------------------------------------------------------------------
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$cleanup`" -RestoreRegistry -RemoveCertificate -Quiet"

$action = New-ScheduledTaskAction -Execute $powershell -Argument $arguments
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 5) -StartWhenAvailable -MultipleInstances IgnoreNew

$taskOk = $false
try {
    Register-ScheduledTask -TaskName $taskFinish -Action $action -Trigger $trigger `
        -Principal $principal -Settings $settings -Force -ErrorAction Stop | Out-Null
    $taskOk = [bool](Get-ScheduledTask -TaskName $taskFinish -ErrorAction SilentlyContinue)
} catch {
    Write-Log "could not register $($taskFinish): $($_.Exception.Message)" 'ERR'
}

if (-not $taskOk) {
    # Put the machine back into the pre-session, bootable state and let the
    # arm task retry on the next boot.
    $restore = @(Get-UpperFilters)
    if ($restore -notcontains $ServiceName) { $restore += $ServiceName }
    New-ItemProperty -Path $classKey -Name 'UpperFilters' -PropertyType MultiString `
        -Value ([string[]]$restore) -Force | Out-Null
    Set-TestSigning -State on | Out-Null
    Write-Log 'rolled back to a bootable test-signed session; will retry on the next boot' 'WARN'
    exit 1
}
Write-Log "task registered: $taskFinish (runs at the next startup)" 'OK'

# ---------------------------------------------------------------------------
# 5. Record the session and remove the one-shot arm task
# ---------------------------------------------------------------------------
$state = [pscustomobject]@{
    Temporary       = $true
    SessionBoot     = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')
    ArmedAt         = (Get-Date).ToString('o')
    FilterLoaded    = $loaded
    TestSigningNext = 'off'
}
try {
    $state | ConvertTo-Json -Depth 3 | Set-Content -Path $stateFile -Encoding UTF8
} catch { }

Unregister-ScheduledTask -TaskName $taskArm -Confirm:$false -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $dataDir 'arm-attempts.txt') -Force -ErrorAction SilentlyContinue
if ($loaded) {
    Write-Log '=== session armed: restart at any time and everything reverts ===' 'OK'
} else {
    Write-Log '=== session dropped (the filter never loaded): the next boot is clean ===' 'OK'
}
exit 0
