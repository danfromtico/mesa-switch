# SPDX-License-Identifier: MIT
param([switch]$Retry)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$m=Check-Package
$stateFile=Join-Path $PSScriptRoot 'default-state.json'
$service='SYSTEM\CurrentControlSet\Services\tegragpurender'
$bootKey='HKLM:\'+$service+'\Tgd3dDefaultBoot'
if((Test-Path $bootKey) -and !$Retry){return}
if((Test-Path $stateFile) -and !$Retry){
    try { $last=Read-Json $stateFile } catch {
        # An interrupted write can leave a zero-filled JSON file. Disable
        # the task before returning the parse error, without granting GPU start.
        Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null
        throw
    }
    if(!$last -or $last.Status -ne 'Enabled'){
        # An interrupted native activation must not turn the next boot into
        # another blind attempt. The frozen .158 remains behind its gate.
        Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null
        'Previous activation failed/interrupted; use RESTORE.cmd or an explicit retry.'
        return
    }
}
$svc=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($service,$true)
$grant=$false
$r=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Status='Starting';SyncMode=$m.sync_mode;Version=$m.kmd_version;PhysicalPanelVerified=$false;OsRebootRequested=$false}
function Save-State {
    $bytes=[Text.Encoding]::UTF8.GetBytes(($r|ConvertTo-Json -Depth 3))
    $file=[IO.FileStream]::new($stateFile,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try{$file.Write($bytes,0,$bytes.Length);$file.Flush($true)}finally{$file.Dispose()}
}
try{
    if($m.startup_mode -ne 'logon-default' -or (Device-Property 'DriverVersion') -ne '0.1.0.158'){throw 'Expected the default .158 package'}
    Check-File "$env:windir\System32\drivers\tegragpurender.sys" $m.kmd.'tegragpurender.sys'
    Check-File "$env:windir\System32\acpitabl.dat" $m.helpers.'display158.aml'
    $registration=Read-Json (Join-Path $PSScriptRoot 'icd-registration.json')
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registration.Subkey)
    try{
        $names=@($key.GetValue('UserModeDriverName'))
        if($names.Count -ne 4 -or $names[1] -ne $registration.TemporaryUmd -or $names[2] -ne $names[1] -or $key.GetValue('VulkanDriverName') -ne $registration.Manifest){throw 'Default UMD/ICD registration changed'}
    }finally{$key.Dispose()}
    $boot=$svc.CreateSubKey('Tgd3dDefaultBoot',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[Microsoft.Win32.RegistryOptions]::Volatile)
    $boot.Close()
    if((Device-Property 'ProblemCode') -eq 0){$r.Status='Enabled';Save-State;return}
    Check-Baseline
    $null=Sync-Recovery
    $params=Get-ItemProperty ('HKLM:\'+$service+'\Parameters')
    if([uint64]$params.StartGateStatus -ne 3221225635 -or [uint64]$params.HardwareStatus -ne 4294967295){throw 'The native startup gate is not closed'}
    Save-State
    $data=[byte[]]::new(4096);$data[0]=1
    $permit="$env:windir\System32\drivers\TgGpuBootTest158.bin"
    $file=[IO.FileStream]::new($permit,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try{$file.Write($data,0,$data.Length);$file.Flush($true)}finally{$file.Dispose()}
    $key=$svc.CreateSubKey('AllowDesktopStart',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[Microsoft.Win32.RegistryOptions]::Volatile)
    $key.Close();$grant=$true
    & pnputil.exe /restart-device $script:DeviceId
    if($LASTEXITCODE -ne 0){throw 'Native device activation failed'}
    $limit=[DateTime]::UtcNow.AddSeconds(20)
    do{
        $params=Get-ItemProperty ('HKLM:\'+$service+'\Parameters')
        if($params.HardwareStatus -eq 0 -and (Device-Property 'ProblemCode') -eq 0){break}
        Start-Sleep -Milliseconds 250
    }while([DateTime]::UtcNow -lt $limit)
    if($params.HardwareStatus -ne 0 -or $params.PanelStatus -ne 0 -or (Device-Property 'ProblemCode') -ne 0){throw 'Native .158 did not activate'}
    $svc.DeleteSubKey('AllowDesktopStart',$false);$grant=$false
    # Passive startup observation only: no test windows or input injection.
    Start-Sleep -Seconds 30
    if((Device-Property 'ProblemCode') -ne 0){throw 'Native device left Code 0 during startup'}
    $r.Status='Enabled';$r.End=[DateTime]::UtcNow.ToString('o');Save-State
}catch{
    $r.Status='Failed';$r.Error=$_.Exception.Message;Save-State
    Disable-ScheduledTask Tgd3dDefaultEnable -ErrorAction SilentlyContinue|Out-Null
    throw
}finally{
    if($grant){$svc.DeleteSubKey('AllowDesktopStart',$false)}
    $svc.Dispose()
}
