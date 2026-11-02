<#
.SYNOPSIS
    Creates a throw-away self-signed code-signing certificate and signs
    TempModFilter.sys so it loads on a machine with test signing enabled.

.DESCRIPTION
    LAB / VM ONLY.  A test-signed driver is rejected by any normal Windows
    installation (and by anything with Secure Boot + HVCI enabled).

    The certificate is imported into LocalMachine\Root and
    LocalMachine\TrustedPublisher of the *current* machine only.  Delete it
    with:

        Get-ChildItem Cert:\LocalMachine\My |
            Where-Object Subject -like '*TempModFilter*' |
            Remove-Item

.EXAMPLE
    .\Sign-Test.ps1
    .\Sign-Test.ps1 -SysPath ..\build\Release\x64\driver\TempModFilter.sys
    .\Sign-Test.ps1 -CerOutPath ..\package\TempModFilter.cer
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string]$SysPath,
    [string]$Subject    = 'CN=TempModFilter Lab Test',
    [string]$Password   = 'TempModFilter!2026',
    [string]$CerOutPath,
    [switch]$KeepCertificate
)

$ErrorActionPreference = 'Stop'

$scriptRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot
$workDir     = Join-Path $projectRoot 'build\signing'
$pfxPath     = Join-Path $workDir 'TempModFilter.pfx'
$cerPath     = Join-Path $workDir 'TempModFilter.cer'

function Find-NewestDriver {
    param([string]$Root)

    Get-ChildItem -Path (Join-Path $Root 'build') -Recurse -Filter 'TempModFilter.sys' `
                  -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Find-SignTool {
    $roots = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'),
        (Join-Path $env:ProgramFiles 'Windows Kits\10\bin')
    )

    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $tool = Get-ChildItem -Path $root -Recurse -Filter 'signtool.exe' `
                              -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\x64\\' } |
                Sort-Object FullName -Descending |
                Select-Object -First 1
        if ($tool) { return $tool.FullName }
    }

    return $null
}

# ---------------------------------------------------------------------------

if (-not $SysPath) {
    $found = Find-NewestDriver -Root $projectRoot
    if (-not $found) {
        Write-Host '[-] No TempModFilter.sys found. Run scripts\Build.ps1 first.' -ForegroundColor Red
        exit 1
    }
    $SysPath = $found.FullName
}

if (-not (Test-Path $SysPath)) {
    Write-Host "[-] Driver not found: $SysPath" -ForegroundColor Red
    exit 1
}

$signtool = Find-SignTool
if (-not $signtool) {
    Write-Host '[-] signtool.exe not found. Install the Windows SDK / WDK.' -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $workDir)) {
    New-Item -Path $workDir -ItemType Directory -Force | Out-Null
}

Write-Host ''
Write-Host '=== TempModFilter test signing ===' -ForegroundColor Cyan
Write-Host "    driver  : $SysPath"
Write-Host "    subject : $Subject"

# 1. Certificate ------------------------------------------------------------

$cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq $Subject } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

if (-not $cert) {
    Write-Host '    creating a new self-signed code-signing certificate...'
    $cert = New-SelfSignedCertificate `
                -Type CodeSigningCert `
                -Subject $Subject `
                -CertStoreLocation 'Cert:\LocalMachine\My' `
                -KeyUsage DigitalSignature `
                -KeyLength 2048 `
                -NotAfter (Get-Date).AddYears(3)
}

$securePassword = ConvertTo-SecureString -String $Password -Force -AsPlainText

Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $securePassword -Force | Out-Null
Export-Certificate   -Cert $cert -FilePath $cerPath -Force | Out-Null

# 2. Trust it locally so the chain validates without warnings --------------

foreach ($store in @('Root', 'TrustedPublisher')) {
    Import-PfxCertificate -FilePath $pfxPath `
                          -CertStoreLocation "Cert:\LocalMachine\$store" `
                          -Password $securePassword | Out-Null
}

Write-Host "    thumbprint: $($cert.Thumbprint)"

# 3. Sign -------------------------------------------------------------------

Write-Host ''
& $signtool sign /fd SHA256 /f $pfxPath /p $Password /v $SysPath

if ($LASTEXITCODE -ne 0) {
    Write-Host '[-] signtool failed.' -ForegroundColor Red
    exit 1
}

if (Test-Path $cerPath) {
    if ($CerOutPath) {
        $cerDir = Split-Path -Parent $CerOutPath
        if ($cerDir -and -not (Test-Path $cerDir)) {
            New-Item -ItemType Directory -Path $cerDir -Force | Out-Null
        }
        Copy-Item -Path $cerPath -Destination $CerOutPath -Force
        Write-Host ''
        Write-Step "public cert  : $CerOutPath"
        Write-Host '    Ship this file with the driver: it is the public half only,'
        Write-Host '    and the target machine needs it in TrustedPublisher.'
    }
    Remove-Item $cerPath -Force
}

Write-Host ''
Write-Host '[+] driver signed' -ForegroundColor Green
Write-Host ''
Write-Host 'Test signing must be ON in the target VM:'
Write-Host '    bcdedit /set testsigning on'
Write-Host '    (Secure Boot must be disabled in the VM settings, then reboot)'
Write-Host ''

if (-not $KeepCertificate) {
    Write-Host "Remember to remove '$Subject' from Cert:\LocalMachine\My," -ForegroundColor Yellow
    Write-Host 'Root and TrustedPublisher when you are done with the lab.' -ForegroundColor Yellow
    Write-Host ''
}
