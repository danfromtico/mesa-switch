# SPDX-License-Identifier: MIT
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$script:TrialRoot=$PSScriptRoot
$script:DeviceId='ACPI\SWOA0F01\0'
function Require-Administrator {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    if(!([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this entry point from an elevated administrator shell'
    }
}
function Read-Json([string]$Path) { [IO.File]::ReadAllText($Path)|ConvertFrom-Json }
function Save-Json([string]$Path,$Value) {
    $bytes=[Text.Encoding]::UTF8.GetBytes(($Value|ConvertTo-Json -Depth 8))
    $file=[IO.FileStream]::new($Path,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try { $file.Write($bytes,0,$bytes.Length);$file.Flush($true) } finally { $file.Dispose() }
}
function File-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Stop-RecordedTrialProcess($Record) {
    $process=Get-Process -Id $Record.Pid -ErrorAction SilentlyContinue
    if(!$process){return}
    try {
        # Retain the process object while checking identity; a recycled PID
        # must never select an unrelated process for termination.
        $null=$process.Handle
        if($process.StartTime.ToUniversalTime() -eq [DateTime]::Parse($Record.StartTime).ToUniversalTime() -and
           $process.ProcessName -eq [IO.Path]::GetFileNameWithoutExtension($Record.Exe)) {
            $process.Kill()
            if(!$process.WaitForExit(5000)){throw 'Owned collector did not exit'}
        }
    } catch {
        # ETW stop can make the consumer exit between lookup and Kill.
        if(!$process.HasExited){throw}
    } finally { $process.Dispose() }
}
function Device-Property([string]$Name) {
    (Get-PnpDeviceProperty -InstanceId $script:DeviceId -KeyName ('DEVPKEY_Device_'+$Name)).Data
}
function Package-Path([string]$Name) {
    $path=[IO.Path]::GetFullPath((Join-Path $script:TrialRoot $Name))
    if(!$path.StartsWith($script:TrialRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes the trial directory' }
    $path
}
function Check-File([string]$Path,$Expected) {
    $file=Get-Item -LiteralPath $Path
    if($file.Length -ne $Expected.Length -or (File-Hash $Path) -ne $Expected.Sha256) { throw ('File mismatch: '+$Path) }
}
function Kernel-BaselineHash($Manifest) {
    switch($Manifest.kmd_variant) {
        'reviewed-dvfs-emc' {'e95287cbeb1d2c325baacb9685affd1d24783b22ecadc1801ba1dffcfe7cc25e';return}
        'shader-output-bit14' {'46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088';return}
        'shader-output-bit14-native-boot' {'46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088';return}
        'shader-output-bit14-gpu460' {'c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53';return}
        'shader-output-bit14-gpu460-vidpn' {
            $hash=[string]$Manifest.baseline_kmd.'tegragpurender.sys'.Sha256
            if($hash -in @('59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81',
                          '69aa6036da81870efb439e8e09530ae662e810dccf887b603bdfeb6ba8a4a11a',
                          '1a86a02b479b716b2b695ade0bd6860076d4d5f61a29d95b804cb655ce0bf164')){$hash;return}
            throw 'Unexpected reviewed .158 display baseline'
        }
        default {throw 'Unexpected kernel variant'}
    }
}
function Check-Package {
    $m=Read-Json (Join-Path $script:TrialRoot 'trial-components.json')
    if($m.kmd_version -ne '0.1.0.158' -or $m.umd_name -ne 'TegraGpuD3DVk.dll') { throw 'Unexpected trial component identity' }
    if($m.kmd_variant -and $m.baseline_kmd.'tegragpurender.sys'.Sha256 -ne (Kernel-BaselineHash $m)) { throw 'Unexpected kernel recovery baseline' }
    if($m.kmd_variant -in @('shader-output-bit14-gpu460','shader-output-bit14-gpu460-vidpn') -and
       ($m.startup_mode -notin @('logon-default','native-boot') -or $m.kmd_gpu_khz -ne 460800)) { throw 'Clock control requires the reviewed 460.8 MHz package' }
    if($m.kmd_variant -eq 'reviewed-dvfs-emc' -and
       ($m.startup_mode -ne 'logon-default' -or $m.sync_mode -ne 'gpu-bridge' -or
        $m.platform_fan_version -ne 33 -or !$m.kmd -or
        ((@($m.baseline_kmd.PSObject.Properties.Name|Sort-Object) -join ',') -ne (@($m.kmd.PSObject.Properties.Name|Sort-Object) -join ',')))){throw 'EMC requires the matched Fan33/ACTMON-IRQ/GPU-bridge logon package'}
    foreach($entry in $m.files.PSObject.Properties) { Check-File (Package-Path $entry.Name) $entry.Value }
    if(!$m.helpers -or !$m.helpers.PSObject.Properties['display158.aml']) { throw 'Missing packaged ACPI table identity' }
    Check-File (Package-Path 'display158.aml') $m.helpers.'display158.aml'
    if($m.kmd_variant) {
        if(!$m.baseline_kmd -or !$m.kmd) { throw 'Missing kernel package manifest' }
        foreach($req in @('tegragpurender.inf','tegragpurender.cat','tegragpurender.sys','tegragpuno3d.dll')) {
            if(!$m.baseline_kmd.PSObject.Properties[$req] -or !$m.kmd.PSObject.Properties[$req]) { throw ('Missing kernel package member: '+$req) }
        }
        foreach($entry in $m.baseline_kmd.PSObject.Properties) { Check-File (Package-Path ('baseline-kmd/'+$entry.Name)) $entry.Value }
        foreach($entry in $m.kmd.PSObject.Properties) { Check-File (Package-Path ('package/'+$entry.Name)) $entry.Value }
    }
    foreach($file in Get-ChildItem -LiteralPath $script:TrialRoot -Filter '*.ps1') {
        $tokens=$null;$errors=$null
        $null=[System.Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)
        if($errors.Count) { throw ('PowerShell parse failed: '+$file.Name) }
    }
    $m
}
function Check-Baseline {
    $version=Device-Property 'DriverVersion';$problem=Device-Property 'ProblemCode'
    if($version -eq '0.1.0.158' -and $problem -eq 43) {
        # Expected closed startup gate: BasicDisplay remains usable while the
        # retained .158 miniport is installed but native GPU work is disabled.
        $hashes=@('46D1757674FF156028CE5B729BA53671981FFAB09948FCAF125751C46BB10088')
        $manifest=Read-Json (Join-Path $script:TrialRoot 'trial-components.json')
        if($manifest.kmd_variant) {
            $expected=Kernel-BaselineHash $manifest
            if($manifest.baseline_kmd.'tegragpurender.sys'.Sha256 -ne $expected){throw 'Kernel recovery baseline differs'}
            $hashes=@($expected)
            $hashes+=([string]$manifest.kmd.'tegragpurender.sys'.Sha256)
            if($manifest.kmd_variant -eq 'shader-output-bit14-native-boot'){$hashes+='2e4ae8081687537f777d72636ff1fab0640559215a703f6ccfab2127f95f0572'}
        }
        if((Device-Property 'Service') -ne 'tegragpurender' -or
           (File-Hash "$env:windir\System32\drivers\tegragpurender.sys") -notin $hashes -or
           (File-Hash "$env:windir\System32\acpitabl.dat") -ne $manifest.helpers.'display158.aml'.Sha256) { throw 'Retained .158 baseline differs' }
    } elseif($version -eq '0.1.0.59' -and $problem -eq 0) {
        # One-time migration from the previous baseline; recovery never
        # automatically reinstalls this version.
        if((File-Hash "$env:windir\System32\drivers\tegragpuprobe.sys") -ne 'DE5E9CD492D35A73EA3D3F5F1723FD82F88922F6E91EF5966B430360F5EB5646' -or
           (File-Hash "$env:windir\System32\acpitabl.dat") -ne 'B829B50944A90745FB6F6F7F44F453A6B8573FE4019A95FEFD0013A983703518') { throw 'Initial .59 baseline differs' }
    } else { throw 'Expected the gated .158 baseline or initial .59 Code 0' }
}
function Check-Recovery {
    $backup=Read-Json (Join-Path $script:TrialRoot 'recovery.json')
    if($backup.InstanceId -ne $script:DeviceId -or $backup.Version -notin @('0.1.0.59','0.1.0.158')) { throw 'Recovery identity differs' }
    foreach($entry in $backup.Files.PSObject.Properties) { Check-File (Package-Path $entry.Name) $entry.Value }
    $inf=Package-Path $backup.Inf
    $name=if($backup.Version -eq '0.1.0.158'){'tegragpurender'}else{'tegragpuprobe'}
    if([IO.Path]::GetFileName($inf) -ne ($name+'.inf')) { throw 'Recovery INF differs' }
    if(!(Test-Path (Join-Path (Split-Path $inf) ($name+'.cat')))) { throw 'Recovery catalogue missing' }
    $backup
}
function Sync-Recovery {
    $backup=Check-Recovery
    # Hashing a newly copied file can read cached bytes. Flush the complete
    # recovery set before a GPU switch can interrupt the system's lazy writer.
    foreach($name in @($backup.Files.PSObject.Properties.Name)+@('recovery.json')) {
        $file=[IO.FileStream]::new((Package-Path $name),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
        try { $file.Flush($true) } finally { $file.Dispose() }
    }
    $backup
}
