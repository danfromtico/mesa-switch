# SPDX-License-Identifier: MIT
# Adapted from the existing .158/.161 GPU trial workflow; see tool-provenance.json.
param([Parameter(Mandatory)][string]$TraceName,
      [Parameter(Mandatory)][string]$FileName)
$ErrorActionPreference='Stop'
$key=Join-Path 'HKLM:\SYSTEM\CurrentControlSet\Control\WMI\Autologger' $TraceName
$traceDir=Split-Path -Parent $FileName
$traceLeaf=Split-Path -Leaf $FileName
if((Test-Path $key) -or (Test-Path -LiteralPath $FileName) -or
    (Get-ChildItem -LiteralPath $traceDir -Filter "$traceLeaf.*" -File)) {
    throw 'A display-start trace is already staged'
}
New-Item -Path $key | Out-Null
try {
    New-ItemProperty $key Guid -PropertyType String -Value ('{' + [Guid]::NewGuid().ToString() + '}') | Out-Null
    New-ItemProperty $key FileName -PropertyType String -Value $FileName | Out-Null
    New-ItemProperty $key ClockType -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $key LogFileMode -PropertyType DWord -Value 0x122 | Out-Null
    New-ItemProperty $key MaxFileSize -PropertyType DWord -Value 64 | Out-Null
    New-ItemProperty $key FileMax -PropertyType DWord -Value 4 | Out-Null
    New-ItemProperty $key BufferSize -PropertyType DWord -Value 64 | Out-Null
    New-ItemProperty $key FlushTimer -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $key MinimumBuffers -PropertyType DWord -Value 16 | Out-Null
    New-ItemProperty $key MaximumBuffers -PropertyType DWord -Value 128 | Out-Null
    $provider=Join-Path $key '{802ec45a-1e99-4b83-9920-87c98277ba9d}'
    New-Item -Path $provider | Out-Null
    New-ItemProperty $provider Enabled -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $provider EnableLevel -PropertyType DWord -Value 5 | Out-Null
    New-ItemProperty $provider MatchAnyKeyword -PropertyType QWord -Value 0x40518001 | Out-Null
    $scheduler=Join-Path $key '{22EAA5DB-0130-4179-B2DE-6D7DF0607024}'
    New-Item -Path $scheduler | Out-Null
    New-ItemProperty $scheduler Enabled -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $scheduler EnableLevel -PropertyType DWord -Value 4 | Out-Null
    New-ItemProperty $scheduler MatchAnyKeyword -PropertyType QWord -Value 1 | Out-Null
    # UMD identity uses a separate small session started before native entry.
    # High-volume kernel traces must not share its buffer/loss accounting.
    # KMD first failure and execution/fetch-boundary records (events 1-3).
    $kmd=Join-Path $key '{c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0}'
    New-Item -Path $kmd | Out-Null
    New-ItemProperty $kmd Enabled -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $kmd EnableLevel -PropertyType DWord -Value 4 | Out-Null
    New-ItemProperty $kmd MatchAnyKeyword -PropertyType QWord -Value 1 | Out-Null
    # DXGI object/event records plus API return values; exclude journal entries.
    # DWM composition/scheduling at informational level includes device-state
    # HRESULTs without verbose frame/overlay detail. Templates were read from
    # this OS and are retained alongside the ETL.
    $dxgi=Join-Path $key '{ca11c036-0102-4a2d-a6ad-f03cfed5d3c9}'
    New-Item -Path $dxgi | Out-Null
    New-ItemProperty $dxgi Enabled -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $dxgi EnableLevel -PropertyType DWord -Value 4 | Out-Null
    New-ItemProperty $dxgi MatchAnyKeyword -PropertyType QWord -Value 0x4000000000000003 | Out-Null
    $dwm=Join-Path $key '{9e9bba3c-2e38-40cb-99f4-9e8281425164}'
    New-Item -Path $dwm | Out-Null
    New-ItemProperty $dwm Enabled -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $dwm EnableLevel -PropertyType DWord -Value 4 | Out-Null
    New-ItemProperty $dwm MatchAnyKeyword -PropertyType QWord -Value 0x81 | Out-Null
    # Raw D3DKMT failure journal before the D3D runtime masks the HRESULT.
    $d3d11=Join-Path $key '{db6f6ddb-ac77-4e88-8253-819df9bbf140}'
    New-Item -Path $d3d11 | Out-Null
    New-ItemProperty $d3d11 Enabled -PropertyType DWord -Value 1 | Out-Null
    New-ItemProperty $d3d11 EnableLevel -PropertyType DWord -Value 4 | Out-Null
    New-ItemProperty $d3d11 MatchAnyKeyword -PropertyType QWord -Value 0x800 | Out-Null
    New-ItemProperty $key Start -PropertyType DWord -Value 1 | Out-Null
} catch {
    Remove-Item -Path $key -Recurse -Force
    throw
}
