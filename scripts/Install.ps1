<#
.SYNOPSIS
    Installs TempModFilter as an upper filter of the DiskDrive setup class.

.DESCRIPTION
    LAB / TEST VM ONLY.

    The script:
      1. refuses to run outside a virtual machine unless -Force is given,
      2. trusts the bundled test certificate (TempModFilter.cer) when present,
      3. backs up the class UpperFilters value (so Uninstall can restore it
         exactly, instead of deleting somebody else's filter),
      4. copies the driver to %SystemRoot%\System32\drivers,
      5. creates the kernel service,
      6. appends "TempModFilter" to the DiskDrive class UpperFilters,
      7. asks for a reboot (the filter only attaches at device start).

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

    [switch]$Temporary,
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

function Find-Client {
    $candidates = @(
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

function Install-TemporarySession {
    # Copy the session scripts + client somewhere stable, then register the
    # one-shot "arm" task.  The arm script itself registers the finish-cleanup
    # task for the following boot.
    $dataDir = Join-Path $env:ProgramData 'TempModFilter'
    New-Item -Path $dataDir -ItemType Directory -Force | Out-Null

    foreach ($name in @('Session-Arm.ps1', 'Session-Cleanup.ps1')) {
        $source = Join-Path $scriptRoot $name
        if (-not (Test-Path $source)) {
            Write-Fail "temporary mode needs $name next to Install.ps1"
            return $false
        }
        Copy-Item -Path $source -Destination (Join-Path $dataDir $name) -Force
    }

    $client = Find-Client
    if ($client) {
        Copy-Item -Path $client -Destination (Join-Path $dataDir 'FilterClient.exe') -Force
        Write-Step "client copy  : $dataDir\FilterClient.exe"
    } else {
        Write-Warn 'FilterClient.exe not found; cleanup will still remove the driver.'
    }

    # A new session replaces any stale finish-cleanup task.
    if (Get-ScheduledTask -TaskName 'TempModFilter-FinishCleanup' -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName 'TempModFilter-FinishCleanup' -Confirm:$false -ErrorAction SilentlyContinue
    }

    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arm        = Join-Path $dataDir 'Session-Arm.ps1'

    $action    = New-ScheduledTaskAction -Execute $powershell `
                     -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$arm`""
    $trigger   = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
                     -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 5) `
                     -StartWhenAvailable -MultipleInstances IgnoreNew

    try {
        Register-ScheduledTask -TaskName 'TempModFilter-ArmSession' -Action $action `
            -Trigger $trigger -Principal $principal -Settings $settings -Force -ErrorAction Stop | Out-Null
    } catch {
        Write-Fail "could not register the arm task: $($_.Exception.Message)"
        return $false
    }

    if (-not (Get-ScheduledTask -TaskName 'TempModFilter-ArmSession' -ErrorAction SilentlyContinue)) {
        Write-Fail 'the arm task is not visible after registration.'
        return $false
    }

    Write-Ok 'temporary session armed (one boot only)'
    return $true
}

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
#
# Registering the filter while test signing is OFF is exactly the state that
# makes the next boot unbootable: the class filter is still in UpperFilters,
# but the kernel refuses to load the test-signed driver.  Therefore test
# signing is enabled *before* the filter is registered, and the install is
# aborted if that cannot be done.
#
# `bcdedit /set` is idempotent, so no localized text parsing is needed.  The
# bare form targets {current} (see "bcdedit /? /set") and is the fallback.

$tsOut = & bcdedit.exe /set '{current}' testsigning on 2>&1
if ($LASTEXITCODE -ne 0) {
    $tsOut = & bcdedit.exe /set testsigning on 2>&1
}
if ($LASTEXITCODE -eq 0) {
    Write-Step 'test signing  : ON'
} else {
    Write-Fail "test signing could not be enabled: $($tsOut | Out-String)"
    Write-Host '    Turn Secure Boot off in the firmware, or enable test'
    Write-Host '    signing manually, then run this script again.'
    Write-Host '    Refusing to register the filter blindly: doing that with'
    Write-Host '    test signing off can make the machine unable to boot.'
    exit 1
}

# ---------------------------------------------------------------------------
# Locate the driver binary
# ---------------------------------------------------------------------------

if (-not $DriverPath) {
    # Release package layout: the .sys sits next to this script.
    $beside = Join-Path $scriptRoot "$ServiceName.sys"
    if (Test-Path $beside) {
        $DriverPath = $beside
    }
}

if (-not $DriverPath) {
    $candidate = Get-ChildItem -Path (Join-Path $projectRoot 'build') -Recurse `
                               -Filter "$ServiceName.sys" -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending |
                 Select-Object -First 1

    if (-not $candidate) {
        Write-Fail "no $ServiceName.sys under build\ or next to this script."
        Write-Host '    Run scripts\Build.ps1 first, or run this script from the release package.'
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
# Trust the bundled test certificate (after the explicit confirmation)
# ---------------------------------------------------------------------------
# The release package ships the public half of the certificate that signed the
# driver.  Trusting it here is what makes the signature valid on this machine;
# without it the kernel rejects the image with error 577 (or 0xC0000428).

$cerPath = Join-Path $scriptRoot 'TempModFilter.cer'

if (Test-Path $cerPath) {
    try {
        $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cerPath

        if ($cert.Subject -eq 'CN=TempModFilter Lab Test') {
            foreach ($store in @('Root', 'TrustedPublisher')) {
                Import-Certificate -FilePath $cerPath `
                                   -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
            }
            Write-Step "test cert     : $($cert.Thumbprint) (trusted)"
        } else {
            Write-Warn "ignoring $cerPath - unexpected subject '$($cert.Subject)'."
            Write-Host '    Expected CN=TempModFilter Lab Test.'
        }
    } catch {
        Write-Warn "could not import $cerPath : $($_.Exception.Message)"
    }
} else {
    Write-Warn 'TempModFilter.cer not found next to this script.'
    Write-Host '    Run scripts\Sign-Test.ps1 -CerOutPath .\TempModFilter.cer, or'
    Write-Host '    import the signing certificate into TrustedPublisher manually.'
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

if ($Temporary) {
    Write-Host '  temporary mode (one boot only)'
    if (-not (Install-TemporarySession)) {
        Write-Fail 'temporary mode could not be armed - rolling the install back.'
        $cleanup = Join-Path $scriptRoot 'Session-Cleanup.ps1'
        if (Test-Path $cleanup) {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $cleanup -ServiceName $ServiceName
        }
        Write-Host '    The filter is no longer registered; run this script again.' -ForegroundColor Yellow
        exit 1
    }
    Write-Host '    * Reboot now. That boot is the spoofing session.'
    Write-Host '    * At startup the session is armed: the filter registration is'
    Write-Host '      removed and test signing is switched off for the next boot.'
    Write-Host '    * Any restart after the session removes the service, the .sys,'
    Write-Host '      the certificate, the registry changes and test mode.'
    Write-Host '    * If the machine loses power mid-session, the cleanup task'
    Write-Host '      still runs on the next boot - it can never leave the box'
    Write-Host '      unable to boot.'
    Write-Host ''
    Write-Ok 'done. Reboot now to start the one-boot temporary session.'
} else {
    Write-Ok 'done. Reboot the VM, then run scripts\Test-RoundTrip.ps1'
}

Write-Host ''
Write-Host '  To roll back at any time:' -ForegroundColor Cyan
Write-Host '    .\scripts\Uninstall.ps1 -Reboot'
