# SPDX-License-Identifier: MIT
# Adapted from the existing .158/.161 GPU trial workflow; see tool-provenance.json.
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';$dir=$PSScriptRoot;$id='ACPI\SWOA0F01\0';$evidence=Join-Path $dir 'evidence'
try {
 $version=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
 $problem=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
 if($version -ne '0.1.0.158'){throw 'Candidate is outside its observation window'}
 $pnp=Get-PnpDevice -InstanceId $id
 & "$dir\display-state.exe" wake > "$evidence\observed-wake.json" 2> "$evidence\observed-wake.err"
 $wake=$LASTEXITCODE
 & "$dir\display-state.exe" visible > "$evidence\observed-display.json" 2> "$evidence\observed-display.err"
 $visible=$LASTEXITCODE -eq 0
 $preference=$null
 if($problem -eq 43 -and $visible){
  & "$dir\display-state.exe" verify "$dir\user-display.bin" > "$evidence\observed-preference.json" 2> "$evidence\observed-preference.err"
  $preference=$LASTEXITCODE -eq 0
 }
 $r=Start-Process -FilePath "$dir\desktop-path.exe" -ArgumentList "$dir\tegragpuno3d.dll" -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$evidence\desktop-path.log" -RedirectStandardError "$evidence\desktop-path.err"
 @{Exit=$r.ExitCode;Session=(Get-Process -Id $PID).SessionId;Utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Set-Content "$evidence\desktop-exit.json"
 @{Utc=[DateTime]::UtcNow.ToString('o');Session=(Get-Process -Id $PID).SessionId;Version=$version;Problem=$problem;Status=$pnp.Status;Name=$pnp.FriendlyName;WakeExit=$wake;Visible=$visible;PreferenceValid=$preference}|ConvertTo-Json -Compress|Set-Content "$evidence\observed-interactive.json"
 $observation=@{Utc=[DateTime]::UtcNow.ToString('o');Observation=(Get-Content -Raw "$evidence\observed-interactive.json"|ConvertFrom-Json);Paths=@(Get-Content "$evidence\desktop-path.log"|ForEach-Object {$_|ConvertFrom-Json})}|ConvertTo-Json -Depth 5 -Compress
 $bytes=[Text.Encoding]::UTF8.GetBytes($observation+"`n")
 $f=[IO.FileStream]::new("$evidence\observations.jsonl",[IO.FileMode]::Append,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
 try{$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally{$f.Dispose()}
}catch{$_|Out-String|Set-Content "$evidence\desktop-task.err";throw}
