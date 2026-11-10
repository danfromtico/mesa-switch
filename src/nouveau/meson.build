# Copyright © 2021 Ilia Mirkin
# SPDX-License-Identifier: MIT

subdir('headers')
if with_platform_windows
  idep_nouveau_ws = declare_dependency()
else
  subdir('drm')
  subdir('winsys')
endif
if with_platform_switch
  subdir('horizon')
endif
if with_nouveau_vk
  subdir('rust')
  subdir('nil')
  subdir('compiler')
endif
if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

if with_nouveau_vk
  # nv_cubin.c/nv_fatbin.c need libelf. It only backs VK_NVX_binary_import,
  # which is itself gated behind the DLSS experimental flag, so skip it when
  # libelf is unavailable (e.g. the Switch cross target has no such portlib).
  if dep_elf.found()
    subdir('cubin')
  endif
  subdir('mme')
  subdir('vulkan')
endif
