# SPDX-License-Identifier: MIT
# Catalog generation only. Does not install drivers, edit policy or restart.
param([Parameter(Mandatory=$true)][string]$Archive,
      [Parameter(Mandatory=$true)][string]$Directory,
      [Parameter(Mandatory=$true)][string]$Sha256)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$tool=Join-Path $env:ProgramData 'SwitchWOA\GpuUpdate\bit14-catalog-stage\catalog-tool\Inf2Cat.exe'
if((Get-FileHash -LiteralPath $Archive).Hash -ne $Sha256){throw 'Catalog input hash differs'}
if(Test-Path -LiteralPath $Directory){throw 'Catalog directory already exists; inspect it before retrying'}
Expand-Archive -LiteralPath $Archive -DestinationPath $Directory
$manifest=Get-Content -LiteralPath (Join-Path $Directory 'input.json') -Raw|ConvertFrom-Json
foreach($e in $manifest.files.PSObject.Properties){
    $f=Join-Path $Directory $e.Name
    if((Get-Item -LiteralPath $f).Length -ne $e.Value.Length -or
       (Get-FileHash -LiteralPath $f).Hash -ne $e.Value.Sha256){throw ('Input differs: '+$e.Name)}
}
$results=@()
try {
    foreach($name in @('tegragpurender','switchfan','switchpwrbtn','switchbattery')){
        $p=[Diagnostics.Process]::new()
        $p.StartInfo.FileName=$tool
        $p.StartInfo.Arguments='/driver:"'+(Join-Path $Directory $name)+'" /os:10_NI_ARM64'
        $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
        $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
        try {
            $null=$p.Start();$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
            if(!$p.WaitForExit(45000)){$p.Kill();throw 'Inf2Cat deadline'}
            $results+=@{Name=$name;Exit=$p.ExitCode;Output=$stdout.Result;Error=$stderr.Result}
            if($p.ExitCode){throw ('Inf2Cat failed: '+$name)}
            $cat=Join-Path $Directory ($name+'\'+$name+'.cat')
            $stream=[IO.FileStream]::new($cat,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
            try{$stream.Flush($true)}finally{$stream.Dispose()}
        } finally {$p.Dispose()}
    }
} finally {
    $json=@{Utc=[DateTime]::UtcNow.ToString('o');Catalogs=$results;DriversInstalled=$false}|ConvertTo-Json -Depth 5
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes($json)
    $stream=[IO.FileStream]::new((Join-Path $Directory 'result.json'),[IO.FileMode]::Create,
        [IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
    $json
}
