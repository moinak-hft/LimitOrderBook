@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-lab.ps1" -Mode simulate
if errorlevel 1 pause
