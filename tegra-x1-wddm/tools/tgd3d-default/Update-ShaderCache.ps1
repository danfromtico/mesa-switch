# SPDX-License-Identifier: MIT
# Stage a UMD fix or a paired UMD/ICD beside the active package for the next boot.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$CandidateDirectory,[switch]$Restart)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$payload=[IO.File]::ReadAllText((Join-Path $CandidateDirectory 'update.json'))|ConvertFrom-Json
$scope=if($payload.PSObject.Properties.Name -contains 'change_scope'){[string]$payload.change_scope}else{'shader-cache'}
if($scope -notin @('shader-cache','resource-teardown','copy-lifetime','modern-ddi','runtime-submit','video-ddi','present-ownership')){throw 'Unsupported update scope'}
$paired=$scope -in @('runtime-submit','present-ownership')
$video=$scope -in @('video-ddi','present-ownership')
if($scope -eq 'present-ownership' -and !$payload.gpu_prerotation){throw 'The presentation update requires the retained fixed landscape configuration'}
if($video -and (!$payload.preserve_display_power -or $payload.modern_ddi_advertised)){throw 'Expected a video update retaining the display and power configuration'}
if($scope -in @('modern-ddi','runtime-submit') -and !$payload.modern_ddi_advertised){throw 'Modern DDI update needs an explicit interface declaration'}
$settings='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault'
$previous=[string](Get-ItemProperty $settings).Directory
if($previous -ne $payload.expected_directory){throw 'Active package changed; inspect before updating'}
$userspace=if($payload.expected_userspace_directory){[string]$payload.expected_userspace_directory}else{$previous}
. (Join-Path $previous 'Common.ps1')
Require-Administrator
$m=Check-Package
$recovery=Check-Recovery
if($m.startup_mode -ne 'logon-default' -or $m.sync_mode -ne 'gpu-bridge' -or
   $payload.sync_mode -ne 'gpu-bridge' -or (!$video -and $payload.gpu_prerotation) -or !$payload.cache.enabled){throw 'Expected a UMD-only update of the logon GPU bridge package'}
if((Device-Property 'ProblemCode') -ne 0 -or
   (File-Hash "$env:windir\System32\drivers\tegragpurender.sys") -ne $payload.expected_kmd -or
   (File-Hash ($userspace+'\app\TegraGpuD3DVk.dll')) -ne $payload.expected_umd -or
   (File-Hash ($userspace+'\app\libvulkan_nouveau.dll')) -ne $payload.expected_icd){throw 'Running component baseline differs'}
Check-File ($CandidateDirectory+'\TegraGpuD3DVk.dll') $payload.candidate_umd
Check-File ($CandidateDirectory+'\tgd3d-contracts.exe') $payload.control
if($paired){Check-File ($CandidateDirectory+'\libvulkan_nouveau.dll') $payload.candidate_icd}
$assets=@()
if($video){
    $assets=@($payload.video_assets.PSObject.Properties)
    if((($assets.Name|Sort-Object) -join ',') -ne 'nvdec.bin,nvdec.bin.LICENSE'){throw 'Incomplete video firmware assets'}
    foreach($entry in $assets){Check-File (Join-Path $CandidateDirectory $entry.Name) $entry.Value}
    if($payload.video_assets.'nvdec.bin'.Length -ne 128000 -or
       $payload.video_assets.'nvdec.bin'.Sha256 -ne '7b5ac5ad66dad47e77f2a056991e422dd2630fa17e0a57a5874af492efa0f909'){throw 'Video firmware differs'}
}
$control=Read-Json ($CandidateDirectory+'\controls-state.json')
if($control.Phase -ne 'Passed' -or $control.Exit -ne 0 -or $control.Candidate -ne $payload.candidate_umd.Sha256 -or $control.GPUExecuted){throw 'Native CPU controls have not passed for this UMD'}
if($paired -and ($control.Icd -ne $payload.candidate_icd.Sha256 -or !$control.NvkControlsPassed)){throw 'Native controls have not passed for this paired ICD'}
$task=Get-ScheduledTask Tgd3dDefaultEnable
if($task.State -ne 'Ready'){throw 'Native startup task must be idle and enabled'}
$state=Join-Path $CandidateDirectory 'installed.json'
if(Test-Path $state){throw 'Update already staged; inspect installed.json instead of repeating it'}
$fileKeys=@{}
$updateNames=@('TegraGpuD3DVk.dll','tgd3d-contracts.exe')
if($paired){$updateNames+='libvulkan_nouveau.dll'}
foreach($name in $updateNames){
    # Earlier packages used forward slashes; merged platform packages use
    # Windows separators. Update the existing record, never add a second key.
    $entries=@($m.files.PSObject.Properties|Where-Object{$_.Name.Replace('\','/') -eq ('app/'+$name)})
    if($entries.Count -ne 1 -or !$m.app.PSObject.Properties[$name]){throw ('Missing or ambiguous UMD manifest entry: '+$name)}
    $fileKeys[$name]=$entries[0].Name
}
$dest=Join-Path (Split-Path $previous) ('tgd3d-'+$scope+'-'+$payload.candidate_umd.Sha256.Substring(0,8)+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
if(Test-Path $dest){throw 'Destination already exists'}
New-Item -ItemType Directory $dest|Out-Null
# Retain the existing package and its exact recovery files. No KMD install.
$names=@($m.files.PSObject.Properties.Name)+@($recovery.Files.PSObject.Properties.Name)+@('recovery.json')
foreach($name in ($names|Select-Object -Unique)){
    $source=Package-Path $name;$target=Join-Path $dest $name
    New-Item -ItemType Directory (Split-Path $target) -Force|Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}
Copy-Item ($CandidateDirectory+'\TegraGpuD3DVk.dll') ($dest+'\app\TegraGpuD3DVk.dll') -Force
Copy-Item ($CandidateDirectory+'\tgd3d-contracts.exe') ($dest+'\app\tgd3d-contracts.exe') -Force
if($paired){Copy-Item ($CandidateDirectory+'\libvulkan_nouveau.dll') ($dest+'\app\libvulkan_nouveau.dll') -Force}
Copy-Item ($PSScriptRoot+'\Restore-ShaderCache.ps1') ($dest+'\Restore-ShaderCache.ps1') -Force
Check-File ($dest+'\app\TegraGpuD3DVk.dll') $payload.candidate_umd
Check-File ($dest+'\app\tgd3d-contracts.exe') $payload.control
if($paired){Check-File ($dest+'\app\libvulkan_nouveau.dll') $payload.candidate_icd}
foreach($name in $updateNames){
    $file=Get-Item ($dest+'\app\'+$name);$record=@{Length=$file.Length;Sha256=(File-Hash $file.FullName)}
    $m.app.PSObject.Properties[$name].Value=$record
    $m.files.PSObject.Properties[$fileKeys[$name]].Value=$record
}
foreach($entry in $assets){
    Copy-Item -LiteralPath (Join-Path $CandidateDirectory $entry.Name) -Destination (Join-Path ($dest+'\app') $entry.Name)
    Check-File (Join-Path ($dest+'\app') $entry.Name) $entry.Value
    $m.app|Add-Member -NotePropertyName $entry.Name -NotePropertyValue $entry.Value -Force
    $m.files|Add-Member -NotePropertyName ('app\'+$entry.Name) -NotePropertyValue $entry.Value -Force
}
$restore=Get-Item ($dest+'\Restore-ShaderCache.ps1')
$m.files|Add-Member -NotePropertyName 'Restore-ShaderCache.ps1' -NotePropertyValue @{Length=$restore.Length;Sha256=(File-Hash $restore.FullName)} -Force
$m|Add-Member -NotePropertyName shader_cache -NotePropertyValue $payload.cache -Force
$m|Add-Member -NotePropertyName umd_frozen_base -NotePropertyValue $payload.frozen_base -Force
$m|Add-Member -NotePropertyName umd_patch_commits -NotePropertyValue $payload.patches -Force
$m|Add-Member -NotePropertyName umd_update_scope -NotePropertyValue $scope -Force
if($scope -in @('modern-ddi','runtime-submit')){$m|Add-Member -NotePropertyName modern_ddi_advertised -NotePropertyValue $true -Force}
if($video){
    if($payload.video_validation_environment){$m|Add-Member -NotePropertyName video_validation_environment -NotePropertyValue $payload.video_validation_environment -Force}
    else{$m.PSObject.Properties.Remove('video_validation_environment')}
}
$m.umd_source=$payload.frozen_base
if($paired){$m.icd_source=$payload.frozen_base;$m|Add-Member -NotePropertyName icd_source_identity -NotePropertyValue $payload.source_identity -Force}
if($payload.source_identity){$m|Add-Member -NotePropertyName umd_source_identity -NotePropertyValue $payload.source_identity -Force}
$m.scope=switch($scope){
    'resource-teardown' {'.158 retained KMD/ICD with tgd3d DestroyResource error-contract fix; logon start; native stability acceptance pending'}
    'copy-lifetime' {'.158 retained KMD/ICD with tgd3d overlap-copy, view bounds and lifetime fixes; modern DDI tables remain unadvertised; logon start; native stability acceptance pending'}
    'modern-ddi' {'.158 retained KMD/ICD with typed WDDM2.0/DXGI1.4 MPO1 and D3D10 pipeline; logon start; native interface and column scan acceptance pending'}
    'runtime-submit' {'.158 retained KMD with paired tgd3d/NVK runtime-owned command and fence transport; native runtime and column scan acceptance pending'}
    'video-ddi' {'D3D11.1 video and NV12 support on the retained NVDEC kernel, display and power configuration'}
    'present-ownership' {'Paired presentation and ownership fixes on the retained fixed landscape, power policy and NVDEC kernel'}
    default {'.158 existing KMD/ICD with cache-only tgd3d UMD; logon start; cache performance acceptance pending'}
}
Save-Json ($dest+'\trial-components.json') $m
$registration=Read-Json ($previous+'\icd-registration.json')
$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registration.Subkey,$true)
try{
    [string[]]$old=@($key.GetValue('UserModeDriverName'));$oldIcd=[string]$key.GetValue('VulkanDriverName')
    if($old.Count -ne 4 -or $old[1] -ne ($userspace+'\app\TegraGpuD3DVk.dll') -or $old[2] -ne $old[1] -or
       $oldIcd -ne $registration.Manifest){throw 'Active UMD/ICD registration differs'}
    Export-ScheduledTask Tgd3dDefaultEnable|Set-Content ($dest+'\cache-prior-task.xml') -Encoding Unicode
    Save-Json ($dest+'\cache-update-backup.json') @{PreviousDirectory=$previous;PreviousUserspaceDirectory=$userspace;Directory=$dest;Subkey=$registration.Subkey;UmdSlots=$old;Icd=$oldIcd;Kernel=$payload.expected_kmd;PreviousUmd=$payload.expected_umd;PreviousIcd=$payload.expected_icd;PairedIcdChanged=$paired;Utc=[DateTime]::UtcNow.ToString('o')}
    $registration.TemporaryUmd=$dest+'\app\TegraGpuD3DVk.dll';$registration.Manifest=$dest+'\app\nvk_windows_icd.json'
    Save-Json ($dest+'\icd-registration.json') $registration
    & icacls.exe $dest /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)(F)' '*S-1-5-32-544:(OI)(CI)(F)' '*S-1-5-32-545:(OI)(CI)(RX)' /Q|Out-Null
    if($LASTEXITCODE){throw 'Package ACL failed'}
    & (Join-Path $dest 'Enable-RuntimeAppContainerRead.ps1') -Directory ($dest+'\app')|Set-Content ($dest+'\runtime-access.json') -Encoding UTF8
    . (Join-Path $dest 'Common.ps1')
    $null=Check-Package;$null=Sync-Recovery
    # The next boot must not depend on data still held by the lazy writer.
    # Sync-Recovery covers only the recovery set, not the new UMD/package.
    $flushNames=@($m.files.PSObject.Properties.Name)+@('trial-components.json',
        'cache-prior-task.xml','cache-update-backup.json','icd-registration.json','runtime-access.json')
    foreach($name in ($flushNames|Select-Object -Unique)){
        $stream=[IO.FileStream]::new((Join-Path $dest $name),[IO.FileMode]::Open,
            [IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
        try{$stream.Flush($true)}finally{$stream.Dispose()}
    }
    # Changes are reversible using the retained registration/task snapshot.
    [string[]]$updated=$old.Clone();$updated[1]=$updated[2]=$registration.TemporaryUmd
    $key.SetValue('UserModeDriverName',$updated,[Microsoft.Win32.RegistryValueKind]::MultiString)
    $key.SetValue('VulkanDriverName',$registration.Manifest,[Microsoft.Win32.RegistryValueKind]::String);$key.Flush()
    Set-ItemProperty $settings Directory $dest
    $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$dest+'\Default-Start.ps1"')
    Set-ScheduledTask Tgd3dDefaultEnable -Action $action|Out-Null
    $icdHash=if($paired){$payload.candidate_icd.Sha256}else{$payload.expected_icd}
    $r=@{Utc=[DateTime]::UtcNow.ToString('o');Directory=$dest;PreviousDirectory=$previous;Phase='StagedForReboot';Umd=$payload.candidate_umd.Sha256;Kernel=$payload.expected_kmd;Icd=$icdHash;KernelChanged=$false;IcdChanged=$paired;GPUStartRequested=$false;OsRebootRequested=[bool]$Restart}
    Save-Json $state $r;$r|ConvertTo-Json
}catch{
    if(Test-Path ($dest+'\cache-update-backup.json')){& ($dest+'\Restore-ShaderCache.ps1')}
    throw
}finally{$key.Dispose()}
if($Restart){& shutdown.exe /r /t 10;if($LASTEXITCODE){throw 'Planned restart was not accepted'}}
