# SPDX-License-Identifier: MIT
# Select one verified .158 kernel image while native startup is closed.
param([ValidateSet('Candidate','Baseline')][string]$Direction='Candidate')
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$m=Read-Json (Join-Path $PSScriptRoot 'trial-components.json')
if($m.baseline_kmd.'tegragpurender.sys'.Sha256 -ne (Kernel-BaselineHash $m)) {
    throw 'Expected the reviewed .158 bit14 package and frozen recovery image'
}
if($Direction -eq 'Candidate'){$m=Check-Package;$records=$m.kmd;$folder='package'}
else {$records=$m.baseline_kmd;$folder='baseline-kmd'}
foreach($entry in $records.PSObject.Properties){Check-File (Join-Path $PSScriptRoot ($folder+'\'+$entry.Name)) $entry.Value}
$systemFile=Join-Path $env:windir 'System32\drivers\tegragpurender.sys'
$current=File-Hash $systemFile;$expected=$records.'tegragpurender.sys'.Sha256
if($current -eq $expected){return}
$allowed=@($m.kmd.'tegragpurender.sys'.Sha256,$m.baseline_kmd.'tegragpurender.sys'.Sha256)
if($m.kmd_variant -eq 'shader-output-bit14-native-boot'){$allowed+='2e4ae8081687537f777d72636ff1fab0640559215a703f6ccfab2127f95f0572'}
if((Device-Property 'DriverVersion') -ne '0.1.0.158' -or (Device-Property 'Service') -ne 'tegragpurender' -or
   $current -notin $allowed) {
    throw 'Installed kernel differs from both reviewed .158 images'
}
$params=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
if((Device-Property 'ProblemCode') -ne 43 -or [uint64]$params.StartGateStatus -ne 3221225635 -or
   [uint64]$params.HardwareStatus -ne 4294967295) {
    throw 'Kernel selection requires the closed .158 startup gate; do not hot-switch an active GPU'
}
if(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowDesktopStart'){throw 'A GPU startup grant is armed'}
if(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowNativeBoot'){throw 'Persistent native startup is armed'}
$permit=Join-Path $env:windir 'System32\drivers\TgGpuBootTest158.bin'
if(Test-Path $permit){
    $bytes=[IO.File]::ReadAllBytes($permit)
    if($bytes.Length -ne 4096 -or [BitConverter]::ToUInt32($bytes,0) -ne 0){throw 'A GPU startup permit is armed'}
    Remove-Item $permit
}
$null=Sync-Recovery
if(Get-ScheduledTask Tgd3dDefaultEnable -ErrorAction SilentlyContinue){Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null}
$request=Join-Path $PSScriptRoot ('kmd-'+$Direction.ToLowerInvariant()+'-request.json')
if(Test-Path $request){throw 'This kernel switch was already requested; inspect its result before retrying'}
$inf=Join-Path $PSScriptRoot ($folder+'\tegragpurender.inf')
$r=@{Begin=[DateTime]::UtcNow.ToString('o');Direction=$Direction;BeforeHash=$current;TargetHash=$expected;BeforeOem=(Device-Property 'DriverInfPath');OsRebootRequested=$false}
Save-Json $request $r
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Tgd3dKernelSelection {
    [DllImport("newdev.dll", EntryPoint="UpdateDriverForPlugAndPlayDevicesW", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool Update(IntPtr parent, string hardwareId, string inf, uint flags, out bool rebootRequired);
}
'@
$reboot=$false
# FORCE supports restoring the older INF date; NONINTERACTIVE is required
# for the SYSTEM controller. Supplying the reboot pointer prevents OS prompts.
$ok=[Tgd3dKernelSelection]::Update([IntPtr]::Zero,'ACPI\SWOA0F01',$inf,5,[ref]$reboot)
$errorCode=if($ok){0}else{[Runtime.InteropServices.Marshal]::GetLastWin32Error()}
$r.End=[DateTime]::UtcNow.ToString('o');$r.Success=$ok;$r.ErrorCode=$errorCode;$r.RebootRequired=$reboot
$r.AfterHash=File-Hash $systemFile;$r.AfterOem=Device-Property 'DriverInfPath';$r.ProblemCode=Device-Property 'ProblemCode'
Save-Json (Join-Path $PSScriptRoot ('kmd-'+$Direction.ToLowerInvariant()+'-result.json')) $r
if(!$ok){throw ("Kernel selection failed: Win32 "+$errorCode)}
if($reboot){throw 'Kernel selection requires a Windows restart; no restart was requested'}
if($r.AfterHash -ne $expected -or $r.ProblemCode -ne 43){throw 'Selected kernel or closed-gate state differs'}
$r|ConvertTo-Json
