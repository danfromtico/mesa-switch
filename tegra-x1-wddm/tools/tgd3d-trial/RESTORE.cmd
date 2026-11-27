@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Restore-AfterBoot.ps1"
exit /b %errorlevel%
