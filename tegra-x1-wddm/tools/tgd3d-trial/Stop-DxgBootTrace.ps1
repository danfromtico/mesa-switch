# SPDX-License-Identifier: MIT
# Adapted from the existing .158/.161 GPU trial workflow; see tool-provenance.json.
param([Parameter(Mandatory)][string]$TraceName)
$ErrorActionPreference='Stop'
$key=Join-Path 'HKLM:\SYSTEM\CurrentControlSet\Control\WMI\Autologger' $TraceName
if(Test-Path $key) {
    Set-ItemProperty $key Start 0
    if(Get-EtwTraceSession -Name $TraceName -ErrorAction SilentlyContinue) {
        Stop-EtwTraceSession -Name $TraceName -ErrorAction SilentlyContinue
    }
    Remove-Item -Path $key -Recurse -Force
}
