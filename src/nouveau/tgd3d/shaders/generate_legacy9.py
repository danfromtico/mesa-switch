#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate the embedded SHDR tokens and DDI signatures for the D3D9 color pipeline."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--fxc', required=True)
parser.add_argument('--runner', help='Wine when cross-compiling')
args = parser.parse_args()
compiler = [args.runner, args.fxc] if args.runner else [args.fxc]
lines = ['/* SPDX-License-Identifier: MIT', ' * Generated from legacy9_color.hlsl by generate_legacy9.py. */', '']
with tempfile.TemporaryDirectory() as working:
    work = Path(working)
    source = 'legacy9_color.hlsl'
    shutil.copyfile(here / source, work / source)
    for entry in ('color_vs', 'color_ps', 'texture_vs', 'texture_ps'):
        stage = entry.rsplit('_', 1)[1]
        subprocess.run([*compiler, '/nologo', '/Ges', '/O3', '/E', entry,
                        '/T', stage + '_4_0', '/Fo', 'color.cso', source], cwd=work, check=True)
        data = (work / 'color.cso').read_bytes()
        count, = struct.unpack_from('<I', data, 28)
        for offset in struct.unpack_from(f'<{count}I', data, 32):
            chunk, size = struct.unpack_from('<4sI', data, offset)
            body = data[offset+8:offset+8+size]
            if chunk in (b'SHDR', b'SHEX'):
                words = struct.unpack(f'<{size//4}I', body)
                lines.append(f'static constexpr UINT {entry}[] = {{')
                for i in range(0, len(words), 6):
                    lines.append('   ' + ', '.join(f'0x{x:08x}' for x in words[i:i+6]) + ',')
                lines.extend(['};', ''])
            elif chunk in (b'ISGN', b'OSGN'):
                direction = 'in' if chunk == b'ISGN' else 'out'
                entries, first = struct.unpack_from('<II', body)
                lines.append(f'static constexpr D3D10DDIARG_SIGNATURE_ENTRY {entry}_{direction}[] = {{')
                for i in range(entries):
                    _, _, system, _, register, masks = struct.unpack_from('<6I', body, first+24*i)
                    lines.append(f'   {{static_cast<D3D10_SB_NAME>({system}), {register}, 0x{masks & 15:x}}},')
                lines.extend(['};', ''])
(here / 'legacy9_color.h').write_text('\n'.join(lines), encoding='ascii')
