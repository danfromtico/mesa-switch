# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$tool=Join-Path $env:ProgramData 'SwitchWOA\GpuUpdate\bit14-catalog-stage\signature-tool\signtool.exe'
$manifest=Get-Content -LiteralPath (Join-Path $Directory 'input.json') -Raw|ConvertFrom-Json
$results=@()
foreach($name in $manifest.driver_names){
    $cat=Join-Path $Directory ($name+'\'+$name+'.cat')
    $members=@($manifest.files.PSObject.Properties|Where-Object {$_.Name.Replace('\','/').StartsWith($name+'/')})
    if($members.Count -lt 2){throw ('Missing SYS/INF catalog members: '+$name)}
    foreach($entry in $members){
        $file=Join-Path $Directory $entry.Name
        if((Get-Item -LiteralPath $file).Length -ne $entry.Value.Length -or
           (Get-FileHash -LiteralPath $file).Hash -ne $entry.Value.Sha256){throw ('Input differs: '+$entry.Name)}
    }
    foreach($file in @($cat)+@($members|ForEach-Object {Join-Path $Directory $_.Name})){
        $p=[Diagnostics.Process]::new();$p.StartInfo.FileName=$tool
        $p.StartInfo.Arguments=if($file -eq $cat){'verify /pa "'+$file+'"'}else{'verify /pa /c "'+$cat+'" "'+$file+'"'}
        $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
        $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
        try {
            $null=$p.Start();$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
            if(!$p.WaitForExit(30000)){$p.Kill();throw 'Signature verification deadline'}
            $results+=@{Driver=$name;Name=[IO.Path]::GetFileName($file);Exit=$p.ExitCode;
                Sha256=(Get-FileHash -LiteralPath $file).Hash;Length=(Get-Item -LiteralPath $file).Length;
                Detail=if($p.ExitCode){$stdout.Result+$stderr.Result}else{'Verified'}}
        }finally{$p.Dispose()}
    }
}
$json=$results|ConvertTo-Json -Depth 5
$bytes=[Text.UTF8Encoding]::new($false).GetBytes($json)
$stream=[IO.FileStream]::new((Join-Path $Directory 'verified.json'),[IO.FileMode]::Create,
    [IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
$json
if(@($results|Where-Object Exit -ne 0).Count){throw 'Signature verification failed'}
