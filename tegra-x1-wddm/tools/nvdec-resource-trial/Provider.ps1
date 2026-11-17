# SPDX-License-Identifier: MIT
# Optional clock-provider package: stage without install, select only with the
# GPU startup gate closed, and restore before activating the original KMD.
function Provider-Property([string]$Name) {
 (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0402\0' -KeyName ('DEVPKEY_Device_'+$Name)).Data
}
function Assert-ProviderSelected([ValidateSet('Candidate','Baseline')][string]$Which) {
 Assert-File "$env:windir\System32\drivers\switchfan.sys" $config.Provider.$Which
 if((Provider-Property 'ProblemCode') -ne 0){throw 'Clock provider is not started'}
 if($Which -eq 'Candidate') {
  $r=Read-Json (Join-Path $root 'provider-candidate-selection.json')
  if($r.Phase -notin @('Selected','SelectedAfterBoot')){throw 'Candidate provider selection is incomplete'}
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
function Invoke-ProviderSelection([ValidateSet('Candidate','Baseline')][string]$Which) {
 Assert-Closed;Assert-Power
 $expectedPhase=if($Which -eq 'Candidate'){'CandidateRebootRequested'}else{'BaselineRebootRequested'}
 $acpi=if($Which -eq 'Candidate'){$config.CandidateAcpi}else{$config.BaselineAcpi}
 if(!$state -or $state.Phase -ne $expectedPhase -or (Read-Boot) -eq $state.Boot){throw 'Provider selection requires the completed gated boot'}
 Assert-LoadedDsdt (Read-LoadedDsdt (Join-Path $root ('loaded-provider-'+$Which+'.dat'))) $acpi
 $receipt=Join-Path $root ('provider-'+$Which.ToLowerInvariant()+'-selection.json');$expected=$config.Provider.$Which
 if(Test-Path $receipt) {
  $r=Read-Json $receipt
  if($r.Phase -eq 'RebootRequired' -and $r.RestartRequested -and (Read-Boot) -ne $r.Boot) {
   Assert-File "$env:windir\System32\drivers\switchfan.sys" $expected
   if((Provider-Property 'ProblemCode') -ne 0){throw 'Provider did not start after the required boot'}
   $r.Phase='SelectedAfterBoot';Write-ActivationJson $receipt $r
  } elseif($r.Phase -notin @('Selected','SelectedAfterBoot')){throw 'Provider selection was already attempted; inspect its receipt'}
  Assert-ProviderSelected $Which;return
 }
 if($Which -eq 'Candidate'){Assert-ProviderStaged}
 $directory=Join-Path $root $(if($Which -eq 'Candidate'){'provider'}else{'baseline-fan'})
 $r=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');Boot=Read-Boot;Target=$expected;Phase='Selecting';RestartRequested=$false;RebootRequired=$false;Before=File-Record "$env:windir\System32\drivers\switchfan.sys"}
 Write-ActivationJson $receipt $r
 if($r.Before.Sha256 -eq $expected.Sha256 -and (Provider-Property 'ProblemCode') -eq 0) {
  $r.Phase='Selected';$r.Reused=$true;Write-ActivationJson $receipt $r;return
 }
 if(!('NvdecProviderDriverApi' -as [type])){Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class NvdecProviderDriverApi {
 [DllImport("newdev.dll",EntryPoint="UpdateDriverForPlugAndPlayDevicesW",CharSet=CharSet.Unicode,SetLastError=true)]
 [return:MarshalAs(UnmanagedType.Bool)] public static extern bool Update(IntPtr p,string id,string inf,uint flags,out bool reboot);
}
'@}
 $reboot=$false;$ok=[NvdecProviderDriverApi]::Update([IntPtr]::Zero,'ACPI\SWOA0402',(Join-Path $directory 'switchfan.inf'),5,[ref]$reboot)
 $r.ErrorCode=if($ok){0}else{[Runtime.InteropServices.Marshal]::GetLastWin32Error()}
 $r.RebootRequired=$reboot;$r.Phase=if(!$ok){'Failed'}elseif($reboot){'RebootRequired'}else{'Selected'}
 $r.Actual=File-Record "$env:windir\System32\drivers\switchfan.sys";$r.Code=Provider-Property 'ProblemCode';$r.End=[DateTime]::UtcNow.ToString('o');Write-ActivationJson $receipt $r
 if(!$ok -or $reboot){throw ('Provider selection requires inspection: '+$r.Phase+'/'+$r.ErrorCode)}
 Assert-ProviderSelected $Which;Assert-Closed
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
  if($r.Phase -ne 'RebootRequired' -or !$r.RebootRequired -or $r.RestartRequested -or (Read-Boot) -ne $r.Boot){throw 'No new provider restart is eligible'}
  $r.RestartRequested=$true;Write-ActivationJson $path $r
  & shutdown.exe /r /t 10 /d p:0:0 /c 'Complete the recorded clock-provider package transition'
  if($LASTEXITCODE){throw 'Provider restart was rejected'}
 } else {throw 'Unknown provider action'}
}
