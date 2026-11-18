param([ValidateSet('Activate','Restore','Status')][string]$Mode='Status')
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$plan = [IO.File]::ReadAllText((Join-Path $dir 'plan.json')) | ConvertFrom-Json
function Digest([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Copy-Durable([string]$Source, [string]$Destination) {
    $bytes = [IO.File]::ReadAllBytes($Source)
    $stream = [IO.FileStream]::new($Destination, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::Read, 4096, [IO.FileOptions]::WriteThrough)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
}
if (!('SwitchIcdFile' -as [type])) {
    Add-Type @'
using System.Runtime.InteropServices;
public static class SwitchIcdFile {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFileExW(string source, string destination, uint flags);
}
'@
}
function SelectFile($row, [bool]$new) {
    $before = $row.before
    $after = $row.after
    $target = if ($new) { $after } else { $before }
    $source = Join-Path $dir ($(if ($new) { 'new-' } else { 'old-' }) + $row.kind + '.json')
    if ((Digest $source) -ne $target) { throw ('Snapshot digest differs for '+$row.kind) }
    $current = Digest $row.live
    if ($current -eq $target) { return }
    if ($current -ne $before -and $current -ne $after) { throw ('Concurrent edit of '+$row.kind) }
    $scratch = $row.live + '.swap-' + [Guid]::NewGuid().ToString('N')
    try {
        Copy-Durable $source $scratch
        if ((Digest $row.live) -ne $current) { throw ('Concurrent edit of '+$row.kind) }
        if (![SwitchIcdFile]::MoveFileExW($scratch, $row.live, 9)) {
            throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
    } finally { if ([IO.File]::Exists($scratch)) { [IO.File]::Delete($scratch) } }
    if ((Digest $row.live) -ne $target) { throw ('Readback failed for '+$row.kind) }
}
$lock = [IO.File]::Open((Join-Path $dir 'select.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    foreach ($row in $plan.files) {
        $live = Digest $row.live
        if ($live -ne $row.before -and $live -ne $row.after) { throw ('Selection has changed outside this transaction: '+$row.kind) }
        $backup = Join-Path $dir ('old-'+$row.kind+'.json')
        if (![IO.File]::Exists($backup) -and $Mode -ne 'Status') {
            if ($live -ne $row.before) { throw ('Original snapshot unavailable: '+$row.kind) }
            Copy-Durable $row.live $backup
        }
        if ([IO.File]::Exists($backup) -and (Digest $backup) -ne $row.before) { throw ('Invalid original snapshot: '+$row.kind) }
    }
    if ($Mode -eq 'Activate') {
        $gpu = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_ProblemCode).Data
        if ($gpu -ne 0) { throw 'GM20B is not ready' }
        foreach ($group in $plan.groups.PSObject.Properties) {
            $g = $group.Value
            if ((Digest (Join-Path $dir $g.file)) -ne $g.sha256 -or (Digest $g.icd) -ne $g.icd_sha256 -or
                (Get-AuthenticodeSignature -LiteralPath $g.icd).Status -ne 'Valid') { throw ('Invalid ICD or manifest for '+$group.Name) }
        }
        foreach ($item in $plan.original_manifest_hashes.PSObject.Properties) {
            if ((Digest $item.Name) -ne $item.Value) { throw ('Previous driver manifest changed: '+$item.Name) }
        }
    }
    if ($Mode -ne 'Status') {
        try { foreach ($row in $plan.files) { SelectFile $row ($Mode -eq 'Activate') } }
        catch {
            $cause = $_
            if ($Mode -eq 'Activate') { foreach ($row in $plan.files) { SelectFile $row $false } }
            throw $cause
        }
    }
    $actual = @(foreach ($row in $plan.files) { @{kind=$row.kind;sha256=(Digest $row.live)} })
    @{mode=$Mode;utc=[DateTime]::UtcNow.ToString('o');files=$actual} | ConvertTo-Json -Compress -Depth 5
} finally { $lock.Dispose() }
