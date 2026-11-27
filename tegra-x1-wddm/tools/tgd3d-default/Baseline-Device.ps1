# SPDX-License-Identifier: MIT
# The installed on-device policy is an immutable snapshot selected under Program Files.
function Get-SwitchPolicySnapshot([string]$Root) {
    $revision=[IO.File]::ReadAllText((Join-Path $Root 'active-policy.txt')).Trim()
    if ($revision -notmatch '^[0-9a-f]{64}$') { throw 'Invalid active promotion policy revision' }
    $folder=Join-Path (Join-Path $Root 'policies') $revision
    $path=Join-Path $folder 'manifest.json'
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $revision) { throw 'Promotion policy manifest changed' }
    $manifest=[IO.File]::ReadAllText($path)|ConvertFrom-Json -ErrorAction Stop
    if ($manifest.schema -ne 1) { throw 'Unknown promotion policy bundle' }
    return [pscustomobject]@{Directory=$folder;Files=$manifest.files;Revision=$revision}
}

function Get-SwitchPolicyFile($Snapshot,[string]$Name) {
    $entry=$Snapshot.Files.PSObject.Properties[$Name]
    if (!$entry -or $entry.Value -notmatch '^[0-9a-f]{64}$') { throw "Unregistered policy file: $Name" }
    $path=Join-Path $Snapshot.Directory $Name
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.Value) { throw "Policy file differs: $Name" }
    return $path
}

function Get-SwitchInstalledProfile([string]$Directory,[string[]]$Names,[switch]$AllowMissing) {
    $result=[ordered]@{}
    foreach ($name in $Names) {
        if ($name -cne 'nv_acr_ucode_prod.bin' -and $name -notmatch '^[a-z0-9_-]+\.(dll|sys)$') { throw "Invalid component name: $name" }
        $file=if ($name -eq 'tegragpurender.sys') { Join-Path $env:windir 'System32/drivers/tegragpurender.sys' }
              elseif ($name -ceq 'nv_acr_ucode_prod.bin') { Join-Path $env:windir 'System32/drivers/TegraGpu/nv_acr_ucode_prod.bin' }
              else { Join-Path (Join-Path $Directory 'app') $name }
        if ($AllowMissing -and ![IO.File]::Exists($file)) { continue }
        $result[$name]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return [pscustomobject]$result
}

function Write-SwitchPolicyText([string]$Path,[string]$Text) {
    $temp=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    $backup=$temp+'.bak'
    try {
        $bytes=[Text.UTF8Encoding]::new($false).GetBytes($Text)
        $stream=[IO.FileStream]::new($temp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None,4096,[IO.FileOptions]::WriteThrough)
        try { $stream.Write($bytes,0,$bytes.Length);$stream.Flush($true) } finally { $stream.Dispose() }
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temp,$Path,$backup) } else { [IO.File]::Move($temp,$Path) }
    } finally {
        if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) }
        if ([IO.File]::Exists($backup)) { [IO.File]::Delete($backup) }
    }
}

function Save-SwitchTrialBookmark([string]$PolicyRoot,[string]$Directory,$Profile) {
    $newest=Get-WinEvent -LogName System -MaxEvents 1 -ErrorAction Stop
    $boot=Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Microsoft-Windows-Kernel-General';Id=12} -MaxEvents 1 -ErrorAction Stop
    $r=[ordered]@{schema=1;directory=$Directory;components=$Profile;first_record=$newest.RecordId;prior_boot=$boot.RecordId}
    Write-SwitchPolicyText (Join-Path $PolicyRoot 'active-trial.json') ($r|ConvertTo-Json -Depth 5)
}

function Get-SwitchRollbackPath([string]$PolicyRoot,[string]$Directory) {
    $bytes=[Text.Encoding]::UTF8.GetBytes($Directory.TrimEnd([char]92).ToUpperInvariant())
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$key=[BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
    return Join-Path (Join-Path $PolicyRoot 'accepted-history') ($key+'.json')
}

function Save-SwitchRollbackProfile([string]$PolicyRoot,$Baseline) {
    Assert-SwitchAcceptedBaseline $Baseline
    $path=Get-SwitchRollbackPath $PolicyRoot $Baseline.directory
    $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    if([IO.File]::Exists($path)){
        $previous=Read-SwitchPolicyJson $path
        if($previous.directory -ne $Baseline.directory -or !(Test-SwitchSameProfile $previous.components $Baseline.components)){throw 'Accepted recovery directory was reused with different bytes'}
        return
    }
    Write-SwitchPolicyText $path ($Baseline|ConvertTo-Json -Depth 6)
}

function Get-SwitchReactivationEvidence($Selected,$Qualification,$Bookmark,$Events,$LastRecord) {
    $proved=$Qualification.Data
    Assert-SwitchAcceptedBaseline $Selected
    Assert-SwitchHash $Qualification.Sha256
    if ($Selected.source_commit -ne $proved.source_commit -or $Selected.qualification_sha256 -ne $Qualification.Sha256 -or
        $Selected.directory -ne $proved.directory -or !(Test-SwitchSameProfile $Selected.components $proved.components)) {
        throw 'A later activation requires the exact repository-selected hardware qualification'
    }
    if (!(Test-SwitchRecordId $proved.system_log.last_record) -or !(Test-SwitchRecordId $Bookmark.first_record) -or $Bookmark.first_record -le $proved.system_log.last_record -or
        !(Test-SwitchRecordId $LastRecord) -or $LastRecord -le $Bookmark.first_record) { throw 'Invalid interval for later activation' }
    $boots=@($Events|Where-Object { $_.Id -eq 12 -and $_.ProviderName -eq 'Microsoft-Windows-Kernel-General' })
    $shutdown=@($Events|Where-Object { $_.Id -eq 1074 -and $_.ProviderName -eq 'User32' })
    if ($boots.Count -ne 1 -or $shutdown.Count -ne 1 -or $shutdown[0].RecordId -le $Bookmark.first_record -or
        $shutdown[0].RecordId -ge $boots[0].RecordId -or $boots[0].RecordId -gt $LastRecord -or
        @($Events|Where-Object { ($_.Id -eq 41 -and $_.ProviderName -eq 'Microsoft-Windows-Kernel-Power') -or
                                 ($_.Id -eq 6008 -and $_.ProviderName -eq 'EventLog') }).Count) {
        throw 'Later activation needs one controlled restart without an abnormal shutdown'
    }
    $boot=$boots[0].RecordId
    return [pscustomobject]@{trial=[pscustomobject]@{boot_record=$boot};system_log=[pscustomobject]@{
        first_record=$Bookmark.first_record;last_record=$LastRecord;boot_records=@($boot);abnormal_events=@();
        restarts=@([pscustomobject]@{kind='controlled';boot_record=$boot;system_records=@($shutdown[0].RecordId,$boot)})}}
}

function Assert-SwitchLiveTrial([string]$PolicyRoot,[string]$Directory,$Profile,$Evidence,[string]$Token) {
    $armed=Read-SwitchPolicyJson (Join-Path $PolicyRoot 'active-trial.json')
    if ($armed.directory -ne $Directory -or !(Test-SwitchSameProfile $armed.components $Profile) -or $armed.first_record -ne $Evidence.system_log.first_record) { throw 'Hardware evidence does not start at this candidate grant' }
    $svc='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender'
    if ((Test-Path "$svc\AllowNativeBoot") -or (Get-ItemProperty "$svc\AllowDesktopStart" -ErrorAction Stop).PolicyMode -ne 'Trial') { throw 'Expected the one-time policy' }
    $permit=[IO.File]::ReadAllBytes($Token)
    if ($permit.Length -ne 4096 -or [BitConverter]::ToUInt32($permit,0) -ne 0) { throw 'One-time permission was not consumed' }
    Add-Type -AssemblyName System.Windows.Forms
    if ([Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus -ne 'Online') { throw 'The qualification requires external power' }
    $gpu=(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_ProblemCode -ErrorAction Stop).Data
    if ($gpu -ne 0) { throw 'The candidate desktop is not Code 0' }
    $dwm=@(Get-Process dwm -ErrorAction Stop|Where-Object SessionId -gt 0)
    if ($dwm.Count -ne 1) { throw 'A unique interactive desktop is required' }
    $modules=@(Get-Process -Id $dwm[0].Id -Module -ErrorAction Stop)
    foreach ($name in @('TegraGpuD3DVk.dll','libvulkan_nouveau.dll','tg_d3d_arm64.dll','tg_nvk_arm64.dll')) {
        $path=Join-Path (Join-Path $Directory 'app') $name
        if (@($modules|Where-Object FileName -ieq $path).Count -ne 1 -or (Get-FileHash -LiteralPath $path).Hash -ne $Profile.PSObject.Properties[$name].Value) { throw "DWM did not load the qualified component: $name" }
    }
    $oldest=Get-WinEvent -LogName System -Oldest -MaxEvents 1 -ErrorAction Stop
    $newest=Get-WinEvent -LogName System -MaxEvents 1 -ErrorAction Stop
    if ($oldest.RecordId -gt $armed.first_record -or $newest.RecordId -lt $Evidence.system_log.last_record) { throw 'System event log does not cover the qualification' }
    $query="*[System[EventRecordID > $([long]$armed.first_record) and (EventID=12 or EventID=41 or EventID=6008 or EventID=1074)]]"
    $events=@(Get-WinEvent -LogName System -FilterXPath $query -ErrorAction Stop)
    $boots=@($events|Where-Object {$_.Id -eq 12 -and $_.ProviderName -eq 'Microsoft-Windows-Kernel-General'}|Sort-Object RecordId)
    if (!$boots.Count -or $boots[-1].RecordId -ne $Evidence.trial.boot_record -or $boots[-1].RecordId -eq $armed.prior_boot) { throw 'Current boot differs from the tested one-time boot' }
    if($boots.Count -ne $Evidence.system_log.boot_records.Count -or (Compare-Object @($boots.RecordId|ForEach-Object {[long]$_}) @($Evidence.system_log.boot_records|ForEach-Object {[long]$_}))){throw 'Boot inventory differs from the actual Windows interval'}
    $previous=[long]$armed.first_record
    foreach ($boot in $boots) {
        $explanation=@($Evidence.system_log.restarts|Where-Object boot_record -eq $boot.RecordId)
        if ($explanation.Count -ne 1 -or $boot.RecordId -notin $explanation[0].system_records) { throw "Unexplained boot record $($boot.RecordId)" }
        if ($explanation[0].kind -ceq 'controlled') {
            $intent=@($events|Where-Object {$_.Id -eq 1074 -and $_.ProviderName -eq 'User32' -and $_.RecordId -gt $previous -and $_.RecordId -lt $boot.RecordId -and $_.RecordId -in $explanation[0].system_records})
            if (!$intent.Count) { throw 'Controlled restart has no matching Windows shutdown intent' }
        }
        $previous=$boot.RecordId
    }
    $abnormal=@($events|Where-Object {($_.Id -eq 41 -and $_.ProviderName -eq 'Microsoft-Windows-Kernel-Power') -or ($_.Id -eq 6008 -and $_.ProviderName -eq 'EventLog')})
    if($abnormal.Count -ne $Evidence.system_log.abnormal_events.Count){throw 'Abnormal-shutdown inventory differs from Windows'}
    foreach ($event in $abnormal) {
        $owner=@($boots|Where-Object RecordId -le $event.RecordId|Select-Object -Last 1)
        $match=@($Evidence.system_log.restarts|Where-Object {$owner.Count -and $_.boot_record -eq $owner[0].RecordId -and $_.kind -ceq 'diagnosed' -and $event.RecordId -in $_.system_records})
        $declared=@($Evidence.system_log.abnormal_events|Where-Object {$owner.Count -and $_.boot_record -eq $owner[0].RecordId -and $_.record -eq $event.RecordId -and $_.id -eq $event.Id})
        if ($match.Count -ne 1 -or $declared.Count -ne 1 -or $event.RecordId -gt $Evidence.system_log.last_record) { throw "Unexplained abnormal shutdown event $($event.RecordId)" }
    }
}

function Initialize-SwitchGrantRename {
    if('SwitchGraphicsPolicy.RegistryCommit' -as [type]){return}
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace SwitchGraphicsPolicy {
    public static class RegistryCommit {
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        public static extern int RegRenameKey(SafeRegistryHandle key, string oldName, string newName);
    }
}
'@
}

function Complete-SwitchGrantReceipt([string]$Root,$Service) {
    $path=Join-Path $Root 'pending-approval.json'
    if(![IO.File]::Exists($path)){return}
    $pending=Read-SwitchPolicyJson $path
    Assert-SwitchAcceptedBaseline $pending.approved
    if($pending.transaction -notmatch '^[a-f0-9]{32}$'){throw 'Invalid pending grant receipt'}
    $gate=$Service.OpenSubKey('AllowNativeBoot')
    try{
        if($gate -and $gate.GetValue('ApprovalTransaction') -eq $pending.transaction){
            $components=$gate.GetValue('ApprovedComponents')|ConvertFrom-Json -ErrorAction Stop
            if($gate.GetValue('ApprovedDirectory') -ne $pending.approved.directory -or !(Test-SwitchSameProfile $components $pending.approved.components)){throw 'Committed grant and prepared evidence disagree'}
            Write-SwitchPolicyText (Join-Path $Root 'last-accepted.json') ($pending.approved|ConvertTo-Json -Depth 8)
        }
    }finally{if($gate){$gate.Dispose()}}
    [IO.File]::Delete($path)
}

function Publish-SwitchPreparedGrant($Service,[string]$Root,$Approved,[string]$Images,[string]$Revision) {
    $transaction=[guid]::NewGuid().ToString('N')
    $record=[pscustomobject]@{transaction=$transaction;approved=$Approved}
    Write-SwitchPolicyText (Join-Path $Root 'pending-approval.json') ($record|ConvertTo-Json -Depth 9)
    $stageName='PreparedSwitchNativeBoot'
    $Service.DeleteSubKey($stageName,$false)
    $gate=$Service.CreateSubKey($stageName)
    try{
        $gate.SetValue('ApprovedImages',$Images)
        $gate.SetValue('ApprovedComponents',($Approved.components|ConvertTo-Json -Compress))
        $gate.SetValue('ApprovalPolicy',$Revision)
        $gate.SetValue('ApprovalTransaction',$transaction)
        $gate.SetValue('ApprovedDirectory',$Approved.directory)
        $gate.Flush()
    }finally{$gate.Dispose()}
    $errorCode=[SwitchGraphicsPolicy.RegistryCommit]::RegRenameKey($Service.Handle,$stageName,'AllowNativeBoot')
    if($errorCode){throw [ComponentModel.Win32Exception]::new($errorCode)}
    $Service.Flush()
}
