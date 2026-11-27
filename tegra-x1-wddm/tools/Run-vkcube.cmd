@echo off
rem SPDX-License-Identifier: MIT
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-vkcube.ps1" %*
exit /b %errorlevel%
