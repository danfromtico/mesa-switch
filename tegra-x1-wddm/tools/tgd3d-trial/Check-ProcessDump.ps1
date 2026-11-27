# SPDX-License-Identifier: MIT
# CPU/process controls on the recovery desktop; never enters native rendering.
param([Parameter(Mandatory=$true)][string]$Helper,
      [Parameter(Mandatory=$true)][string]$OutputDirectory,
      [switch]$IncludeDwm)
$ErrorActionPreference='Stop'
$version=(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
if($version -ne '0.1.0.59'){throw 'Run dump controls on the .59 recovery configuration'}
$null=New-Item -ItemType Directory -Path $OutputDirectory
$exe=[IO.Path]::GetFullPath($Helper)
$fixture=Start-Process $exe -ArgumentList '--fixture' -WindowStyle Hidden -PassThru -RedirectStandardOutput "$OutputDirectory\fixture.jsonl" -RedirectStandardError "$OutputDirectory\fixture.err"
$null=$fixture.Handle
try {
    $watch=[Diagnostics.Stopwatch]::StartNew()
    while((Get-Item "$OutputDirectory\fixture.jsonl").Length -eq 0 -and $watch.Elapsed.TotalSeconds -lt 5){Start-Sleep -Milliseconds 100}
    $identity=Get-Content -Raw "$OutputDirectory\fixture.jsonl"|ConvertFrom-Json
    $base=[IO.Path]::GetFileName($exe)
    & $exe $identity.pid ([uint64]$identity.creation_filetime+1) $base "$OutputDirectory\wrong-identity.dmp" > "$OutputDirectory\wrong-identity.jsonl"
    $wrongExit=$LASTEXITCODE
    if($wrongExit -ne 1 -or (Test-Path "$OutputDirectory\wrong-identity.dmp*") -or $fixture.HasExited){throw 'Wrong-identity control failed'}
    & $exe $identity.pid $identity.creation_filetime $base "$OutputDirectory\fixture.dmp" > "$OutputDirectory\capture-fixture.jsonl"
    $captureExit=$LASTEXITCODE
    if($captureExit -ne 0 -or !(Test-Path "$OutputDirectory\fixture.dmp")){throw 'Snapshot dump control failed'}
    if(!$fixture.WaitForExit(21000) -or $fixture.ExitCode -ne 0){throw 'Control target did not finish normally after capture'}
    $dwmExit=$null
    if($IncludeDwm){
        $targets=@(Get-Process dwm)
        if($targets.Count -ne 1){throw 'Expected one DWM process for the baseline control'}
        $target=$targets[0]
        $birth=$target.StartTime.ToUniversalTime().ToFileTimeUtc()
        & $exe $target.Id $birth 'dwm.exe' "$OutputDirectory\dwm-baseline.dmp" > "$OutputDirectory\capture-dwm.jsonl"
        $dwmExit=$LASTEXITCODE
        if($dwmExit -ne 0){throw ('DWM snapshot capture unavailable: '+$dwmExit)}
        if((Get-Process -Id $target.Id).StartTime.ToUniversalTime().ToFileTimeUtc() -ne $birth){throw 'DWM changed during baseline capture'}
    }
    @{Utc=[DateTime]::UtcNow.ToString('o');Version=$version;HelperSha256=(Get-FileHash $exe).Hash;WrongIdentityExit=$wrongExit;FixtureCaptureExit=$captureExit;FixtureNaturalExit=$fixture.ExitCode;DwmCaptureExit=$dwmExit;GpuTested=$false}|ConvertTo-Json|Set-Content "$OutputDirectory\control.json"
} finally {
    if(!$fixture.HasExited){$fixture.Kill();$fixture.WaitForExit(5000)|Out-Null}
    $fixture.Dispose()
}
