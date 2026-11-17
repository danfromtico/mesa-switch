# SPDX-License-Identifier: MIT
# Optional clock-provider package: stage without install, select only with the
# GPU startup gate closed, and restore before activating the original KMD.
function Provider-Property([string]$Name) {
 (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0402\0' -KeyName ('DEVPKEY_Device_'+$Name)).Data
}
function Assert-ProviderPackageIdentity([ValidateSet('Candidate','Baseline')][string]$Which) {
 Assert-File "$env:windir\System32\drivers\switchfan.sys" $config.Provider.$Which
 $inf=[string](Provider-Property 'DriverInfPath')
 if($inf -notmatch '^oem[0-9]+\.inf$'){throw 'Unexpected selected provider INF'}
 $directory=if($Which -eq 'Candidate'){'provider'}else{'baseline-fan'}
 Assert-File "$env:windir\INF\$inf" (File-Record (Join-Path $root ($directory+'\switchfan.inf')))
 if((Provider-Property 'ProblemCode') -ne 0){throw 'Clock provider is not started'}
}
function Assert-ProviderSelected([ValidateSet('Candidate','Baseline')][string]$Which) {
 Assert-ProviderPackageIdentity $Which
 Assert-ProviderLoadedImage $Which
 if($Which -eq 'Candidate') {
  $r=Read-Json (Join-Path $root 'provider-candidate-selection.json')
  if($r.Phase -notin @('Selected','SelectedAfterBoot')){throw 'Candidate provider selection is incomplete'}
 }
}
function Read-ProviderPeIdentity([string]$Path) {
 $b=[IO.File]::ReadAllBytes($Path)
 if($b.Length -lt 256 -or [BitConverter]::ToUInt16($b,0) -ne 0x5a4d){throw 'Invalid provider PE header'}
 [long]$pe=[BitConverter]::ToUInt32($b,60)
 if($pe -gt $b.Length-112 -or [BitConverter]::ToUInt32($b,$pe) -ne 0x4550 -or [BitConverter]::ToUInt16($b,$pe+4) -ne 0xaa64 -or [BitConverter]::ToUInt16($b,$pe+24) -ne 0x20b){throw 'Provider must be an ARM64 PE32+ image'}
 @{checksum=[BitConverter]::ToUInt32($b,$pe+88);timestamp=[BitConverter]::ToUInt32($b,$pe+8);image_size=[BitConverter]::ToUInt32($b,$pe+80)}
}
function Test-ProviderSameBinary($A,$B) {
 return $A.Length -gt 0 -and $A.Length -eq $B.Length -and
        $A.Sha256 -match '^[0-9a-fA-F]{64}$' -and $A.Sha256 -eq $B.Sha256
}
function Get-ProviderLoadedObservation([string]$Which) {
 $path=Join-Path $root ('provider-loaded-'+$Which.ToLowerInvariant()+'.json')
 if(Test-Path $path){$h=Read-Json $path;if(@($h.Observations).Count){return @($h.Observations)[-1]}}
 return $null
}
function Test-ProviderTraceReplacement($History,[string]$Boot,$Target,$Tool) {
 # One corrected observer may replace a completed, loss-only trace in this
 # boot. Its executable must change; driver identity and boot must not.
 # Running/unknown traces and actual image mismatches are never retryable.
 $same=@($History|Where-Object {Same-Boot $_.Boot $Boot})
 if($same.Count -ne 1){return $false}
 $last=$same[0]
 return $last.Phase -eq 'Failed' -and $last.Exit -eq 5 -and
  $last.Target.Sha256 -eq $Target.Sha256 -and $last.Tool.Sha256 -and
  $last.Tool.Sha256 -ne $Tool.Sha256 -and
  $last.Stderr -match '(?m)^StopTrace=0 events_lost=[1-9][0-9]* buffers_lost=0 session=NvdecImageIdentity-[0-9]+\r?$'
}
function Assert-ProviderLoadedImage([ValidateSet('Candidate','Baseline')][string]$Which) {
 # Deferred copying can finish after the old image has already loaded. A disk
 # hash, selected INF and one boot therefore do not establish the live image.
 $tool=Join-Path $root 'loaded-image-identity.exe';$expected=$config.InputFiles.'loaded-image-identity.exe'
 if(!$expected){throw 'A hashed ETW image-rundown helper is required'}
 Assert-File $tool $expected
 $path=Join-Path $root ('provider-loaded-'+$Which.ToLowerInvariant()+'.json');$boot=Read-Boot
 $history=@();if(Test-Path $path){$history=@((Read-Json $path).Observations)}
 $last=Get-ProviderLoadedObservation $Which
 if($last -and (Same-Boot $last.Boot $boot)) {
  if($last.Phase -eq 'Verified' -and $last.Target.Sha256 -eq $config.Provider.$Which.Sha256 -and $last.Tool.Sha256 -eq $expected.Sha256){return}
  if(!(Test-ProviderTraceReplacement $history $boot $config.Provider.$Which $expected)){throw ('Loaded provider observation is incomplete or mismatched: '+$path)}
 }
 $directory=if($Which -eq 'Candidate'){'provider'}else{'baseline-fan'}
 $other=if($Which -eq 'Candidate'){'baseline-fan'}else{'provider'}
 $want=Read-ProviderPeIdentity (Join-Path $root ($directory+'\switchfan.sys'))
 $alternate=Read-ProviderPeIdentity (Join-Path $root ($other+'\switchfan.sys'))
 $sameBinary=Test-ProviderSameBinary $config.Provider.Candidate $config.Provider.Baseline
 if(!$sameBinary -and $want.checksum -eq $alternate.checksum -and $want.timestamp -eq $alternate.timestamp -and $want.image_size -eq $alternate.image_size){throw 'Paired images cannot be distinguished by ETW PE identity'}
 $etl=Join-Path $root ('provider-loaded-'+$Which.ToLowerInvariant()+'-'+[Guid]::NewGuid().ToString('N')+'.etl')
 $r=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Boot=$boot;Phase='Running';Target=$config.Provider.$Which;ExpectedPe=$want;Tool=$expected;Etl=$etl;MatchesOther=$false}
 $history+=@($r);Write-ActivationJson $path @{Observations=$history}
 $process=[Diagnostics.Process]::new()
 try {
  $process.StartInfo.FileName=$tool;$process.StartInfo.Arguments='"'+$etl+'"'
  $process.StartInfo.UseShellExecute=$false;$process.StartInfo.CreateNoWindow=$true
  $process.StartInfo.RedirectStandardOutput=$true;$process.StartInfo.RedirectStandardError=$true
  $null=$process.Start();$r.ProcessId=$process.Id;Write-ActivationJson $path @{Observations=$history}
  $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
  if(!$process.WaitForExit(20000)){$r.Phase='ObservationExpired';throw 'Image rundown still running; inspect process and session before any retry'}
  $r.Exit=$process.ExitCode;$r.Stdout=$stdout.Result;$r.Stderr=$stderr.Result
  if($r.Exit){throw ('Image rundown failed: '+$r.Exit)}
  $rows=@($r.Stdout -split '\r?\n'|Where-Object {$_}|ForEach-Object {$_|ConvertFrom-Json}|Where-Object image -eq 'switchfan.sys')
  if(!$rows.Count -or @($rows|Where-Object {$_.pid -ne 0 -or $_.opcode -notin @(3,4)}).Count){throw 'Missing or ambiguous kernel provider image rundown'}
  $r.Images=$rows
  $different=@($rows|Where-Object {$_.checksum -ne $want.checksum -or $_.timestamp -ne $want.timestamp -or $_.image_size -ne $want.image_size})
  $r.MatchesOther=!$sameBinary -and @($rows|Where-Object {$_.checksum -ne $alternate.checksum -or $_.timestamp -ne $alternate.timestamp -or $_.image_size -ne $alternate.image_size}).Count -eq 0
  $r.EtlFile=File-Record $etl
  if(!(Same-Boot (Read-Boot) $boot)){throw 'Boot changed during image rundown'}
  if($different.Count){$r.Phase='Mismatch';throw 'Disk provider and loaded image differ; no decode or baseline promotion permitted'}
  $r.Phase='Verified'
 } catch {
  if($r.Phase -eq 'Running'){$r.Phase='Failed'}
  $r.Error=$_.Exception.Message;throw
 } finally {
  $process.Dispose();$r.End=[DateTime]::UtcNow.ToString('o');Write-ActivationJson $path @{Observations=$history}
 }
}
function Find-ProviderStore {
 $verified=Read-Json (Join-Path $root 'signature-result.json')
 $members=@($verified.Files|Where-Object Directory -eq 'provider')
 if($members.Count -ne 3){throw 'Expected SYS/INF/CAT provider verification'}
 $matches=@(foreach($dir in Get-ChildItem "$env:windir\System32\DriverStore\FileRepository" -Directory -Filter 'switchfan.inf_arm64_*') {
  $file=Join-Path $dir.FullName 'switchfan.sys'
  if(Test-Path $file){$s=File-Record $file;if($s.Sha256 -eq $config.Provider.Candidate.Sha256 -and $s.Length -eq $config.Provider.Candidate.Length){$dir.FullName}}
 })
 if(!$matches.Count){return $null}
 if($matches.Count -ne 1){throw 'Provider store identity is ambiguous'}
 foreach($f in $members){Assert-File (Join-Path $matches[0] $f.Name) @{Length=$f.Bytes;Sha256=$f.Sha256}}
 if(!('NvdecProviderStoreApi' -as [type])){Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NvdecProviderStoreApi {
 [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 [return:MarshalAs(UnmanagedType.Bool)] public static extern bool SetupGetInfPublishedNameW(string path,StringBuilder value,uint size,out uint required);
 [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 [return:MarshalAs(UnmanagedType.Bool)] public static extern bool SetupGetInfDriverStoreLocationW(string path,IntPtr platform,IntPtr locale,StringBuilder value,uint size,out uint required);
}
'@}
 $inf=Join-Path $matches[0] 'switchfan.inf';$published=[Text.StringBuilder]::new(260);[uint32]$needed=0
 if(![NvdecProviderStoreApi]::SetupGetInfPublishedNameW($inf,$published,260,[ref]$needed)){throw 'Provider published-INF query failed'}
 $systemInf=Join-Path "$env:windir\INF" ([IO.Path]::GetFileName($published.ToString()))
 Assert-File $systemInf (File-Record $inf)
 $resolved=[Text.StringBuilder]::new(260)
 if(![NvdecProviderStoreApi]::SetupGetInfDriverStoreLocationW($systemInf,[IntPtr]::Zero,[IntPtr]::Zero,$resolved,260,[ref]$needed)){throw 'Provider driver-store query failed'}
 if([IO.Path]::GetFullPath($resolved.ToString()) -ne [IO.Path]::GetFullPath($inf)){throw 'Provider INF round-trip differs'}
 @{Directory=$matches[0];PublishedInf=$systemInf}
}
function Assert-ProviderStaged {
 if(!$state.ProviderStaged){throw 'StageProviderPackage must finish before the boot-table change'}
 $receipt=Read-Json (Join-Path $root 'provider-staging.json');$actual=Find-ProviderStore
 if(!$actual -or $actual.Directory -ne $receipt.Package.Directory -or $actual.PublishedInf -ne $receipt.Package.PublishedInf){throw 'Provider staging identity changed'}
}
function Defer-ProviderInstall([string]$Directory) {
 # An unexpected reboot interrupted the live provider selection; the last
 # complete SetupAPI entry was Restarting Devices. Select the signed package with
 # public deferred-install flags; never hot-remove the power/clock provider.
 $expected=$config.InputFiles.'defer-provider.exe'
 if(!$expected){throw 'A hashed deferred-install helper is required'}
 $tool=Join-Path $root 'defer-provider.exe';Assert-File $tool $expected
 $output=& $tool --defer (Join-Path $Directory 'switchfan.inf')
 $exit=$LASTEXITCODE
 @{Exit=$exit;Output=$output -join "`n"}
}
function Invoke-ProviderSelection([ValidateSet('Candidate','Baseline')][string]$Which) {
 Assert-Closed;Assert-Power
 $expectedPhase=if($Which -eq 'Candidate'){'CandidateRebootRequested'}else{'BaselineRebootRequested'}
 $acpi=if($Which -eq 'Candidate'){$config.CandidateAcpi}else{$config.BaselineAcpi}
 if(!$state -or $state.Phase -ne $expectedPhase -or (Same-Boot (Read-Boot) $state.Boot)){throw 'Provider selection requires the completed gated boot'}
 Assert-LoadedDsdt (Read-LoadedDsdt (Join-Path $root ('loaded-provider-'+$Which+'.dat'))) $acpi
 $receipt=Join-Path $root ('provider-'+$Which.ToLowerInvariant()+'-selection.json');$expected=$config.Provider.$Which
 if(Test-Path $receipt) {
  $r=Read-Json $receipt
  if($r.Phase -eq 'RebootRequired' -and $r.RestartRequested -and !(Same-Boot (Read-Boot) $r.Boot)) {
   Assert-ProviderPackageIdentity $Which
   Assert-ProviderLoadedImage $Which
   $r.Phase='SelectedAfterBoot';Write-ActivationJson $receipt $r
  } elseif($r.Phase -notin @('Selected','SelectedAfterBoot')){throw 'Provider selection was already attempted; inspect its receipt'}
  Assert-ProviderSelected $Which;return
 }
 if($Which -eq 'Candidate'){Assert-ProviderStaged}
 $directory=Join-Path $root $(if($Which -eq 'Candidate'){'provider'}else{'baseline-fan'})
 $r=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Boot=Read-Boot;Target=$expected;Phase='Selecting';RestartRequested=$false;RebootRequired=$false;Before=File-Record "$env:windir\System32\drivers\switchfan.sys"}
 Write-ActivationJson $receipt $r
 if($r.Before.Sha256 -eq $expected.Sha256 -and (Provider-Property 'ProblemCode') -eq 0) {
  Assert-ProviderPackageIdentity $Which
  Assert-ProviderLoadedImage $Which
  $r.Phase='Selected';$r.Reused=$true;Write-ActivationJson $receipt $r;return
 }
 try {
  $deferred=Defer-ProviderInstall $directory
  $r.ErrorCode=$deferred.Exit;$r.Output=$deferred.Output
  $ok=$deferred.Exit -eq 0 -and $deferred.Output -match '(?m)^RESULT=STAGED_FOR_REBOOT live driver identity is not verified\r?$'
 } catch {
  $r.ErrorCode=-1;$r.Error=$_.Exception.Message;$ok=$false
 }
 # A disk hash/Code0 cannot prove that the live image was replaced. Every
 # changed package needs a recorded boot before it can become Selected.
 $r.RebootRequired=$ok;$r.Phase=if($ok){'RebootRequired'}else{'Failed'}
 $r.Actual=File-Record "$env:windir\System32\drivers\switchfan.sys";$r.Code=Provider-Property 'ProblemCode';$r.End=[DateTime]::UtcNow.ToString('o');Write-ActivationJson $receipt $r
 throw ('Provider selection requires inspection: '+$r.Phase+'/'+$r.ErrorCode)
}
function Invoke-ProviderAction([string]$Operation) {
 if($Operation -eq 'StageProviderPackage') {
  if(!$state -or $state.Phase -notin @('Prepared','StagingProvider') -or $state.ProviderStaged){throw 'Expected a prepared, unstaged provider'}
  Assert-RetainedFiles -Baseline;Assert-Power
  if((Device-Property 'ProblemCode') -ne 0){throw 'Baseline GPU is not active'}
  if($state.Phase -eq 'StagingProvider') {
   $prior=Read-Json (Join-Path $root 'provider-staging.json')
   if(!$prior.ImportRequested -or $null -eq $prior.Exit -or $prior.Exit -ne 0){throw 'Previous import is not proven complete; do not repeat it'}
   $actual=Find-ProviderStore;if(!$actual){throw 'Completed provider import is not registered'}
   $prior|Add-Member NoteProperty Package $actual -Force
   Write-ActivationJson (Join-Path $root 'provider-staging.json') $prior
   $state.ProviderStaged=$true;$state.Phase='Prepared';Save-State $state;return
  }
  $receipt=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');ImportRequested=$false;Reused=$false}
  $staged=Find-ProviderStore
  if($staged){$receipt.Reused=$true}else{
   $state.Phase='StagingProvider';Save-State $state
   $receipt.ImportRequested=$true;Write-ActivationJson (Join-Path $root 'provider-staging.json') $receipt
   $output=& pnputil.exe /add-driver (Join-Path $root 'provider\switchfan.inf');$exit=$LASTEXITCODE
   $receipt.Exit=$exit;$receipt.Output=$output -join "`n";Write-ActivationJson (Join-Path $root 'provider-staging.json') $receipt
   if($exit){throw ('Provider import failed: '+$exit)}
   $staged=Find-ProviderStore;if(!$staged){throw 'Imported provider not registered'}
  }
  $receipt.Package=$staged;$receipt.End=[DateTime]::UtcNow.ToString('o');Write-ActivationJson (Join-Path $root 'provider-staging.json') $receipt
  $state.ProviderStaged=$true;$state.Phase='Prepared';Save-State $state
 } elseif($Operation -eq 'SelectProvider') {Invoke-ProviderSelection 'Candidate'}
 elseif($Operation -eq 'SelectOriginalProvider') {Invoke-ProviderSelection 'Baseline'}
 elseif($Operation -eq 'ProviderRestart') {
  Assert-Closed;Assert-Power
  $which=if($state.Phase -eq 'CandidateRebootRequested'){'candidate'}elseif($state.Phase -eq 'BaselineRebootRequested'){'baseline'}else{throw 'Unexpected provider-restart phase'}
  $path=Join-Path $root ('provider-'+$which+'-selection.json');$r=Read-Json $path
  if($r.Phase -ne 'RebootRequired' -or !$r.RebootRequired -or $r.RestartRequested -or !(Same-Boot (Read-Boot) $r.Boot)){throw 'No new provider restart is eligible'}
  $r.RestartRequested=$true;Write-ActivationJson $path $r
  & shutdown.exe /r /t 10 /d p:0:0 /c 'Complete the recorded clock-provider package transition'
  if($LASTEXITCODE){throw 'Provider restart was rejected'}
 } elseif($Operation -eq 'ProviderLoadRestart') {
  # A second boot is eligible only after a successful deferred install, an
  # actual first boot, and ETW proof that the known other image remains loaded.
  # Never repeat installation or infer this transition from an SSH timeout.
  Assert-Closed;Assert-Power
  $which=if($state.Phase -eq 'CandidateRebootRequested'){'Candidate'}elseif($state.Phase -eq 'BaselineRebootRequested'){'Baseline'}else{throw 'Unexpected loaded-image recovery phase'}
  $selection=Read-Json (Join-Path $root ('provider-'+$which.ToLowerInvariant()+'-selection.json'))
  $observed=Get-ProviderLoadedObservation $which;$boot=Read-Boot
  if($selection.Phase -ne 'RebootRequired' -or !$selection.RestartRequested -or (Same-Boot $boot $selection.Boot)){throw 'No completed deferred-install boot to reconcile'}
  if(!$observed -or $observed.Phase -ne 'Mismatch' -or !$observed.MatchesOther -or !(Same-Boot $boot $observed.Boot) -or $observed.Target.Sha256 -ne $config.Provider.$which.Sha256){throw 'No current known-image mismatch proves another boot is needed'}
  Assert-ProviderPackageIdentity $which
  $acpi=if($which -eq 'Candidate'){$config.CandidateAcpi}else{$config.BaselineAcpi}
  Assert-File "$env:windir\System32\acpitabl.dat" $acpi
  $receipt=Join-Path $root ('provider-'+$which.ToLowerInvariant()+'-load-restart.json')
  if(Test-Path $receipt){throw 'Loaded-image reconciliation boot already requested; inspect, do not repeat'}
  Write-ActivationJson $receipt @{Utc=[DateTime]::UtcNow.ToString('o');Boot=$boot;Observation=$observed;RestartRequested=$true;DriversInstalled=$false}
  & shutdown.exe /r /t 10 /d p:0:0 /c 'Complete provider image replacement verified by ETW'
  if($LASTEXITCODE){throw 'Loaded-image recovery restart rejected'}
 } else {throw 'Unknown provider action'}
}
