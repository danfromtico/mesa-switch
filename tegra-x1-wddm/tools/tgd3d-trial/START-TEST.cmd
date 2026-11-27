@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-Test.ps1"
exit /b %errorlevel%
