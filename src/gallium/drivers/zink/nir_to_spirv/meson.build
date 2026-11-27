# Copyright © 2026 Valve Corporation
# SPDX-License-Identifier: MIT

inc_ntv = include_directories('.')

files_ntv = files(
  'nir_to_spirv.c',
  'spirv_builder.c',
)

idep_ntv = declare_dependency(
  sources : [files_ntv],
  include_directories : [inc_util, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [idep_nir_headers, idep_mesautil],
)

if with_tests
  test('zink_descriptor_binding', executable(
    'test_zink_descriptor_binding',
    files('descriptor_binding_test.c'),
    include_directories : [inc_include],
    dependencies : [idep_ntv, idep_nir],
  ), suite : ['zink'])
endif
