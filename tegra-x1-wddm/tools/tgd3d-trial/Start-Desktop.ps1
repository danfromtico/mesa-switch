# SPDX-License-Identifier: MIT
# Adapted from the existing .158/.161 GPU trial workflow; see tool-provenance.json.
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new()
$id='ACPI\SWOA0F01\0'
$dir=$PSScriptRoot
$evidence=Join-Path $dir 'evidence'
$permitFile='C:\Windows\System32\drivers\TgGpuBootTest158.bin'
$svcPath='SYSTEM\CurrentControlSet\Services\tegragpurender'
$observe='Tgd3dTrialObserve'
$marker=Join-Path $dir 'demand-started.json'
function Prop($key) { (Get-PnpDeviceProperty -InstanceId $id -KeyName "DEVPKEY_Device_$key").Data }
function Hash($file) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
function Write-StartEvidence($path, [byte[]]$data) {
    $file=[IO.FileStream]::new($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try {
        $file.Write($data,0,$data.Length)
        $file.Flush($true)
    } finally { $file.Dispose() }
}
function Read-StartPermit {
    $file=[IO.FileStream]::new($permitFile,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $bytes=[byte[]]::new(4)
        if ($file.Read($bytes,0,4) -ne 4) { throw 'Cannot read the remaining permit' }
        [BitConverter]::ToUInt32($bytes,0)
    } finally { $file.Dispose() }
}

if ((Prop 'DriverVersion') -ne '0.1.0.158' -or (Prop 'Service') -ne 'tegragpurender' -or
    (Prop 'ProblemCode') -ne 43) { throw 'Expected the gated .158 display device' }
if ((Hash 'C:\Windows\System32\drivers\tegragpurender.sys') -ne '46D1757674FF156028CE5B729BA53671981FFAB09948FCAF125751C46BB10088') {
    throw 'Installed test driver differs'
}
if ((Hash 'C:\Windows\System32\acpitabl.dat') -ne '515D8D9C1612514CA2F197D6C96774A9265785A7E519921A195F0F8D257AA500') {
    throw 'Expected the test ACPI table'
}
if ((Get-ScheduledTask -TaskName $observe).State -ne 'Ready') {
    throw 'Expected the ready desktop probe'
}
$boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
$armed=Get-Content -Raw -LiteralPath (Join-Path $dir 'before-restart.json') | ConvertFrom-Json
$initial=Get-Content -Raw -LiteralPath (Join-Path $evidence 'observed-interactive.json') | ConvertFrom-Json
$display=@(Get-Content -LiteralPath (Join-Path $evidence 'desktop-path.log') | ForEach-Object { $_ | ConvertFrom-Json })
$prepared=[DateTime]::Parse($armed.Utc).ToUniversalTime()
$reuseBoot=$armed.RebootRequired -eq $false
$expectedBoot=[DateTime]::Parse($armed.Boot).ToUniversalTime()
$earliestObservation=if($reuseBoot){$prepared}else{$boot}
if ((!$reuseBoot -and $boot -le $prepared) -or ($reuseBoot -and $boot -ne $expectedBoot) -or
    [DateTime]::Parse($initial.Utc).ToUniversalTime() -le $earliestObservation -or
    $initial.Session -eq 0 -or $initial.Version -ne '0.1.0.158' -or $initial.Problem -ne 43 -or
    $display.Count -lt 2 -or !$display[0].active_path -or $display[1].matching_kmd -ne 0) {
    throw 'Expected a fresh BasicDisplay observation on the prepared boot'
}
$parameters=Get-ItemProperty -LiteralPath ("HKLM:\$svcPath\Parameters")
if ([uint64]$parameters.StartGateStatus -ne 3221225635 -or $parameters.InitializeStatus -ne 0 -or
    [uint64]$parameters.HardwareStatus -ne 4294967295) { throw 'The hardware startup gate was not closed' }
if (Test-Path -LiteralPath ("HKLM:\$svcPath\AllowDesktopStart")) { throw 'The desktop grant already exists' }

# Reserve one invocation before making a permit available to the driver.
$started=@{Utc=[DateTime]::UtcNow.ToString('o');Boot=$boot.ToString('o');Session=$initial.Session} |
    ConvertTo-Json -Compress
Write-StartEvidence $marker ([Text.Encoding]::UTF8.GetBytes($started))
foreach ($name in 'observed-interactive.json','desktop-path.log','desktop-exit.json') {
    Write-StartEvidence (Join-Path $dir ('before-'+$name)) ([IO.File]::ReadAllBytes((Join-Path $evidence $name)))
}

# Native identity needs the small UMD session. Kernel trace collection and
# its boot-only autologger/flush helper are not prerequisites for rendering.
$umdConsumer=& (Join-Path $dir 'Start-NativeUmdTrace.ps1') -Directory $dir -TraceName 'Tgd3dTrialUmd' -CreateSession
$svc=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($svcPath,$true)
if (!$svc) { throw 'Display service key is unavailable' }
$grant=$false
try {
    $file=[IO.FileStream]::new($permitFile,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try {
        $bytes=[byte[]]::new(4)
        if ($file.Length -ne 4096 -or $file.Read($bytes,0,4) -ne 4 -or [BitConverter]::ToUInt32($bytes,0) -ne 0) {
            throw 'Expected the unarmed startup record'
        }
        $bytes[0]=1
        $file.Position=0
        $file.Write($bytes,0,4)
        $file.Flush($true)
    } finally { $file.Dispose() }
    $key=$svc.CreateSubKey('AllowDesktopStart',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[Microsoft.Win32.RegistryOptions]::Volatile)
    $grant=$true
    $key.Close()
    $requestQpc=[Diagnostics.Stopwatch]::GetTimestamp()
    $restartRequested=[DateTime]::UtcNow.ToString('o')
    $request=@{Utc=$restartRequested;Qpc=$requestQpc;Frequency=[Diagnostics.Stopwatch]::Frequency}|ConvertTo-Json -Compress
    Write-StartEvidence (Join-Path $dir 'native-request.json') ([Text.Encoding]::UTF8.GetBytes($request))
    Write-StartEvidence (Join-Path $dir 'restart-requested.txt') ([Text.Encoding]::UTF8.GetBytes($restartRequested))
    $umdProcess=Get-Process -Id $umdConsumer.Pid
    if($umdProcess.StartTime.ToUniversalTime() -ne [DateTime]::Parse($umdConsumer.StartTime).ToUniversalTime()){throw 'UMD trace consumer changed before native startup'}
    & pnputil.exe /restart-device $id | Tee-Object -FilePath (Join-Path $dir 'demand-pnp.log')
    $restartExit=$LASTEXITCODE
    $restartReturned=[DateTime]::UtcNow.ToString('o')
    # PnP can return while Dxgkrnl still has StartDevice queued.
    if ($restartExit -eq 0) {
        $wait=[Diagnostics.Stopwatch]::StartNew()
        while ((Read-StartPermit) -eq 1 -and $wait.ElapsedMilliseconds -lt 20000) {
            Start-Sleep -Milliseconds 200
        }
    }
} finally {
    try {
        if ($grant) {
            $grantReleased=[DateTime]::UtcNow.ToString('o')
            $svc.DeleteSubKey('AllowDesktopStart',$false)
        }
    }
    finally { $svc.Close() }
}

$remaining=Read-StartPermit
$params=Get-ItemProperty -LiteralPath ("HKLM:\$svcPath\Parameters")
$result=@{Utc=[DateTime]::UtcNow.ToString('o');Exit=$restartExit;Permit=$remaining;Version=(Prop 'DriverVersion');
  Problem=(Prop 'ProblemCode');Hardware=$params.HardwareStatus;Panel=$params.PanelStatus;
  StartGate=$params.StartGateStatus;BootPermit=$params.BootPermitStatus;
  RestartRequested=$restartRequested;RestartReturned=$restartReturned;GrantReleased=$grantReleased}
$json=$result|ConvertTo-Json -Compress
Write-StartEvidence (Join-Path $dir 'demand-result.json') ([Text.Encoding]::UTF8.GetBytes($json))
$json

Start-ScheduledTask -TaskName $observe
