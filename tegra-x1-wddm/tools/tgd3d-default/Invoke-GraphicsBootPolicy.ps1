# SPDX-License-Identifier: MIT
# Stable entry installed as Program Files/SwitchWOA-GpuBoot/Native-Boot.ps1.
[CmdletBinding()]
param([ValidateSet('Trial','Accept','Restore','Disarm')][string]$Mode='Trial',
      [string]$Directory,[string]$VerifiedKmd,[string]$VerifiedUmd,[string]$VerifiedIcd,
      [string]$Qualification,[string]$LedgerSha256)
$ErrorActionPreference='Stop'
$root=Join-Path $env:ProgramFiles 'SwitchWOA-GpuBoot'
$revision=[IO.File]::ReadAllText((Join-Path $root 'active-policy.txt')).Trim()
if($revision -notmatch '^[0-9a-f]{64}$'){throw 'Invalid active policy pointer'}
$folder=Join-Path (Join-Path $root 'policies') $revision
$manifestPath=Join-Path $folder 'manifest.json'
if((Get-FileHash -LiteralPath $manifestPath).Hash -ne $revision){throw 'Invalid active policy snapshot'}
$m=[IO.File]::ReadAllText($manifestPath)|ConvertFrom-Json -ErrorAction Stop
$tool=Join-Path $folder 'Native-Boot.ps1'
if($m.schema -ne 1 -or (Get-FileHash -LiteralPath $tool).Hash -ne $m.files.'Native-Boot.ps1'){throw 'Active policy entry changed'}
if(!$PSBoundParameters.ContainsKey('Directory') -and $Mode -ne 'Disarm'){
    $PSBoundParameters['Directory']=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault' -ErrorAction Stop).Directory
}
& $tool @PSBoundParameters
