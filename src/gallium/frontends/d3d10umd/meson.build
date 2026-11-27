inc_d3d10umd = include_directories('.')
_c_args_d3d10umd = []

if cc.get_argument_syntax() == 'gcc'
  _c_args_d3d10umd += '-Wno-unknown-pragmas'
endif

_d3d10_kmt_sources = []
if d3d10_backend == 'gdi'
  # These thunks emulate an adapter for D3D_DRIVER_TYPE_SOFTWARE. A hardware
  # UMD must use the Windows runtime and must not export the emulation.
  _d3d10_kmt_sources += files('D3DKMT.cpp')
else
  _c_args_d3d10umd += '-DD3D10_UMD_HARDWARE'
  _d3d10_kmt_sources += files('RuntimeMemory.cpp')
endif

libd3d10umd = static_library(
  'd3d10umd',
  files(
    'Adapter.cpp',
    'Debug.cpp',
    'Device.cpp',
    'Draw.cpp',
    'DxgiFns.cpp',
    'Format.cpp',
    'InputAssembly.cpp',
    'OutputMerger.cpp',
    'Query.cpp',
    'Rasterizer.cpp',
    'Resource.cpp',
    'Shader.cpp',
    'ShaderDump.cpp',
    'ShaderParse.c',
    'ShaderTGSI.c',
    'Trace.cpp',
  ),
  _d3d10_kmt_sources,
  c_args : _c_args_d3d10umd,
  cpp_args : _c_args_d3d10umd,
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_winddk,
  ],
  dependencies: [idep_mesautilformat, cc.find_library('advapi32')],
)
