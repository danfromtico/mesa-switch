# SPDX-License-Identifier: MIT
# Revoke the accepted driver's permanent grant BEFORE replacing GPU components.
[CmdletBinding()]
param([ValidateSet('Trial','Accept','Restore','Disarm')][string]$Mode='Trial',
      [string]$Directory=$PSScriptRoot,
      [string]$VerifiedKmd,[string]$VerifiedUmd,[string]$VerifiedIcd,
      [string]$Qualification,[string]$LedgerSha256)
$ErrorActionPreference='Stop'
if($Mode -ne 'Disarm'){$Directory=[IO.Path]::GetFullPath($Directory).TrimEnd([char]92,[char]47)}
$policyRoot=Join-Path $env:ProgramFiles 'SwitchWOA-GpuBoot'
$persistent=$Mode -in @('Accept','Restore')
$mutex=[Threading.Mutex]::new($false,'Global\SwitchWOA-GraphicsBootPolicy')
$locked=$false
$svc=$null
try {
try { $locked=$mutex.WaitOne([TimeSpan]::FromSeconds(30)) } catch [Threading.AbandonedMutexException] { $locked=$true }
if(!$locked){throw 'Another graphics boot policy operation is running'}
$path='SYSTEM\CurrentControlSet\Services\tegragpurender'
$svc=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path,$true)
if(!$svc){throw 'Tegra GPU service unavailable'}
$permit=Join-Path $env:windir 'System32\drivers\TgGpuBootTest158.bin'
function Close-NativeGrants {
    $svc.DeleteSubKey('AllowNativeBoot',$false)
    $svc.DeleteSubKey('AllowDesktopStart',$false)
    $svc.Flush()
    if([IO.File]::Exists($permit)){[IO.File]::Delete($permit)}
}
try {
    $policy=$null
    $approval=$Mode
    $pointer=Join-Path $policyRoot 'active-policy.txt'
    $recoverBeforeDisarm=$Mode -eq 'Disarm' -and [IO.File]::Exists($pointer) -and [IO.File]::Exists((Join-Path $policyRoot 'pending-approval.json'))
    if(($Mode -ne 'Disarm' -and ($persistent -or [IO.File]::Exists($pointer))) -or $recoverBeforeDisarm){
        try{
        $revision=[IO.File]::ReadAllText($pointer).Trim()
        if($revision -notmatch '^[0-9a-f]{64}$'){throw 'Invalid installed promotion policy revision'}
        $folder=Join-Path (Join-Path $policyRoot 'policies') $revision
        $manifestPath=Join-Path $folder 'manifest.json'
        if((Get-FileHash -LiteralPath $manifestPath).Hash -ne $revision){throw 'Installed promotion policy manifest changed'}
        $manifest=[IO.File]::ReadAllText($manifestPath)|ConvertFrom-Json -ErrorAction Stop
        if($manifest.schema -ne 1 -or $manifest.files.'Native-Boot.ps1' -ne (Get-FileHash -LiteralPath $PSCommandPath).Hash){throw 'Use the boot policy registered for this device'}
        $deviceFile=Join-Path $folder 'Baseline-Device.ps1'
        if((Get-FileHash -LiteralPath $deviceFile).Hash -ne $manifest.files.'Baseline-Device.ps1'){throw 'Installed policy reader changed'}
        . $deviceFile
        $policy=Get-SwitchPolicySnapshot $policyRoot
        . (Get-SwitchPolicyFile $policy 'Baseline-Promotion.ps1')
        Complete-SwitchGrantReceipt $policyRoot $svc
        }catch{
            if(!$recoverBeforeDisarm){throw}
            Write-Warning ('Could not recover the previous approval receipt before revocation: '+$_.Exception.Message)
        }
    }
    if($Mode -ne 'Disarm'){
        $selected=(Get-ItemProperty ('HKLM:\'+$path+'\Tgd3dDefault')).Directory
        if(!$selected -or [IO.Path]::GetFullPath($selected).TrimEnd([char]92,[char]47) -ne $Directory){throw 'Selected graphics package changed'}
        $m=[IO.File]::ReadAllText((Join-Path $Directory 'trial-components.json'))|ConvertFrom-Json
        $reg=[IO.File]::ReadAllText((Join-Path $Directory 'icd-registration.json'))|ConvertFrom-Json
        $class='SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_Driver).Data
        if($reg.Subkey -ne $class){throw 'Adapter registration changed'}
        $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($class)
        try{$slots=@($key.GetValue('UserModeDriverName'));$icd=$key.GetValue('VulkanDriverName')}finally{$key.Close()}
        if($m.kmd_version -ne '0.1.0.158' -or $slots.Count -ne 4 -or $slots[1] -ne $reg.TemporaryUmd -or $slots[2] -ne $slots[1] -or $icd -ne $reg.Manifest){throw 'Active package differs'}
        $k=(Get-FileHash "$env:windir\System32\drivers\tegragpurender.sys").Hash
        $u=(Get-FileHash $slots[1]).Hash
        $i=(Get-FileHash (Join-Path (Split-Path $slots[1]) 'libvulkan_nouveau.dll')).Hash
        if($k -ne $m.kmd.'tegragpurender.sys'.Sha256 -or $u -ne $m.app.'TegraGpuD3DVk.dll'.Sha256 -or $i -ne $m.app.'libvulkan_nouveau.dll'.Sha256 -or (Get-FileHash (Join-Path (Split-Path $icd) 'libvulkan_nouveau.dll')).Hash -ne $i){throw 'Installed files differ from the selected package'}
        $acrName='nv_acr_ucode_prod.bin'
        $acrExpected=$m.kmd.$acrName.Sha256
        if($acrExpected){
            $acrFile=Join-Path $env:windir 'System32/drivers/TegraGpu/nv_acr_ucode_prod.bin'
            if((Get-FileHash -LiteralPath $acrFile).Hash -ne $acrExpected){throw 'Installed ACR differs from the selected package'}
        }
        if($Mode -eq 'Accept' -and ($k -ne $VerifiedKmd -or $u -ne $VerifiedUmd -or $i -ne $VerifiedIcd)){throw 'Accepted baseline hashes differ'}
        if($persistent){
            $acceptedReceipt=Join-Path $policyRoot 'last-accepted.json'
            $approved=Read-SwitchPolicyJson $acceptedReceipt
            if($approved.schema -eq 1 -and $approved.none_accepted -is [bool] -and $approved.none_accepted -and !$approved.components){$approved=$null}
            else{Assert-SwitchAcceptedBaseline $approved}
            $previousApproved=$approved
            if($Mode -eq 'Restore' -and (!$approved -or $approved.directory -ne $Directory)){
                $approved=Read-SwitchPolicyJson (Get-SwitchRollbackPath $policyRoot $Directory)
                Assert-SwitchAcceptedBaseline $approved
            }
            if($slots[1] -ne (Join-Path (Join-Path $Directory 'app') 'TegraGpuD3DVk.dll') -or $icd -ne (Join-Path (Join-Path $Directory 'app') 'nvk_windows_icd.json')){throw 'Registration points outside the selected graphics package'}
            if($approved -and $approved.directory -eq $Directory){
                $live=Get-SwitchInstalledProfile $Directory @($approved.components.PSObject.Properties.Name)
                if(!(Test-SwitchSameProfile $approved.components $live)){throw 'Previously accepted graphics package bytes changed'}
                if($Mode -eq 'Restore' -or [string]::IsNullOrWhiteSpace($Qualification)){$approval='RestoreAccepted'}
            }
            if($approval -ne 'RestoreAccepted'){
                if($Mode -eq 'Restore'){throw 'Recovery is restricted to the exact accepted package'}
                $ledger=Read-SwitchPolicyJson (Get-SwitchPolicyFile $policy 'open-faults.json')
                Assert-SwitchFaultLedger $ledger
                if ($LedgerSha256 -ne $policy.Files.'open-faults.json') { throw 'Sync the current repository open-fault ledger before new promotion' }
                $e=Read-SwitchEvidence $Qualification
                Assert-SwitchProfile $e.Data.components
                $live=Get-SwitchInstalledProfile $Directory @($e.Data.components.PSObject.Properties.Name)
                $priorFile=Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Qualification))) 'previous-qualification.json'
                Assert-SwitchPromotion $ledger $e.Data $e.Sha256 $live $Directory $previousApproved $priorFile
                Assert-SwitchChangedComponentLoads $previousApproved.components $live $e.Data
                Assert-SwitchCaseFiles $e.Data $Qualification
                $activation=$e.Data
                $mark=Read-SwitchPolicyJson (Join-Path $policyRoot 'active-trial.json')
                if ($mark.first_record -ne $e.Data.system_log.first_record) {
                    $selectedProfile=Read-SwitchPolicyJson (Get-SwitchPolicyFile $policy 'graphics-baseline.json')
                    $last=(Get-WinEvent -LogName System -MaxEvents 1 -ErrorAction Stop).RecordId
                    if (!(Test-SwitchRecordId $mark.first_record) -or !(Test-SwitchRecordId $last)) { throw 'Missing live activation bookmark' }
                    $filter="*[System[EventRecordID > $([long]$mark.first_record) and EventRecordID <= $([long]$last) and (EventID=12 or EventID=41 or EventID=6008 or EventID=1074)]]"
                    $events=@(Get-WinEvent -LogName System -FilterXPath $filter -ErrorAction Stop)
                    $activation=Get-SwitchReactivationEvidence $selectedProfile $e $mark $events $last
                }
                Assert-SwitchLiveTrial $policyRoot $Directory $live $activation $permit
                $approval='QualifiedPromotion'
            }
        }elseif($Mode -eq 'Trial' -and $policy){
            $names=@('tegragpurender.sys','TegraGpuD3DVk.dll','libvulkan_nouveau.dll','tg_d3d_arm64.dll','tg_d3d_x64.dll','tg_nvk_arm64.dll','tg_nvk_x64.dll')
            $names+=@($m.app.PSObject.Properties.Name|Where-Object {$_ -match '^[a-z0-9_-]+\.dll$' -and $_ -notin $names})
            $selectedBaseline=Read-SwitchPolicyJson (Get-SwitchPolicyFile $policy 'graphics-baseline.json')
            $acceptedAcr=$selectedBaseline.components.PSObject.Properties[$acrName]
            $baseFirmware=Join-Path (Join-Path $selectedBaseline.directory 'package') $acrName
            if($acceptedAcr -or ($acrExpected -and (![IO.File]::Exists($baseFirmware) -or (Get-FileHash -LiteralPath $baseFirmware).Hash -ne $acrExpected))){
                if(!$acrExpected){throw 'Missing ACR package hash for tracked firmware'}
                $names+=$acrName
            }
            $trialProfile=Get-SwitchInstalledProfile $Directory $names -AllowMissing
        }
    }
    if($persistent){
        Initialize-SwitchGrantRename
        if($previousApproved -and $previousApproved.directory -ne $Directory){Save-SwitchRollbackProfile $policyRoot $previousApproved}
        if($approval -eq 'QualifiedPromotion'){
            $nextAccepted=[pscustomobject]@{schema=1;source_commit=$e.Data.source_commit;directory=$Directory;components=$live;qualification=$e.Sha256;qualification_sha256=$e.Sha256;boot_record=$activation.trial.boot_record;qualification_boot_record=$e.Data.trial.boot_record}
        }else{$nextAccepted=$approved}
    }
    $renew=Get-ScheduledTask SwitchGpuRenewVerifiedBoot -ErrorAction SilentlyContinue
    if($renew){
        if($renew.Actions.Count -ne 1 -or $renew.Actions[0].Arguments -notlike ('*'+$env:ProgramFiles+'\SwitchWOA-GpuBoot\Renew-*.ps1"* -Mode Renew *')){throw 'Unexpected renewal task owner'}
        Disable-ScheduledTask SwitchGpuRenewVerifiedBoot|Out-Null
        if($renew.State -eq 'Running'){Stop-ScheduledTask SwitchGpuRenewVerifiedBoot}
        Unregister-ScheduledTask SwitchGpuRenewVerifiedBoot -Confirm:$false
    }
    $delayed=Get-ScheduledTask Tgd3dDefaultEnable -ErrorAction SilentlyContinue
    if($delayed){
        if($delayed.State -eq 'Running'){throw 'Desktop activation in progress'}
        Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null
    }
    Close-NativeGrants
    if($Mode -eq 'Disarm'){return}
    try{
        if($persistent){
            # Only the fully populated, renamed key authorizes a boot.
            Publish-SwitchPreparedGrant $svc $policyRoot $nextAccepted "$k/$u/$i" $policy.Revision
        }else{
            if($policy){Save-SwitchTrialBookmark $policyRoot $Directory $trialProfile}
            $data=[byte[]]::new(4096);$data[0]=1
            $f=[IO.FileStream]::new($permit,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,
                [IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
            try{$f.Write($data,0,$data.Length);$f.Flush($true)}finally{$f.Dispose()}
            $gate=$svc.CreateSubKey('AllowDesktopStart')
            try{$gate.SetValue('PolicyMode','Trial');$gate.Flush()}finally{$gate.Close()}
        }
        $svc.Flush()
    }catch{Close-NativeGrants;throw}
    if($persistent){Complete-SwitchGrantReceipt $policyRoot $svc}
    [ordered]@{Mode=$Mode;Approval=$approval;Directory=$Directory;Images="$k/$u/$i";PersistentNativeBoot=$persistent;OneUsePermit=($Mode -eq 'Trial')}|ConvertTo-Json -Compress
}finally{$svc.Close()}
}finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
