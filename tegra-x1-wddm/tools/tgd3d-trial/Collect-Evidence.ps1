# SPDX-License-Identifier: MIT
# Collect the completed trial only after verified .59 recovery.
param([Parameter(Mandatory=$true)][string]$Archive)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
Check-Baseline
$p=$PSScriptRoot
if(Get-ScheduledTask -TaskName 'Tgd3dTrial*' -ErrorAction SilentlyContinue) { throw 'Trial tasks remain' }
foreach($trace in 'Tgd3dTrialDxg','Tgd3dTrialUmd') {
    if(Get-EtwTraceSession -Name $trace -ErrorAction SilentlyContinue) { throw 'Trial trace remains active' }
}
if(Test-Path "$env:windir\System32\drivers\TgGpuBootTest158.bin") { throw 'Trial permit remains' }
if(Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowDesktopStart') { throw 'Trial grant remains' }
$verified=Read-Json (Join-Path $p 'recovery-verified.json')
if(!$verified.Verified) { throw 'Display/platform restoration is not verified' }
$destination=[IO.Path]::GetFullPath($Archive)
if($destination.StartsWith($p.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path $destination)) { throw 'Use a new archive outside the trial directory' }
$m=Read-Json (Join-Path $p 'trial-components.json')
Check-File "$p\app\tgd3d-trace-export.exe" $m.app.'tgd3d-trace-export.exe'
Check-File "$p\etl-export-full.exe" $m.helpers.'etl-export-full.exe'
$export=@()
foreach($f in Get-ChildItem $p -Filter 'dxg-start.etl*' -File|Where-Object {$_.Name -match '^dxg-start\.etl(\.[0-9]+)?$'}) {
    $base=$f.FullName+'.recovered'
    if(Test-Path "$base.umd.jsonl") { throw 'An export already exists; preserve it' }
    $process=Start-Process "$p\app\tgd3d-trace-export.exe" -ArgumentList ('"'+$f.FullName+'"') -Wait -PassThru -NoNewWindow -RedirectStandardOutput "$base.umd.jsonl" -RedirectStandardError "$base.umd.log"
    $code=$process.ExitCode
    $kernel=Start-Process "$p\etl-export-full.exe" -ArgumentList @(('"'+$f.FullName+'"'),('"'+$base+'.bin"'),'3600') -Wait -PassThru -NoNewWindow -RedirectStandardOutput "$base.kernel.log" -RedirectStandardError "$base.kernel.err"
    $export+=@{File=$f.Name;UmdExit=$code;KernelExit=$kernel.ExitCode;UmdSummary=[IO.File]::ReadAllText("$base.umd.log");KernelSummary=[IO.File]::ReadAllText("$base.kernel.log")}
}
$export|ConvertTo-Json -Depth 5|Set-Content "$p\recovered-export.json" -Encoding UTF8
if(Test-Path "$p\umd.etl") {
    if(Test-Path "$p\umd.etl.recovered.jsonl"){throw 'A UMD export already exists; preserve it'}
    $process=Start-Process "$p\app\tgd3d-trace-export.exe" -ArgumentList ('"'+$p+'\umd.etl"') -Wait -PassThru -NoNewWindow -RedirectStandardOutput "$p\umd.etl.recovered.jsonl" -RedirectStandardError "$p\umd.etl.recovered.log"
    if($process.ExitCode){throw 'Separate UMD export failed'}
}
$start=[DateTime]::Parse((Read-Json "$p\before-restart.json").Utc).AddMinutes(-2)
@(Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$start;Id=@(41,6005,6006,6008,1001,1074,12,13)} -ErrorAction SilentlyContinue|ForEach-Object {
    @{Time=$_.TimeCreated.ToUniversalTime().ToString('o');RecordId=$_.RecordId;Id=$_.Id;Provider=$_.ProviderName;Xml=$_.ToXml();Message=$_.Message}
})|ConvertTo-Json -Depth 5|Set-Content "$p\system-events.json" -Encoding UTF8
$crash=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'|Select-Object CrashDumpEnabled,DumpFile,MinidumpDir
$dumps=@(Get-ChildItem "$env:windir\Minidump" -File -ErrorAction SilentlyContinue|Where-Object {$_.LastWriteTimeUtc -ge $start.ToUniversalTime()})
foreach($dump in $dumps) {
    New-Item -ItemType Directory "$p\crash-dumps" -Force|Out-Null
    Copy-Item $dump.FullName (Join-Path "$p\crash-dumps" $dump.Name)
}
@{Settings=$crash;Dumps=@($dumps|Select-Object Name,Length,LastWriteTimeUtc)}|ConvertTo-Json -Depth 5|Set-Content "$p\crash-records.json" -Encoding UTF8
# Keep native registry output. Serializing provider-owned PowerShell objects
# recursively was present in the SD inventory that exhausted system commit.
& reg.exe query 'HKLM\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'|
    Out-File "$p\kmd-parameters-after.txt" -Encoding UTF8
if($LASTEXITCODE -notin @(0,1)){throw 'KMD registry evidence query failed'}
$entries=@()
foreach($f in Get-ChildItem $p -Recurse -File|Where-Object {$_.Name -ne 'evidence-manifest.json'}) {
    # Directory enumeration can retain an old size after an interrupted writer.
    # Measure and hash through the same file handle.
    $stream=[IO.File]::Open($f.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $sha=[Security.Cryptography.SHA256]::Create()
    try {
        $length=$stream.Length
        $hash=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','')
        if($stream.Position -ne $length) { throw 'Evidence changed during hashing' }
    } finally { $sha.Dispose();$stream.Dispose() }
    $entries+=@{Path=$f.FullName.Substring($p.Length+1);Length=$length;Sha256=$hash}
}
$entries|ConvertTo-Json -Depth 4|Set-Content "$p\evidence-manifest.json" -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($p,$destination,[IO.Compression.CompressionLevel]::Optimal,$false)
@{Archive=$destination;Length=(Get-Item $destination).Length;Sha256=(File-Hash $destination);Files=$entries.Count;Utc=[DateTime]::UtcNow.ToString('o');Export=$export}|ConvertTo-Json -Depth 5
