# SPDX-License-Identifier: MIT
# Keep the last identified DWM and KMD state before uninstall erases them.
# Failure is evidence unavailability and must never prevent driver recovery.
param([string]$Directory=$PSScriptRoot)
$ErrorActionPreference='Stop'
$p=[IO.Path]::GetFullPath($Directory)
$stem=Join-Path $p 'pre-recovery'
if(Test-Path ($stem+'.json')){return}
$result=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');DwmDumpCaptured=$false}
$child=$null
try {
    $child=Start-Process "$env:windir\System32\reg.exe" -ArgumentList @('query','HKLM\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters') -NoNewWindow -PassThru -RedirectStandardOutput ($stem+'-kmd.txt') -RedirectStandardError ($stem+'-kmd.err')
    $null=$child.Handle
    if(!$child.WaitForExit(3000)){throw 'KMD registry query timed out'}
    $result.KmdRegistryExit=$child.ExitCode
    $child.Dispose();$child=$null
    $identities=@(Get-ChildItem (Join-Path $p 'evidence') -Filter 'stage-*-identity.json' -File|Sort-Object LastWriteTimeUtc -Descending)
    $identity=$null
    foreach($file in $identities){
        $candidate=[IO.File]::ReadAllText($file.FullName)|ConvertFrom-Json
        if($candidate.Verified -and $candidate.DwmPid -and $candidate.DwmStart){$identity=$candidate;$result.IdentityFile=$file.Name;break}
    }
    if(!$identity){throw 'No verified DWM identity from this trial'}
    $result.DwmPid=$identity.DwmPid
    $result.DwmStart=$identity.DwmStart
    $result.Luid=$identity.Luid
    $birth=[DateTime]::Parse($identity.DwmStart).ToUniversalTime().ToFileTimeUtc()
    $manifest=[IO.File]::ReadAllText((Join-Path $p 'trial-components.json'))|ConvertFrom-Json
    $helper=Join-Path $p 'app\tgd3d-dump-capture.exe'
    $expected=$manifest.app.'tgd3d-dump-capture.exe'
    if(!$expected -or (Get-Item $helper).Length -ne $expected.Length -or (Get-FileHash $helper).Hash -ne $expected.Sha256){throw 'Dump helper does not match the trial manifest'}
    $result.HelperSha256=$expected.Sha256
    $arguments=([string]$identity.DwmPid)+' '+$birth+' dwm.exe "'+$stem+'-dwm.dmp"'
    $child=Start-Process $helper -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput ($stem+'-dwm.log') -RedirectStandardError ($stem+'-dwm.err')
    $null=$child.Handle
    if(!$child.WaitForExit(28000)){throw 'DWM snapshot writer deadline exceeded'}
    $result.DwmCaptureExit=$child.ExitCode
    $result.DwmDumpCaptured=$child.ExitCode -eq 0 -and (Test-Path ($stem+'-dwm.dmp'))
    if($result.DwmDumpCaptured){$result.DumpSha256=(Get-FileHash ($stem+'-dwm.dmp')).Hash}
} catch {
    $result.Error=$_.Exception.Message
} finally {
    if($child){
        try{if(!$child.HasExited){$child.Kill();$child.WaitForExit(2000)|Out-Null}}
        finally{$child.Dispose()}
    }
    $result.End=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText(($stem+'.json'),($result|ConvertTo-Json -Depth 3),[Text.UTF8Encoding]::new($false))
}
