#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate a scoped WinPE registry update; never writes a physical disk itself.

Apply only to the verified offline Windows image. Existing session/per-app keys
are rejected, so restoration deletes only keys created by this package.
"""
import argparse
import json
import uuid
from pathlib import Path

TAG = 'TgdColdBoot-20261113'
ROOT = r'C:\ProgramData\SwitchWOA\ColdBootTrace-20261113'
SYS = r'HKEY_LOCAL_MACHINE\TgdBootSYSTEM\ControlSet001'
SOFT = r'HKEY_LOCAL_MACHINE\TgdBootSOFTWARE'
AUTO = SYS + r'\Control\WMI\Autologger'
GLOBAL = SYS + r'\Control\WMI\GlobalLogger'
DUMPS = SOFT + r'\Microsoft\Windows\Windows Error Reporting\LocalDumps'


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    keys = {}
    roots = []
    sessions = []

    def session(key, name, file, size, providers=()):
        roots.append(key)
        values = {'Start': (4, 1), 'FileName': (1, ROOT + '\\' + file),
                  'ClockType': (4, 1), 'LogFileMode': (4, 2),
                  'MaxFileSize': (4, size), 'FileMax': (4, 4),
                  'BufferSize': (4, 64), 'FlushTimer': (4, 1),
                  'MinimumBuffers': (4, 16), 'MaximumBuffers': (4, 64)}
        if key != GLOBAL:
            values['Guid'] = (1, '{' + str(uuid.uuid4()) + '}')
        else:
            # SDK 10.0.26100: PROCESS|THREAD|IMAGE_LOAD|CSWITCH. The boot
            # GlobalLogger contract requires REG_BINARY, not REG_DWORD.
            values['EnableKernelFlags'] = (3, '17000000')
        keys[key] = values
        for guid, level, keyword in providers:
            keys[key + '\\' + guid] = {
                'Enabled': (4, 1), 'EnableLevel': (4, level),
                'MatchAnyKeyword': (11, keyword), 'MatchAllKeyword': (11, 0)}
        sessions.append({'name': name, 'file': file, 'size_mib': size,
                         'providers': list(providers)})

    # Standard boot kernel logger: no sampling, disk/file/registry traffic.
    session(GLOBAL, 'NT Kernel Logger', 'kernel.etl', 64)
    session(AUTO + '\\' + TAG + '-UMD', TAG + '-UMD', 'tgd3d.etl', 32, [
        ('{5f0c4c6e-7a0b-4b52-9d3a-2f6d1e8b4a11}', 4, 1)])
    # Same bounded graphics provider selection as Start-DxgBootTrace.ps1.
    session(AUTO + '\\' + TAG + '-Graphics', TAG + '-Graphics', 'graphics.etl', 64, [
        ('{802ec45a-1e99-4b83-9920-87c98277ba9d}', 5, 0x40518001),
        ('{22eaa5db-0130-4179-b2de-6d7df0607024}', 4, 1),
        ('{c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0}', 4, 1),
        ('{ca11c036-0102-4a2d-a6ad-f03cfed5d3c9}', 4, 0x4000000000000003),
        ('{9e9bba3c-2e38-40cb-99f4-9e8281425164}', 4, 0x81),
        ('{db6f6ddb-ac77-4e88-8253-819df9bbf140}', 4, 0x800)])
    # GUIDs verified in this target's recovered SOFTWARE publisher registry.
    session(AUTO + '\\' + TAG + '-Loader', TAG + '-Loader', 'loader.etl', 32, [
        ('{b059b83f-d946-4b13-87ca-4292839dc2f2}', 5, 0),
        ('{dbe9b383-7cf3-4331-91cc-a3cb16a3b538}', 4, 0),
        ('{4ee76bd8-3cf4-44a0-a0ac-3937643e37a3}', 4, 0)])
    for app in ['dwm.exe', 'LogonUI.exe']:
        key = DUMPS + '\\' + app
        roots.append(key)
        keys[key] = {'DumpFolder': (2, ROOT + '\\Dumps\\' + app),
                     'DumpType': (4, 2), 'DumpCount': (4, 2)}

    def quoted(s):
        return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

    def value(t, v):
        if t == 1:
            return quoted(v)
        if t == 4:
            return 'dword:%08x' % v
        raw = ((v + '\0').encode('utf-16le') if t == 2 else
               int(v).to_bytes(8, 'little') if t == 11 else bytes.fromhex(v))
        kind = 'hex' if t == 3 else 'hex(%x)' % t
        return kind + ':' + ','.join('%02x' % b for b in raw)

    text = 'Windows Registry Editor Version 5.00\r\n\r\n'
    for key, vals in keys.items():
        text += '[' + key + ']\r\n'
        # Start last in the import too; no session runs against these offline hives.
        for n, (t, v) in sorted(vals.items(), key=lambda item: item[0] == 'Start'):
            text += quoted(n) + '=' + value(t, v) + '\r\n'
        text += '\r\n'
    (output / 'enable-offline.reg').write_bytes(text.encode('utf-16'))
    plan = {'tag': TAG, 'directory': ROOT, 'control_set': 1, 'sessions': sessions,
            'absent_before': roots, 'keys': keys,
            'restore_precondition': 'all affected keys absent before installation',
            'gpu_components_changed': False}
    (output / 'capture-plan.json').write_text(json.dumps(plan, indent=2) + '\n')
    remove = 'Windows Registry Editor Version 5.00\r\n\r\n'
    remove += '\r\n\r\n'.join('[-' + key + ']' for key in roots) + '\r\n'
    (output / 'disable-offline.reg').write_bytes(remove.encode('utf-16'))

    # reg.exe performs supported transaction-log recovery when loading the hives.
    # This script runs only in the reviewed copy-on-write WinPE image.
    lines = ['@echo off', 'setlocal', 'set "drive=%~1"',
             'set "target=%drive%:\\ProgramData\\SwitchWOA\\ColdBootTrace-20261113"',
             'if not exist "%drive%:\\Windows\\System32\\config\\SYSTEM" exit /b 2',
             'reg query HKLM\\TgdBootSYSTEM >nul 2>&1',
             'if not errorlevel 1 exit /b 3',
             'reg query HKLM\\TgdBootSOFTWARE >nul 2>&1',
             'if not errorlevel 1 exit /b 3',
             'reg load HKLM\\TgdBootSYSTEM "%drive%:\\Windows\\System32\\config\\SYSTEM"',
             'if errorlevel 1 exit /b 4',
             'reg load HKLM\\TgdBootSOFTWARE "%drive%:\\Windows\\System32\\config\\SOFTWARE"',
             'if errorlevel 1 goto fail',
             'reg query HKLM\\TgdBootSYSTEM\\Select',
             'reg query HKLM\\TgdBootSYSTEM\\ControlSet001\\Services\\tegragpurender\\AllowNativeBoot',
             'if errorlevel 1 goto fail']
    for key in roots:
        lines += ['reg query "' + key + '" >nul 2>&1', 'if not errorlevel 1 goto fail']
    lines += ['if exist "%target%" goto fail', 'mkdir "%target%"',
              'if errorlevel 1 goto fail',
              'X:\\tgd3d-update\\setacl.exe "%target%" traverse',
              'if errorlevel 1 goto fail',
              'mkdir "%target%\\Dumps"', 'if errorlevel 1 goto fail',
              'X:\\tgd3d-update\\setacl.exe "%target%\\Dumps" traverse',
              'if errorlevel 1 goto fail',
              'mkdir "%target%\\Dumps\\dwm.exe"', 'if errorlevel 1 goto fail',
              'X:\\tgd3d-update\\setacl.exe "%target%\\Dumps\\dwm.exe" dwm',
              'if errorlevel 1 goto fail',
              'mkdir "%target%\\Dumps\\LogonUI.exe"', 'if errorlevel 1 goto fail',
              'reg import X:\\tgd3d-update\\enable-offline.reg', 'if errorlevel 1 goto fail']
    for name in ['capture-plan.json', 'Restore.ps1', 'README.md']:
        lines += [f'copy /y "X:\\tgd3d-update\\{name}" "%target%\\{name}" >nul',
                  'if errorlevel 1 goto fail']
    for key in roots:
        lines += ['reg query "' + key + '" /s', 'if errorlevel 1 goto fail']
    lines += ['X:\\tgd3d-update\\setacl.exe "%target%\\Dumps\\LogonUI.exe" system', 'if errorlevel 1 goto fail',
              'reg unload HKLM\\TgdBootSOFTWARE', 'if errorlevel 1 goto fail',
              'reg unload HKLM\\TgdBootSYSTEM', 'if errorlevel 1 exit /b 7',
              'echo COLD_BOOT_CAPTURE_CONFIGURED', 'exit /b 0', ':fail',
              'reg unload HKLM\\TgdBootSOFTWARE', 'reg unload HKLM\\TgdBootSYSTEM',
              'echo CONFIGURATION_FAILED_DO_NOT_APPLY_COW', 'exit /b 8']
    (output / 'configure.cmd').write_bytes(('\r\n'.join(lines) + '\r\n').encode('ascii'))
    restore = '''# SPDX-License-Identifier: MIT
# Run elevated on the target Windows; leaves evidence and GPU settings intact.
# Offline use: load hives as HKLM\\TgdBootSYSTEM/SOFTWARE and pass -Offline.
param([switch]$Offline)
$ErrorActionPreference = 'Stop'
$plan = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'capture-plan.json') -Raw | ConvertFrom-Json
foreach ($key in $plan.absent_before) {
    $path = $key
    if (-not $Offline) {
        $path = $path.Replace('TgdBootSYSTEM\\ControlSet001', 'SYSTEM\\CurrentControlSet').Replace('TgdBootSOFTWARE', 'SOFTWARE')
    }
    $path = 'Registry::' + $path
    if (!(Test-Path -LiteralPath $path)) { continue }
    # A later unrelated owner must not be removed accidentally.
    $v = Get-ItemProperty -LiteralPath $path
    $file = if ($key -like '*\\LocalDumps\\*') { $v.DumpFolder } else { $v.FileName }
    if (!$file -or !$file.StartsWith($plan.directory + '\\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Capture key changed ownership: $path"
    }
    if (!$Offline -and $key -notlike '*\\LocalDumps\\*') {
        $name = if ($key -like '*\\GlobalLogger') { 'NT Kernel Logger' } else { Split-Path $key -Leaf }
        & logman.exe stop $name -ets
        if ($LASTEXITCODE -ne 0) { Write-Warning "Session $name was not stopped; inspect status before collecting its ETL." }
    }
    Remove-Item -LiteralPath $path -Recurse -Force
}
Write-Output 'Capture removed; evidence retained; native tgd3d startup unchanged.'
'''
    (output / 'Restore.ps1').write_text(restore)
    (output / 'README.md').write_text((Path(__file__).parent / 'README.md').read_text())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    generate(parser.parse_args().output)
