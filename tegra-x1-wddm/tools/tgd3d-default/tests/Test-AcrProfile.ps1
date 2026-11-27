$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../Baseline-Promotion.ps1')
. (Join-Path $PSScriptRoot '../Baseline-Device.ps1')
$old = Read-SwitchPolicyJson (Join-Path $PSScriptRoot '../../../docs/graphics-baseline.json')
$name = 'nv_acr_ucode_prod.bin'
$old.components.PSObject.Properties.Remove($name)
# Exercise migration from the original seven-component baseline.
$priorWindir = $env:windir
$root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
try {
    $env:windir=$root
    $path=Join-Path $root "System32/drivers/TegraGpu/$name"
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllBytes($path,[byte[]]@(1,2))
    $hash=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
    $actual=Get-SwitchInstalledProfile $root @($name)
    if ($actual.$name -ne $hash) { throw 'Wrong installed path' }
    $tracked=($old.components|ConvertTo-Json|ConvertFrom-Json)
    $tracked|Add-Member -NotePropertyName $name -NotePropertyValue $hash
    Assert-SwitchProfile $tracked
    if(Test-SwitchSameProfile $tracked $old.components){throw 'Firmware identity lost'}
    $proof=[pscustomobject]@{trial=[pscustomobject]@{boot_record=11};cases=@()}
    function Block([scriptblock]$test,[string]$reason) {
        try { & $test } catch {if($_.Exception.Message -match $reason){return}; throw}
        throw "Expected rejection matching $reason"
    }
    Block {Assert-SwitchChangedComponentLoads $old.components $tracked $proof} 'No test'
    Block {Assert-SwitchChangedComponentLoads $tracked $old.components $proof} 'already tracked'
    $proof.cases=@([pscustomobject]@{before_boot=11;loaded_components=$actual})
    Assert-SwitchChangedComponentLoads $old.components $tracked $proof
    [IO.File]::WriteAllBytes($path,[byte[]]@(3,4))
    if ((Get-SwitchInstalledProfile $root @($name)).$name -eq $hash){throw 'Changed bytes matched'}
    Block {Get-SwitchInstalledProfile $root @('other.bin')} 'Invalid component'
    'PASS firmware identity, candidate-boot evidence, rollback hash and file allowlist'
} finally {
    $env:windir=$priorWindir
    if([IO.Directory]::Exists($root)){[IO.Directory]::Delete($root,$true)}
}
