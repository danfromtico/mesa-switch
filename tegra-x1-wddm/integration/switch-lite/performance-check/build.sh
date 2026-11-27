#!/bin/sh
# BUILD_TESTS=1 builds the native device-I/O fixture test.
set -eu
root=${MINGW_SYSROOT:?pass the mingw-w64 header sysroot}
src=$(CDPATH= cd -- "$(dirname "$0")" && pwd -P)
gpu_root=$(CDPATH= cd -- "$src/../../.." && pwd -P)
cc=${CLANG:-clang}
link=${LLD_LINK:-lld-link}
export LLVM_DLLTOOL=${LLVM_DLLTOOL:-llvm-dlltool}
mkdir -p ui
"$cc" --target=aarch64-w64-windows-gnu --sysroot="$root" -ffreestanding -fasynchronous-unwind-tables -fno-stack-protector -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -I "$gpu_root" -c "$src/performance-check.c" -o ui/performance-check.obj
python3 - <<'PY'
import os, subprocess
from pathlib import Path
libs={
 'msvcrt':'memcpy memset',
 'kernel32':'CompareStringOrdinal ExitProcess GetModuleHandleW lstrlenW lstrcmpW CreateFileW DeviceIoControl CloseHandle GetLastError CreateThread GetModuleFileNameW GetCommandLineW GetStdHandle WriteFile LocalFree MulDiv FlushFileBuffers',
 'user32':'wsprintfA wsprintfW GetClientRect ScreenToClient GetPointerInfo InvalidateRect BeginPaint FillRect EndPaint DrawTextW DefWindowProcW KillTimer PostQuitMessage LoadCursorW RegisterClassW GetSystemMetrics CreateWindowExW SetTimer UpdateWindow GetMessageW TranslateMessage DispatchMessageW SendMessageW EnableWindow PostMessageW DestroyWindow MoveWindow',
 'gdi32':'TextOutW GetStockObject SetBkMode SelectObject SetTextColor SetDCBrushColor Ellipse CreateFontW DeleteObject SetMapMode SetWindowExtEx SetViewportExtEx',
 'shell32':'ShellExecuteW CommandLineToArgvW',
 'advapi32':'RegOpenKeyExW RegQueryValueExW RegCloseKey',
 'setupapi':'SetupDiGetClassDevsW SetupDiEnumDeviceInfo SetupDiGetDeviceInstanceIdW SetupDiDestroyDeviceInfoList',
 'cfgmgr32':'CM_Get_DevNode_Status'}
for name,s in libs.items():
 d=Path('ui')/(name+'.def');d.write_text('LIBRARY '+name+'.dll\nEXPORTS\n'+'\n'.join(s.split())+'\n')
 subprocess.run([os.environ['LLVM_DLLTOOL'],'-m','arm64','-d',str(d),'-l','ui/'+name+'.lib'],check=True)
PY
"$link" /nologo /machine:arm64 /subsystem:windows,10.0 /release /entry:start /include:_load_config_used /nodefaultlib /opt:ref /timestamp:0 /out:ui/performance-check.exe ui/performance-check.obj ui/*.lib
if [ "${BUILD_TESTS:-0}" = 1 ]; then
 "${HOST_CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -I "$gpu_root" "$src/tests/test-gpu-pass.c" -o ui/test-gpu-pass
 ./ui/test-gpu-pass
 "$cc" --target=aarch64-w64-windows-gnu --sysroot="$root" -ffreestanding -fasynchronous-unwind-tables -fno-stack-protector -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -I "$gpu_root" -c "$src/tests/test-gpu-json.c" -o ui/test-gpu-json.obj
 "$link" /nologo /machine:arm64 /subsystem:console,10.0 /release /entry:start /include:_load_config_used /nodefaultlib /opt:ref /timestamp:0 /out:ui/test-gpu-json.exe ui/test-gpu-json.obj ui/*.lib
fi
