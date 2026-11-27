# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$ExpectedLuid
)

$ErrorActionPreference='Stop'
$p=[IO.Path]::GetFullPath($Directory)
if(Test-Path -LiteralPath $OutputPath){throw 'Native identity record already exists'}
$session=(Get-Process -Id $PID).SessionId
$r=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Session=$session;Verified=$false;PhysicalPanelVerified=$false}
try {
    if(!$session){throw 'Run the identity check on the interactive desktop'}
    $id='ACPI\SWOA0F01\0'
    $r.Version=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
    $r.Problem=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
    $m=[IO.File]::ReadAllText((Join-Path $p 'trial-components.json'))|ConvertFrom-Json
    if($r.Version -ne $m.kmd_version -or $r.Problem -ne 0){throw 'The identified native KMD is not running with Code 0'}
    $helper=Join-Path $p 'desktop-path.exe'
    if((Get-FileHash $helper).Hash -ne $m.helpers.'desktop-path.exe'.Sha256){throw 'Display path helper differs from the trial manifest'}
    # The query helper requires the frozen KMT bridge DLL as argv[1], as in
    # the interactive observer. The actual renderer is app\TegraGpuD3DVk.dll.
    $bridge=Join-Path $p 'tegragpuno3d.dll'
    if((Get-FileHash $bridge).Hash -ne $m.kmd.'tegragpuno3d.dll'.Sha256){throw 'Display query bridge differs from the trial manifest'}
    $lines=@(& $helper $bridge)
    $r.HelperExitCode=$LASTEXITCODE
    $r.HelperOutput=$lines
    $rows=@($lines|ForEach-Object {$_|ConvertFrom-Json})
    $r.Paths=$rows
    # Frozen helper 6a10be74 prints its read-only DisplayConfig/KMT records,
    # then exits 4 because the no3d bridge deliberately has no
    # TgGpuReadD3DAdapterProbe export. This occurs before D3D11CreateDevice.
    # Actual DWM device creation/presentation is checked separately via ETW.
    if($r.HelperExitCode -ne 4 -or $rows.Count -ne 2 -or $rows[1].kmt_status -ne '00000000'){throw 'Active display path query failed'}
    $r.Luid=$rows[1].kmt_luid
    if(!$rows[0].active_path -or !$rows[1].matching_kmd -or !$rows[1].nvk_ready -or
        $rows[0].source_luid -ne $r.Luid -or $rows[0].target_luid -ne $r.Luid -or
        $r.Luid -eq '0000000000000000' -or ($ExpectedLuid -and $ExpectedLuid -ne $r.Luid)) {
        throw 'The active desktop is not on the expected native GM20B adapter'
    }
    $dwm=@(Get-Process dwm | Where-Object {$_.SessionId -eq $session})
    if($dwm.Count -ne 1){throw 'Expected one DWM process in the interactive session'}
    $r.DwmPid=$dwm[0].Id
    $r.DwmStart=$dwm[0].StartTime.ToUniversalTime().ToString('o')
    # tgd3d loads its paired ICD directly; there is no Vulkan loader selection log.
    foreach($name in 'TegraGpuD3DVk.dll','libvulkan_nouveau.dll') {
        if((Get-FileHash (Join-Path $p ('app\'+$name))).Hash -ne $m.app.$name.Sha256) { throw 'Runtime file differs' }
    }
    $subkey='SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_Driver).Data
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subkey)
    try {
        $names=@($key.GetValue('UserModeDriverName'))
        if($names.Count -ne 4 -or $names[1] -ne (Join-Path $p 'app\TegraGpuD3DVk.dll') -or $names[2] -ne $names[1] -or
           $key.GetValue('VulkanDriverName') -ne (Join-Path $p 'app\nvk_windows_icd.json')) { throw 'Trial UMD/ICD registration differs' }
    } finally { if($key) { $key.Dispose() } }
    $consumer=[IO.File]::ReadAllText((Join-Path $p 'umd-live-process.json'))|ConvertFrom-Json
    $process=Get-Process -Id $consumer.Pid
    # The observer runs at the interactive user's normal integrity level;
    # reading a SYSTEM consumer's module path would require extra privileges.
    # Its PID and creation time identify the process started by the controller.
    if($process.StartTime.ToUniversalTime() -ne [DateTime]::Parse($consumer.StartTime).ToUniversalTime() -or
        $process.ProcessName -ne [IO.Path]::GetFileNameWithoutExtension($consumer.Exe)){throw 'The current trial UMD consumer is no longer running'}
    $request=[IO.File]::ReadAllText((Join-Path $p 'native-request.json'))|ConvertFrom-Json
    # Both submission modes may be idle. This check establishes the current
    # process/adapter identity; an eventless desktop is not a GPU failure.
    # Use the same predicate for both sides of a synchronization comparison.
    $r.Umd=& (Join-Path $p 'Read-NativeUmdEvidence.ps1') -Path (Join-Path $p 'umd-live.jsonl') -DwmProcessId $r.DwmPid -DwmStart ([DateTime]::Parse($r.DwmStart)) -TrialQpc $request.Qpc -Frequency $request.Frequency -TraceName $consumer.TraceName -ExpectedLuid $r.Luid -AllowIdle
    $r.Verified=$true
} catch {
    $r.Error=$_.Exception.Message
    throw
} finally {
    $r.End=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($OutputPath,($r|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
}
