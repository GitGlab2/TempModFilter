@echo off
title HWID Spoofer - Quick Menu (one click)
rem ============================================================
rem  ONE-CLICK MENU: double-click me AFTER Spoofer-Setup + reboot.
rem  Shows a numbered menu (spoof all, backup, restore, ...).
rem
rem  Temporary mode: this boot is the only spoofing session.
rem  Options 10/11 clean up immediately / clean up + restart.
rem  Any plain restart also removes everything automatically.
rem
rem  Runs elevated because spoofing touches HKLM registry.
rem ============================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator rights... click YES on the popup.
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\OneClick-Spoof.ps1" -Mode menu
echo.
pause
