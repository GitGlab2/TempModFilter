<#
.SYNOPSIS
    One-click engine for the HWID Spoofer toolkit.

.DESCRIPTION
    Runs with zero user knowledge required:
      Mode 'setup' : elevate, enable test signing, sign the driver,
                     install the kernel service, offer the reboot.
      Mode 'menu'  : friendly numbered menu on top of FilterClient.exe.

    The root-level Spoofer-Setup.cmd / Spoofer-Run.cmd launchers call this
    engine after self-elevating, so the user only ever clicks once (plus the
    single UAC "Yes" that Windows itself requires).

.EXAMPLE
    powershell -File scripts\OneClick-Spoof.ps1 -Mode setup
#>
[CmdletBinding()]
param(
    [ValidateSet('setup', 'menu')]
    [string]$Mode = 'setup'
)

$ErrorActionPreference = 'Continue'

$scriptRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot
$client      = $null

function Is-Admin {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal $identity
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Self-Elevate {
    # Relaunch this same engine in a brand-new elevated console, then leave.
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass',
              '-File', "`"$PSCommandPath`"", '-Mode', $Mode)
    try {
        Start-Process -FilePath 'powershell.exe' -ArgumentList $args -Verb RunAs | Out-Null
        exit 0
    } catch {
        Write-Host '[-] Elevation was cancelled. Nothing was changed.' -ForegroundColor Red
        exit 1
    }
}

function Find-Client {
    $candidates = @(
        (Join-Path $projectRoot 'build\manual\FilterClient.exe'),
        (Join-Path $projectRoot 'package\TempModFilter-Release-x64\FilterClient.exe'),
        (Join-Path $projectRoot 'build\Release\x64\user\FilterClient.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

function Should-Sign-Fresh {
    # Returns true when the .sys we can find is newer than the .cer we can
    # find (driver rebuilt after last signing), or no .cer exists at all.
    $sys = Get-ChildItem -Path (Join-Path $projectRoot 'build') -Recurse `
                         -Filter 'TempModFilter.sys' -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $sys) { return $false }

    $cer = Get-ChildItem -Path (Join-Path $projectRoot 'scripts') -Filter 'TempModFilter.cer' `
                         -ErrorAction SilentlyContinue
    if (-not $cer) { return $true }
    return $sys.LastWriteTime -gt $cer.LastWriteTime
}

function Banner {
    param([string]$Title)
    Clear-Host
    Write-Host ''
    Write-Host '=============================================================' -ForegroundColor Yellow
    Write-Host " HWID Spoofer  -  $Title" -ForegroundColor Yellow
    Write-Host ' LAB / TEST MACHINES ONLY' -ForegroundColor Yellow
    Write-Host '=============================================================' -ForegroundColor Yellow
    Write-Host ''
}

# ---------------------------------------------------------------------------
# Gate: everything below needs the elevated re-launch.
# ---------------------------------------------------------------------------
if (-not (Is-Admin)) {
    Write-Host 'Requesting administrator rights... (click YES on the popup)'
    Self-Elevate
}

$client = Find-Client

if ($Mode -eq 'setup') {

    Banner 'one-click setup'

    # -- 1. Sanity: driver binary present ----------------------------------
    $sys = Get-ChildItem -Path (Join-Path $projectRoot 'build') -Recurse `
                         -Filter 'TempModFilter.sys' -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $sys) {
        Write-Host '[-] TempModFilter.sys not found under build\. Run the build first.' -ForegroundColor Red
        Read-Host 'Press Enter to close'
        exit 1
    }
    Write-Host "[*] driver binary : $($sys.FullName)"

    # -- 2. Secure Boot check (informational) ------------------------------
    try {
        $sb = Confirm-SecureBootUEFI -ErrorAction Stop
        if ($sb) {
            Write-Host '[!] Secure Boot is ON. Test signing cannot be enabled until' -ForegroundColor Yellow
            Write-Host '    it is turned OFF in the BIOS/UEFI. Continuing anyway...'
        } else {
            Write-Host '[+] Secure Boot is OFF - good.'
        }
    } catch {
        Write-Host '[*] Secure Boot state unknown (legacy BIOS or query failed) - skipping.'
    }

    # -- 3. Test signing ----------------------------------------------------
    Write-Host '[*] enabling test signing (bcdedit /set testsigning on)...'
    $bcdOut = & bcdedit.exe /set testsigning on 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host '[+] test signing enabled. A REBOOT is required for it to apply.' -ForegroundColor Green
    } elseif ("$bcdOut" -match 'denied|access') {
        Write-Host "[-] bcdedit failed: $bcdOut" -ForegroundColor Red
    } else {
        Write-Host "    $bcdOut"
    }

    # -- 4. Sign the driver (only when needed) ------------------------------
    if (Should-Sign-Fresh) {
        Write-Host '[*] signing the driver with a throw-away test certificate...'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $scriptRoot 'Sign-Test.ps1') `
            -SysPath $sys.FullName `
            -CerOutPath (Join-Path $scriptRoot 'TempModFilter.cer')
        if ($LASTEXITCODE -ne 0) {
            Write-Host '[-] signing step failed. Install may still work if the cert' -ForegroundColor Yellow
            Write-Host '    was already trusted on this machine.'
        }
    } else {
        Write-Host '[+] driver is already signed (cer newer than sys) - skipping.'
    }

    # -- 5. Install the kernel service -------------------------------------
    Write-Host '[*] installing the kernel service...'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $scriptRoot 'Install.ps1') `
        -DriverPath $sys.FullName -Force -NoPrompt
    if ($LASTEXITCODE -ne 0) {
        Write-Host '[-] install step reported a problem - see messages above.' -ForegroundColor Red
    }

    # -- 6. Done page -------------------------------------------------------
    Write-Host ''
    Write-Host '=============================================================' -ForegroundColor Green
    if ($client) {
        Write-Host '[+] SETUP COMPLETE.' -ForegroundColor Green
        Write-Host '    1. Let Windows reboot now (required for test signing + filter).'
        Write-Host '    2. After the reboot: double-click Spoofer-Run.cmd'
        Write-Host '       and pick a numbered action from the menu.'
    } else {
        Write-Host '[!] Setup done, but FilterClient.exe was not found.' -ForegroundColor Yellow
        Write-Host '    Build the client, then run Spoofer-Run.cmd.'
    }
    Write-Host '=============================================================' -ForegroundColor Green
    Write-Host ''

    $answer = Read-Host 'Reboot NOW? (Y/n)'
    if ($answer -match '^n') {
        Write-Host '    Reboot later manually - changes are staged but inactive until then.'
    } else {
        Restart-Computer -Force
    }
}

if ($Mode -eq 'menu') {

    Banner 'quick menu'

    if (-not $client) {
        Write-Host '[-] FilterClient.exe not found. Run Spoofer-Setup.cmd first.' -ForegroundColor Red
        Read-Host 'Press Enter to close'
        exit 1
    }

    $running = $true
    while ($running) {
        Write-Host ''
        Write-Host '---------------------------------------------'
        Write-Host ' 1) Driver status          (is the filter up?)'
        Write-Host ' 2) Show machine identity   (disks/mac/regs)'
        Write-Host ' 3) BACKUP current IDs   <- always run first'
        Write-Host ' 4) SPOOF ALL (disk + registry, random)'
        Write-Host ' 5) SPOOF disk only (RAM, survives reboot)'
        Write-Host ' 6) WMI identity (show)'
        Write-Host ' 7) RESTORE original IDs (undo registry+filter)'
        Write-Host ' 8) Driver demo (before/after a disk query)'
        Write-Host ' 9) Exit'
        Write-Host '---------------------------------------------'
        $pick = Read-Host 'Pick a number'

        switch ($pick) {
            '1' { & $client status }
            '2' { & $client show }
            '3' { & $client backup }
            '4' { & $client spoof all }
            '5' { & $client spoof disk --all-drives --all-fields --random }
            '6' { & $client wmi-query }
            '7' { & $client restore }
            '8' { & $client demo }
            '9' { $running = $false }
            default { Write-Host '    pick 1-9.' -ForegroundColor DarkGray }
        }
    }
}
