@echo off
title HWID Spoofer - Setup (one click)
rem ============================================================
rem  ONE-CLICK SETUP: double-click me.
rem  1. Requests admin (one UAC popup - click YES).
rem  2. Enables test signing, signs the driver, installs it.
rem  3. Offers the reboot; afterwards use Spoofer-Run.cmd.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights... click YES on the popup.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\OneClick-Spoof.ps1" -Mode setup
echo.
pause
