# Copyright © 2021 VMware, Inc
# Copyright © 2018 Intel Corporation
# SPDX-License-Identifier: MIT

_d3d10_source = 'd3d10_gdi.c'
_d3d10_def = 'd3d10.def.in'
_d3d10_link = [libwsgdi]
_d3d10_deps = [driver_swrast]
if d3d10_backend == 'nvk'
  _d3d10_source = 'd3d10_nvk.c'
  _d3d10_def = 'd3d10_nvk.def.in'
  _d3d10_link = []
  _d3d10_deps = [driver_zink, idep_xmlconfig]
endif

libgallium_d3d10_def = custom_target(
  'd3d10.def',
  input: _d3d10_def,
  output : 'd3d10.def',
  command : gen_vs_module_defs_normal_command,
)

gallium_d3d10_name = get_option('gallium-d3d10-dll-name')
libgallium_d3d10 = shared_library(
  gallium_d3d10_name,
  [_d3d10_source],
  vs_module_defs : libgallium_d3d10_def,
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_d3d10umd, inc_gallium_winsys, inc_gallium_winsys_sw, inc_gallium_drivers, inc_winddk
  ],
  link_whole : [libd3d10umd],
  link_with : [
    libgallium, _d3d10_link
  ],
  dependencies : [
    dep_ws2_32, idep_nir, _d3d10_deps, idep_mesautil
  ],
  name_prefix: '',
  install : true,
)

if with_tests and d3d10_backend == 'nvk'
  test('d3d10_runtime_shared', executable(
    'test_d3d10_runtime_shared',
    files('tests/runtime_shared.cpp', '../../frontends/d3d10umd/RuntimeMemory.cpp'),
    include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_d3d10umd, inc_winddk],
    dependencies : [idep_mesautilformat],
  ), suite : ['d3d10'])
  test('d3d10_runtime_present', executable(
    'test_d3d10_runtime_present', files('tests/runtime_present.cpp'),
    include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_d3d10umd, inc_winddk],
    link_with : [libd3d10umd, libgallium],
    dependencies : [idep_nir, idep_mesautil, idep_mesautilformat, dep_ws2_32],
  ), suite : ['d3d10'])
  test('d3d10_runtime_primary', executable(
    'test_d3d10_runtime_primary',
    files('tests/runtime_primary.cpp', '../../frontends/d3d10umd/RuntimeMemory.cpp'),
    include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_d3d10umd, inc_winddk],
    dependencies : [idep_mesautilformat],
  ), suite : ['d3d10'])
  executable(
    'd3d10-trace-export',
    files('tests/trace_export.cpp'),
    include_directories : [inc_d3d10umd],
    dependencies : [cpp.find_library('advapi32')],
    link_args : cc.get_argument_syntax() == 'gcc' ? ['-municode'] : [],
  )
  # Explicitly run on a native-submission KMD; never a default Meson test.
  executable(
    'nvk_d3d_check',
    files('tests/nvk_native.cpp'),
    include_directories : [inc_src, inc_winddk],
  )
  executable(
    'nvk_d3d_runtime_check',
    files('tests/nvk_runtime.cpp'),
    dependencies : [cpp.find_library('d3d11'), cpp.find_library('dxgi')],
  )
  test(
    'd3d10_nvk_device',
    executable(
      'test_d3d10_nvk_device',
      files('tests/nvk_device.cpp'),
      include_directories : [inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_d3d10umd, inc_winddk],
      link_with : [libd3d10umd, libgallium],
      dependencies : [idep_nir, idep_mesautil, idep_mesautilformat, dep_ws2_32],
    ),
    suite : ['d3d10'],
  )
  test(
    'd3d10_nvk_entry',
    executable(
      'test_d3d10_nvk_entry',
      files('tests/nvk_entry.cpp'),
      include_directories : [inc_src, inc_winddk],
    ),
    args : [libgallium_d3d10.full_path()],
    depends : [libgallium_d3d10],
    suite : ['d3d10'],
  )
  test(
    'd3d10_nvk_adapter',
    executable(
      'test_d3d10_nvk_adapter',
      files('tests/nvk_adapter.c', 'd3d10_nvk.c'),
      include_directories : [inc_src, inc_gallium_drivers, inc_winddk, inc_d3d10umd],
    ),
    suite : ['d3d10'],
  )
endif

if with_tests and d3d10_backend == 'gdi'
  test(
    'd3d10',
    executable(
      'test_d3d10',
      files('tests/tri.cpp'),
      cpp_args : [cpp_msvc_compat_args],
      dependencies : [cpp.find_library('d3d11')],
      link_with : [libgallium_d3d10],
    ),
    suite : ['d3d10'],
  )
endif
