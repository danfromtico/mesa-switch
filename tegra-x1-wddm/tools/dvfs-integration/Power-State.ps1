# SPDX-License-Identifier: MIT
param([int]$Mode=-1,[int]$ChargeLimit=-1,[switch]$PowerOnly,[switch]$EmcOnly)
$ErrorActionPreference='Stop'
if(!('SwitchPowerIo' -as [type])) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SwitchPowerIo {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr CreateFile(string name,uint access,uint share,IntPtr sa,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)] public static extern bool DeviceIoControl(IntPtr h,uint code,byte[] input,uint inputSize,[Out] byte[] output,uint outputSize,out uint bytes,IntPtr overlapped);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
'@
}
function Power-Request([string]$Path,[uint32]$Code,[byte[]]$InputBytes,[int]$OutputBytes) {
 $h=[SwitchPowerIo]::CreateFile($Path,3221225472,3,[IntPtr]::Zero,3,0,[IntPtr]::Zero)
 if($h -eq [IntPtr](-1)){throw ('Open '+$Path+': '+[Runtime.InteropServices.Marshal]::GetLastWin32Error())}
 try {
  $out=[byte[]]::new($OutputBytes);[uint32]$bytes=0
  if(![SwitchPowerIo]::DeviceIoControl($h,$Code,$InputBytes,$InputBytes.Length,$out,$OutputBytes,[ref]$bytes,[IntPtr]::Zero)){throw ('IOCTL '+$Code+': '+[Runtime.InteropServices.Marshal]::GetLastWin32Error())}
  if($bytes -ne $OutputBytes){throw 'Power ABI result length differs'}
  return ,$out
 }finally{[void][SwitchPowerIo]::CloseHandle($h)}
}
if($EmcOnly){
 if($Mode -ge 0 -or $ChargeLimit -ge 0){throw 'EMC status is read-only; do not combine it with policy changes'}
 # SWFAN_EMC_STATUS v1, paired with drivers/SwitchFan/src/emc_ioctl.h.
 $data=Power-Request '\\.\SwitchBacklight' 0x00226054 @() 188
 $names=@('Version','Flags','ActualKhz','BootKhz','SampleAgeMs','LastError','EnumerationError','RateCount')
 $emc=[ordered]@{}
 for($i=0;$i -lt $names.Count;$i++){
  $emc[$names[$i]]=if($i -in @(5,6)){[BitConverter]::ToInt32($data,4*$i)}else{[BitConverter]::ToUInt32($data,4*$i)}
 }
 if($emc.Version -ne 1 -or $emc.RateCount -gt 32){throw 'Unexpected EMC status version or rate count'}
 $emc.RatesKhz=@(for($i=0;$i -lt $emc.RateCount;$i++){[BitConverter]::ToUInt32($data,32+4*$i)})
 $names=@('ActmonError','AverageActiveKhz','RawActiveKhz','TargetKhz','DisplayFloorKhz','QosError','TransitionCount')
 for($i=0;$i -lt $names.Count;$i++){
  $emc[$names[$i]]=if($i -in @(0,5)){[BitConverter]::ToInt32($data,160+4*$i)}else{[BitConverter]::ToUInt32($data,160+4*$i)}
 }
 [pscustomobject]@{Utc=[DateTime]::UtcNow.ToString('o');Emc=$emc};return
}
if($Mode -ge 0){$null=Power-Request '\\.\SwitchBacklight' 0x0022a04c ([byte[]]([BitConverter]::GetBytes([uint32]2)+[BitConverter]::GetBytes([uint32]$Mode))) 0}
if($ChargeLimit -ge 0){$null=Power-Request '\\.\SwitchBattery' 0x0022a054 ([BitConverter]::GetBytes([uint32]$ChargeLimit)) 0}
$data=Power-Request '\\.\SwitchBacklight' 0x00226048 @() 132
$names=@('Version','IsLite','CpuConfigured','PowerMode','CpuTargetKhz','CpuActualKhz','CpuBootKhz','CpuMv','RamActualKhz','GpuPolicyMode','GpuPolicyMinKhz','GpuPolicyMaxKhz','GpuReportedKhz','GpuReportedUv','GpuStatusWord','TempC','TempValid','FanDuty','Flags','CpuPolicyMinKhz','CpuPolicyMaxKhz','CpuRequestedMaxKhz','GpuRequestedMaxKhz','CpuSpeedo','CpuProcessId','CpuDfllAvailable','CpuDfllActive','CpuLastError','CpuLoadPermille','GpuSampleAgeMs','SocTempC','BoardTempC','TempSampleAgeMs')
$power=[ordered]@{};for($i=0;$i -lt $names.Count;$i++){$power[$names[$i]]=[BitConverter]::ToUInt32($data,4*$i)}
if($power.Version -ne 2){throw 'Unexpected power ABI version'}
if($PowerOnly){[pscustomobject]@{Utc=[DateTime]::UtcNow.ToString('o');Power=$power};return}
$data=Power-Request '\\.\SwitchBattery' 0x00226050 @() 44
$names=@('Version','LimitCenter','BandMin','BandMax','CapacityPercent','VoltageMv','Reg00InputSrc','Reg01PwrOn','Reg08SysStat','LastApplyStatus','Flags')
$battery=[ordered]@{};for($i=0;$i -lt $names.Count;$i++){$battery[$names[$i]]=[BitConverter]::ToUInt32($data,4*$i)}
if($battery.Version -ne 1){throw 'Unexpected battery ABI version'}
[pscustomobject]@{Utc=[DateTime]::UtcNow.ToString('o');Power=$power;Battery=$battery}
