# SPDX-License-Identifier: MIT
. (Join-Path $PSScriptRoot 'Merge-Common.ps1')
. (Join-Path $PSScriptRoot 'Emc-Readiness.ps1')
$state=Join-Path $MergeRoot 'install-state.json';if(!(Test-Path $state)){return};$r=Read-Json $state
if($r.Phase -ne 'StagedRebootRequested'){return}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
if([Math]::Abs(([DateTime]::Parse($boot)-[DateTime]::Parse($r.Boot)).TotalSeconds) -le 2){throw 'The planned gate/ACPI restart has not occurred'}
Disable-ScheduledTask Tgd3dDvfsInstall|Out-Null
$r|Add-Member -NotePropertyName AfterBoot -NotePropertyValue $boot -Force
try {
 $m=Merge-Check
 if((Device-Property 'ProblemCode') -ne 43){throw 'Native gate is not closed'}
 Check-File $m.Before.FirmwarePath $m.Files.'firmware\bl33.bin'
 Check-File "$env:windir\System32\acpitabl.dat" $m.Files.'gpu\display158.aml'
 $r.Phase='InstallingPlatform';Save-Json $state $r
 # Install the existing VDD_CORE owner before its new SwitchFan client.
 foreach($item in @(@{Name='switchpwrbtn';HardwareId='ACPI\SWOA0601'},@{Name='switchfan';HardwareId='ACPI\SWOA0402'},@{Name='switchbattery';HardwareId='ACPI\SWOA0314'})) {
  $dir=Join-Path $MergeRoot ('platform\'+$item.Name)
  $result=Update-PlatformDriver $item.Name $item.HardwareId (Join-Path $dir ($item.Name+'.inf')) (File-Hash (Join-Path $dir ($item.Name+'.sys')))
  if($result.RebootRequired){throw ('Platform restart required before activation: '+$item.Name)}
 }
 $deadline=[DateTime]::UtcNow.AddSeconds(15);$p=$null;$emc=$null
 do{
  try{$p=& (Join-Path $MergeRoot 'Power-State.ps1');$emc=& (Join-Path $MergeRoot 'Power-State.ps1') -EmcOnly}catch{$p=$null;$emc=$null}
  if($p -and $emc -and $p.Power.CpuConfigured -and
     (Test-EmcBeforeNative $p.Power $emc.Emc (Device-Property 'ProblemCode'))){break}
  Start-Sleep -Milliseconds 500
 }while([DateTime]::UtcNow -lt $deadline)
 if($p){Save-Json (Join-Path $MergeRoot 'platform-before-native.json') $p}
 if($emc){Save-Json (Join-Path $MergeRoot 'emc-before-native.json') $emc}
 if(!$p -or !$emc -or $p.Power.Version -ne 2 -or !$p.Power.CpuConfigured -or !$p.Power.CpuDfllAvailable -or $p.Power.CpuLastError -ne 0 -or
    !(Test-EmcBeforeNative $p.Power $emc.Emc (Device-Property 'ProblemCode'))){throw 'Platform handoff or fresh EMC readback is unavailable'}
 foreach($name in @('test-dvfs.exe','test-platform.exe','test-worker.exe')) {
  & (Join-Path $MergeRoot ('controls\'+$name))|Out-File (Join-Path $MergeRoot ($name+'.log'))
  if($LASTEXITCODE){throw ('Native CPU control failed: '+$name)}
 }
 $r.Phase='PreparingGpu';Save-Json $state $r
 $output=@(& (Join-Path $MergeRoot 'gpu\Prepare.ps1'));$line=@($output|Where-Object{$_ -like 'PREPARED_DIRECTORY=*'})
 if($line.Count -ne 1){throw 'Prepared GPU directory was not published'}
 $directory=$line[0].Substring('PREPARED_DIRECTORY='.Length)
 $gpu=Read-Json (Join-Path $directory 'trial-components.json')
 Sync-PackageFiles $directory $gpu.files 'trial-components.json'
 $r|Add-Member -NotePropertyName CandidateDirectory -NotePropertyValue $directory -Force;Save-Json $state $r
 & (Join-Path $directory 'Install-Default.ps1') -Prepared|Out-File (Join-Path $MergeRoot 'gpu-install.log')
 & (Join-Path $directory 'app\tgd3d-contracts.exe')|Out-File (Join-Path $MergeRoot 'umd-controls.log')
 if($LASTEXITCODE){throw 'Native ARM64 UMD CPU controls failed'}
 $r.Phase='Activating';Save-Json $state $r
 Start-ScheduledTask Tgd3dDefaultEnable
 $deadline=[DateTime]::UtcNow.AddSeconds(110);$activation=$null
 do {
  $file=Join-Path $directory 'default-state.json';if(Test-Path $file){try{$activation=Read-Json $file}catch{$activation=$null}}
  if($activation -and $activation.Status -in @('Enabled','Failed')){break}
  Start-Sleep -Milliseconds 500
 }while([DateTime]::UtcNow -lt $deadline)
 if(!$activation -or $activation.Status -ne 'Enabled' -or (Device-Property 'ProblemCode') -ne 0){throw 'Native activation did not complete'}
 $modules=@([Diagnostics.Process]::GetProcessesByName('dwm')|ForEach-Object{$process=$_;foreach($module in $process.Modules){if($module.ModuleName -in @('TegraGpuD3DVk.dll','libvulkan_nouveau.dll')){@{Pid=$process.Id;Name=$module.ModuleName;Path=$module.FileName;Sha256=File-Hash $module.FileName}}}})
 foreach($name in @('TegraGpuD3DVk.dll','libvulkan_nouveau.dll')){if(!($modules|Where-Object{$_.Name -eq $name -and $_.Path -eq (Join-Path $directory ('app\'+$name)) -and $_.Sha256 -eq $m.GpuApp.$name.Sha256})){throw ('DWM module differs: '+$name)}}
 # Preserve readiness separately from a policy-set failure; these are cached
 # status IOCTLs, not another clock transition or a new register scan.
 Save-Json (Join-Path $MergeRoot 'platform-before-policy.json') (& (Join-Path $MergeRoot 'Power-State.ps1'))
 Save-Json (Join-Path $MergeRoot 'emc-after-native.json') (& (Join-Path $MergeRoot 'Power-State.ps1') -EmcOnly)
 $p=& (Join-Path $MergeRoot 'Power-State.ps1') -Mode 1 -ChargeLimit 85
 if($p.Power.PowerMode -ne 1 -or !$p.Power.CpuDfllActive -or $p.Power.CpuLastError -ne 0 -or $p.Power.GpuPolicyMode -ne 1 -or $p.Battery.LimitCenter -ne 85 -or $p.Battery.BandMin -ne 80 -or $p.Battery.BandMax -ne 90 -or $p.Battery.LastApplyStatus -ne 0){throw 'Handheld/85% policy did not apply'}
 if(!(Get-ScheduledTask Tgd3dDefaultEnable).Settings.Enabled){throw 'Default login activation is disabled'}
 $r|Add-Member -NotePropertyName Modules -NotePropertyValue $modules -Force;$r|Add-Member -NotePropertyName End -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
 $r.Phase='NativeEnabled';Save-Json $state $r
 Save-Json (Join-Path $MergeRoot 'platform-after-native.json') $p
}catch{
 $r.Phase='Failed';$r|Add-Member -NotePropertyName Error -NotePropertyValue $_.Exception.Message -Force;Save-Json $state $r
 & (Join-Path $MergeRoot 'Restore-Dvfs.ps1')
 throw
}
