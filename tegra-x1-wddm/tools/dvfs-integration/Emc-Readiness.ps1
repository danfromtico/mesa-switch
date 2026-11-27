# SPDX-License-Identifier: MIT
# This is an installation-stage predicate, not runtime EMC acceptance.
function Test-EmcBeforeNative($Power,$Emc,[int]$GpuCode) {
 if(!$Power -or !$Emc -or $Emc.Version -ne 1 -or
    $Emc.ActualKhz -le 0 -or $Emc.SampleAgeMs -ge 2000 -or
    ($Emc.Flags -band 7) -ne 7 -or ($Emc.Flags -band 144) -ne 0 -or
    $Emc.EnumerationError -ne 0 -or $Emc.QosError -ne 0){return $false}
 if($Emc.LastError -eq 0){return $true}
 # At a trained >1600MHz boot clock, SwitchFan waits for the KMD's GPU-floor
 # ABI/ACK before enabling ANY EMC transition. Requiring that ACK before
 # starting the gated GPU creates an installer dependency cycle. Accept only
 # this unchanged, fresh, fault-free boot state; the conservative650mV PMIC
 # readback covers all supported B01 GPU bins through2500MHz. Clock/voltage
 # transaction guards remain unchanged and runtime must subsequently pass.
 return $GpuCode -eq 43 -and $Emc.LastError -eq -6 -and
    $Emc.ActualKhz -gt 1600000 -and $Emc.ActualKhz -le 2500000 -and
    $Emc.BootKhz -eq $Emc.ActualKhz -and $Emc.TransitionCount -eq 0 -and
    $Emc.ActmonError -eq 0 -and $Power.GpuReportedKhz -eq 0 -and
    ($Power.Flags -band 16) -eq 0 -and $Power.GpuReportedUv -ge 650000
}
