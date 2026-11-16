# SPDX-License-Identifier: MIT
#
# tgd3d: Direct3D 10 WDDM user-mode driver on Vulkan (NVK, Tegra GM20B).
# No Gallium, Zink or software rasterizer is linked.

dxbc_spv = subproject(
  'dxbc-spirv',
  default_options : ['enable_sm3=false', 'enable_tests=false', 'enable_tools=false',
                     'warning_level=1'],
)
dep_dxbc_spv = dxbc_spv.get_variable('dxbc_spv_dep')

tgd3d_files = files(
  'tgd3d_blit.cpp',
  'tgd3d_clear.cpp',
  'tgd3d_cmd.cpp',
  'tgd3d_ddi11.cpp',
  'tgd3d_device.cpp',
  'tgd3d_draw.cpp',
  'tgd3d_dxgi.cpp',
  'tgd3d_entry.cpp',
  'tgd3d_format.cpp',
   'tgd3d_offer.cpp',
  'tgd3d_query.cpp',
  'tgd3d_resource.cpp',
  'tgd3d_runtime.cpp',
  'tgd3d_runtime_queue.cpp',
  'tgd3d_shader.cpp',
  'tgd3d_shader_cache.cpp',
  'tgd3d_state.cpp',
  'tgd3d_trace.cpp',
  'tgd3d_upload.cpp',
  'tgd3d_vk.cpp',
)

tgd3d_inc = [inc_include, inc_src, inc_winddk]
tgd3d_args = ['-DDXBC_SPV_ENABLE_SM5', '-DDXBC_SPV_ENABLE_SPIRV', '-DNOMINMAX']
tgd3d_deps = [dep_dxbc_spv, idep_mesautil, cpp.find_library('advapi32')]
tgd3d_link_args = cc.get_argument_syntax() == 'gcc' ? ['-static', '-static-libgcc', '-static-libstdc++'] : []

# The shipped driver: loads the NVK ICD installed beside it.
libtgd3d = shared_library(
  get_option('tgd3d-dll-name'),
  tgd3d_files,
  vs_module_defs : 'tgd3d.def',
  include_directories : tgd3d_inc,
  cpp_args : tgd3d_args,
  dependencies : tgd3d_deps,
  link_args : tgd3d_link_args,
  name_prefix : '',
  install : true,
)

# Controlled test build of the same sources: any Vulkan 1.3 driver through
# the loader, runtime allocations imported by host pointer. Never installed.
if with_tests
  libtgd3d_test = shared_library(
    'tgd3d_test',
    tgd3d_files,
    vs_module_defs : 'tgd3d.def',
    include_directories : tgd3d_inc,
    cpp_args : tgd3d_args + ['-DTGD3D_GENERIC_VULKAN'],
    dependencies : tgd3d_deps,
    link_args : tgd3d_link_args,
    name_prefix : '',
    install : false,
  )
  subdir('tests')
endif
