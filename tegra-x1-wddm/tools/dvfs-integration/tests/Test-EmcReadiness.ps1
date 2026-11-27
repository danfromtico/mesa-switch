param([Parameter(Mandatory=$true)][string]$Helper)
$ErrorActionPreference='Stop';. $Helper
function Check([bool]$Ok,[string]$Label){if(!$Ok){throw $Label}}
$p=@{GpuReportedKhz=0;Flags=1;GpuReportedUv=650000}
$e=@{Version=1;ActualKhz=2500000;BootKhz=2500000;SampleAgeMs=140;Flags=71;LastError=-6;EnumerationError=0;QosError=0;ActmonError=0;TransitionCount=0}
Check (Test-EmcBeforeNative $p $e 43) 'Captured gated2500 readiness must pass'
Check (!(Test-EmcBeforeNative $p $e 0)) 'Runtime not-ready is not acceptance'
foreach($entry in @(@('LastError',-2),@('Flags',215),@('Flags',87),@('SampleAgeMs',2000),@('ActualKhz',2499999),@('BootKhz',1600000),@('TransitionCount',1),@('QosError',-2),@('EnumerationError',-1),@('ActmonError',-1))){$v=$e[$entry[0]];$e[$entry[0]]=$entry[1];Check (!(Test-EmcBeforeNative $p $e 43)) ('Must reject '+$entry[0]);$e[$entry[0]]=$v}
$p.GpuReportedUv=640000;Check (!(Test-EmcBeforeNative $p $e 43)) 'Insufficient boot GPU voltage';$p.GpuReportedUv=650000
$p.Flags=17;Check (!(Test-EmcBeforeNative $p $e 43)) 'Live GPU requires completion';$p.Flags=1
$e.ActualKhz=$e.BootKhz=1600000;$e.LastError=0;Check (Test-EmcBeforeNative $p $e 43) 'Retained1600 stage stays valid'
'PASS: gated boot-floor dependency; real faults, stale data, undervoltage and runtime incomplete state still rejected'
