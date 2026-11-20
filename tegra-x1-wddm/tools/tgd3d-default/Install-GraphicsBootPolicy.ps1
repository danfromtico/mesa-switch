# SPDX-License-Identifier: MIT
# Update the protected policy snapshot without changing GPU registration or permission.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ManifestSha256,
      [string]$SourceDirectory=$PSScriptRoot,[switch]$RetireLegacy)
$ErrorActionPreference='Stop'
if($ManifestSha256 -notmatch '^[0-9a-f]{64}$'){throw 'Expected the SHA-256 from package_graphics_policy.py'}
$source=Join-Path $SourceDirectory 'graphics-policy-manifest.json'
if((Get-FileHash -LiteralPath $source).Hash -ne $ManifestSha256){throw 'Transferred policy manifest differs'}
$manifest=[IO.File]::ReadAllText($source)|ConvertFrom-Json
$names=@('Native-Boot.ps1','Baseline-Device.ps1','Baseline-Promotion.ps1','Install-GraphicsBootPolicy.ps1','Invoke-GraphicsBootPolicy.ps1','open-faults.json','graphics-baseline.json')
if($manifest.schema -ne 1 -or @($manifest.files.PSObject.Properties).Count -ne $names.Count){throw 'Invalid policy bundle schema'}
foreach($name in $names){
    if((Get-FileHash -LiteralPath (Join-Path $SourceDirectory $name)).Hash -ne $manifest.files.PSObject.Properties[$name].Value){throw "Policy bundle differs: $name"}
}
. (Join-Path $SourceDirectory 'Baseline-Device.ps1')
. (Join-Path $SourceDirectory 'Baseline-Promotion.ps1')
$ledger=Read-SwitchPolicyJson (Join-Path $SourceDirectory 'open-faults.json')
$baseline=Read-SwitchPolicyJson (Join-Path $SourceDirectory 'graphics-baseline.json')
Assert-SwitchFaultLedger $ledger
Assert-SwitchAcceptedBaseline $baseline
$root=Join-Path $env:ProgramFiles 'SwitchWOA-GpuBoot'
$mutex=[Threading.Mutex]::new($false,'Global\SwitchWOA-GraphicsBootPolicy')
$locked=$false
try{
    try{$locked=$mutex.WaitOne([TimeSpan]::FromSeconds(30))}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(!$locked){throw 'Another graphics policy operation is running'}
    $store=Join-Path $root 'policies'
    $null=[IO.Directory]::CreateDirectory($store)
    $pointer=Join-Path $root 'active-policy.txt'
    $previous=$null
    if([IO.File]::Exists($pointer)){
        $previous=Get-SwitchPolicySnapshot $root
        $priorLedger=Read-SwitchPolicyJson (Get-SwitchPolicyFile $previous 'open-faults.json')
        Assert-SwitchFaultHistory $priorLedger $ledger
    }
    $accepted=Join-Path $root 'last-accepted.json'
    if(![IO.File]::Exists($accepted)){
        if($previous){throw 'Previously accepted device receipt is missing'}
        $svc='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender'
        if(!(Test-Path "$svc\AllowNativeBoot")){
            Write-SwitchPolicyText $accepted '{"schema":1,"none_accepted":true}'
        }else{
        $perm=Get-ItemProperty "$svc\AllowNativeBoot" -ErrorAction Stop
        $dir=(Get-ItemProperty "$svc\Tgd3dDefault" -ErrorAction Stop).Directory
        if($dir -ne $baseline.directory -or $perm.ApprovedDirectory -ne $dir){throw 'The recorded baseline is not already permanently approved'}
        $live=Get-SwitchInstalledProfile $dir @($baseline.components.PSObject.Properties.Name)
        if(!(Test-SwitchSameProfile $baseline.components $live)){throw 'Bootstrap component hashes differ'}
        $tuple=$live.'tegragpurender.sys'+'/'+$live.'TegraGpuD3DVk.dll'+'/'+$live.'libvulkan_nouveau.dll'
        if($tuple -ne $perm.ApprovedImages -or (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_ProblemCode).Data -ne 0){throw 'Existing approved native desktop could not be confirmed'}
        $dw=@(Get-Process dwm|Where-Object SessionId -gt 0)
        if($dw.Count -ne 1){throw 'A unique baseline desktop is required for bootstrap'}
        $mods=@(Get-Process -Id $dw[0].Id -Module -ErrorAction Stop)
        foreach($name in @('TegraGpuD3DVk.dll','libvulkan_nouveau.dll','tg_d3d_arm64.dll','tg_nvk_arm64.dll')){
            $file=Join-Path (Join-Path $dir 'app') $name
            if(@($mods|Where-Object FileName -ieq $file).Count -ne 1){throw "Baseline module not loaded from its accepted directory: $name"}
        }
        Write-SwitchPolicyText $accepted ($baseline|ConvertTo-Json -Depth 6)
        }
    }
    if([IO.File]::Exists((Join-Path $root 'pending-approval.json'))){
        $service=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\tegragpurender')
        if(!$service){throw 'Cannot reconcile the previous GPU approval receipt'}
        try{Complete-SwitchGrantReceipt $root $service}finally{$service.Dispose()}
    }
    $known=Read-SwitchPolicyJson $accepted
    if(!($known.schema -eq 1 -and $known.none_accepted -is [bool] -and $known.none_accepted -and !$known.components)){
        Assert-SwitchAcceptedBaseline $known
    }
    $target=Join-Path $store $ManifestSha256.ToLowerInvariant()
    if(![IO.Directory]::Exists($target)){
        $pending=Join-Path $store ([guid]::NewGuid().ToString('N')+'.tmp')
        $null=[IO.Directory]::CreateDirectory($pending)
        try{
            [IO.File]::Copy($source,(Join-Path $pending 'manifest.json'))
            foreach($name in $names){[IO.File]::Copy((Join-Path $SourceDirectory $name),(Join-Path $pending $name))}
            foreach($name in $names){if((Get-FileHash -LiteralPath (Join-Path $pending $name)).Hash -ne $manifest.files.PSObject.Properties[$name].Value){throw 'Staged policy data changed'}}
            [IO.Directory]::Move($pending,$target)
        }finally{if([IO.Directory]::Exists($pending)){[IO.Directory]::Delete($pending,$true)}}
    }
    foreach($name in $names){if((Get-FileHash -LiteralPath (Join-Path $target $name)).Hash -ne $manifest.files.PSObject.Properties[$name].Value){throw 'Installed immutable snapshot differs'}}
    if((Get-FileHash -LiteralPath (Join-Path $target 'manifest.json')).Hash -ne $ManifestSha256){throw 'Installed snapshot manifest differs'}
    $entry=Join-Path $root 'Native-Boot.ps1'
    # The loader resolves the immutable executable through the single active pointer.
    if(![IO.File]::Exists($entry) -or (Get-FileHash -LiteralPath $entry).Hash -ne $manifest.files.'Invoke-GraphicsBootPolicy.ps1'){
        $temporary=$entry+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
        $backup=$temporary+'.bak'
        try{
            [IO.File]::Copy((Join-Path $target 'Invoke-GraphicsBootPolicy.ps1'),$temporary)
            if((Get-FileHash -LiteralPath $temporary).Hash -ne $manifest.files.'Invoke-GraphicsBootPolicy.ps1'){throw 'Staged boot policy loader differs'}
            if([IO.File]::Exists($entry)){[IO.File]::Replace($temporary,$entry,$backup)}else{[IO.File]::Move($temporary,$entry)}
        }finally{
            if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}
            if([IO.File]::Exists($backup)){[IO.File]::Delete($backup)}
        }
    }
    Write-SwitchPolicyText $pointer ($ManifestSha256.ToLowerInvariant())
    if($RetireLegacy){
        $legacy=Join-Path $root 'Native-Boot-0eef4f5b33e4.ps1'
        if([IO.File]::Exists($legacy)){
            $knownHash='0eef4f5b33e4ed5a1feeceb4bc2ed25305e99741bc42c87cfad4f4ef3f9f7bed'
            if((Get-FileHash -LiteralPath $legacy).Hash -ne $knownHash){throw 'Unexpected legacy boot policy bytes'}
            $grant=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowNativeBoot' -ErrorAction Stop
            if($grant.ApprovalPolicy -ne $ManifestSha256 -or $grant.ApprovedDirectory -ne $known.directory -or !(Test-SwitchSameProfile ($grant.ApprovedComponents|ConvertFrom-Json) $known.components)){throw 'Exercise guarded restoration of the accepted baseline before retiring the old entry'}
            $tasks=@(Get-ScheduledTask|Where-Object {@($_.Actions|Where-Object {$_.Arguments -match 'Native-Boot-0eef4f5b33e4\.ps1'}).Count})
            if($tasks.Count){throw 'A registered task still uses the old boot policy'}
            $archive=Join-Path $root 'retired-policy-source'
            $null=[IO.Directory]::CreateDirectory($archive)
            $targetFile=Join-Path $archive ($knownHash+'.txt')
            if([IO.File]::Exists($targetFile)){throw 'Unexpected pre-existing retired policy source'}
            [IO.File]::Move($legacy,$targetFile)
        }
    }
    [pscustomobject]@{entry=$entry;revision=$ManifestSha256.ToLowerInvariant();open_faults=@($ledger.faults|Where-Object status -ceq open).Count;gpu_permission_modified=$false}
}finally{if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
