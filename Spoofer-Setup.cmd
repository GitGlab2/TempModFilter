@echo off
title HWID Spoofer - Setup (one click)
rem ============================================================
rem  ONE-CLICK SETUP: double-click me.
rem  1. Requests admin (one UAC popup - click YES).
rem  2. Enables test signing, signs the driver, installs it and
rem     arms a TEMPORARY one-boot session.
rem  3. Offers the reboot.  That reboot is the spoofing session.
rem
rem  After the session, ANY restart removes the driver, the
rem  certificate, the registry changes and test mode automatically.
rem
rem  Permanent install: run scripts\Install.ps1 without -Temporary.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights... click YES on the popup.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\OneClick-Spoof.ps1" -Mode setup %*
echo.
pause
