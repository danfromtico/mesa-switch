/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_instance.h"

#include "nvk_entrypoints.h"
#include "nvk_physical_device.h"
#include "nvk_drirc.h"
#include "nvkmd/nvkmd.h"

#include "vk_common_entrypoints.h"
#include "vk_internal_exts.h"
#include "vulkan/wsi/wsi_common.h"

#include "util/build_id.h"
#include "util/detect_os.h"
#include "util/hex.h"
#include "util/mesa-blake3.h"
#include "util/os_misc.h"
#include "util/u_debug.h"

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   uint32_t version_override = vk_get_version_override();
   *pApiVersion = version_override ? version_override :
                  VK_MAKE_VERSION(1, 4, VK_HEADER_VERSION);

   return VK_SUCCESS;
}

static const struct vk_instance_extension_table instance_extensions = {
#ifdef NVK_USE_WSI_PLATFORM
   .KHR_get_surface_capabilities2 = true,
   .KHR_surface = true,
   .KHR_surface_maintenance1 = true,
   .KHR_surface_protected_capabilities = true,
   .EXT_surface_maintenance1 = true,
   .EXT_swapchain_colorspace = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
   .KHR_xcb_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
   .KHR_xlib_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
   .EXT_acquire_xlib_display = true,
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
   .KHR_win32_surface = true,
#endif
#ifdef VK_USE_PLATFORM_DISPLAY_KHR
   .KHR_display = true,
   .KHR_get_display_properties2 = true,
   .EXT_direct_mode_display = true,
   .EXT_display_surface_counter = true,
   .EXT_acquire_drm_display = true,
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
   .EXT_headless_surface = true,
#endif
#ifdef VK_USE_PLATFORM_VI_NN
   .NN_vi_surface = true,
#endif
   .KHR_device_group_creation = true,
   .KHR_external_fence_capabilities = true,
   .KHR_external_memory_capabilities = true,
   .KHR_external_semaphore_capabilities = true,
   .KHR_get_physical_device_properties2 = true,
   .EXT_debug_report = true,
   .EXT_debug_utils = true,
};

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                         uint32_t *pPropertyCount,
                                         VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &instance_extensions, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                     VkLayerProperties *pProperties)
{
   if (pPropertyCount == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   *pPropertyCount = 0;
   return VK_SUCCESS;
}

#ifdef _WIN32
VkResult nvk_enumerate_windows_physical_devices(struct vk_instance *instance);
#endif

#ifdef __SWITCH__
static VkResult
nvk_enumerate_switch_physical_devices(struct vk_instance *_instance)
{
   struct nvk_instance *instance = (struct nvk_instance *)_instance;
   struct nvkmd_pdev *nvkmd;

   VkResult result =
      nvkmd_try_create_pdev_for_switch(&instance->vk.base,
                                       instance->debug_flags, &nvkmd);
   if (result != VK_SUCCESS)
      return result;

   struct vk_physical_device *pdev;
   result = nvk_create_physical_device_from_nvkmd(instance, nvkmd, &pdev);
   if (result != VK_SUCCESS)
      return result;

   list_addtail(&pdev->link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}
#endif

static void
nvk_init_debug_flags(struct nvk_instance *instance)
{
   const struct debug_control flags[] = {
      { "push_dump", NVK_DEBUG_PUSH_DUMP },
      { "push", NVK_DEBUG_PUSH_DUMP },
      { "push_sync", NVK_DEBUG_PUSH_SYNC },
      { "zero_memory", NVK_DEBUG_ZERO_MEMORY },
      { "trash_memory", NVK_DEBUG_TRASH_MEMORY },
      { "vm", NVK_DEBUG_VM },
      { "no_cbuf", NVK_DEBUG_NO_CBUF },
      { "edb_bview", NVK_DEBUG_FORCE_EDB_BVIEW },
      { "gart", NVK_DEBUG_FORCE_GART },
      { "coherent", NVK_DEBUG_FORCE_COHERENT },
      { "no_compression", NVK_DEBUG_NO_COMPRESSION },
      { "log", NVK_DEBUG_LOG },
      { NULL, 0 },
   };

   instance->debug_flags = parse_debug_string(os_get_option("NVK_DEBUG"), flags);
   instance->vk.enable_debug_logging = instance->debug_flags & NVK_DEBUG_LOG;
}

static void
nvk_init_experimental_flags(struct nvk_instance *instance)
{
   const struct debug_control flags[] = {
      { "dlss", NVK_EXPERIMENTAL_DLSS },
      { "dlss_backwards_compat", NVK_EXPERIMENTAL_DLSS_BACK_COMPAT },
      { NULL, 0 },
   };

   instance->experimental_flags = parse_debug_string(os_get_option("NVK_EXPERIMENTAL"), flags);
}

static void
nvk_init_dri_options(struct nvk_instance *instance)
{
   nvk_parse_dri_options(&instance->drirc,
                         &(driConfigFileParseParams){
                            .driverName = "nvk",
                            .applicationName = instance->vk.app_info.app_name,
                            .applicationVersion = instance->vk.app_info.app_version,
                            .engineName = instance->vk.app_info.engine_name,
                            .engineVersion = instance->vk.app_info.engine_version,
                         });

   if (instance->drirc.debug.zero_vram)
      instance->debug_flags |= NVK_DEBUG_ZERO_MEMORY;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkInstance *pInstance)
{
   struct nvk_instance *instance;
   VkResult result;

   if (pAllocator == NULL)
      pAllocator = vk_default_allocator();

   instance = vk_alloc(pAllocator, sizeof(*instance), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table,
                                               &nvk_instance_entrypoints,
                                               true);
   vk_instance_dispatch_table_from_entrypoints(&dispatch_table,
                                               &wsi_instance_entrypoints,
                                               false);

   result = vk_instance_init(&instance->vk, &instance_extensions,
                             &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   nvk_init_debug_flags(instance);
   nvk_init_experimental_flags(instance);
   nvk_init_dri_options(instance);

   instance->allow_embedded_gm20b = false;
#ifdef _WIN32
   const struct nvk_embedded_instance_info *embedded =
      vk_find_struct_const(pCreateInfo->pNext, NVK_EMBEDDED_INSTANCE_INFO_MESA);
   instance->allow_embedded_gm20b = embedded && embedded->allow_experimental_device;
#endif

#ifdef __SWITCH__
   instance->vk.physical_devices.enumerate =
      nvk_enumerate_switch_physical_devices;
#elif defined(_WIN32)
   instance->vk.physical_devices.enumerate =
      nvk_enumerate_windows_physical_devices;
#else
   instance->vk.physical_devices.try_create_for_drm =
      nvk_create_drm_physical_device;
#endif
   instance->vk.physical_devices.destroy = nvk_physical_device_destroy;

#ifdef HAVE_DL_ITERATE_PHDR
   const struct build_id_note *note =
      build_id_find_nhdr_for_addr(nvk_CreateInstance);
   if (!note) {
      result = vk_errorf(NULL, VK_ERROR_INITIALIZATION_FAILED,
                         "Failed to find build-id");
      goto fail_init;
   }

   unsigned build_id_len = build_id_length(note);
   if (build_id_len < BUILD_ID_EXPECTED_HASH_LENGTH) {
      result = vk_errorf(NULL, VK_ERROR_INITIALIZATION_FAILED,
                        "build-id too short.  It needs to be a SHA");
      goto fail_init;
   }

   STATIC_ASSERT(sizeof(instance->driver_build_sha) == BLAKE3_KEY_LEN);
   copy_build_id_to_sha1(instance->driver_build_sha, note);
#else
   /* Without dl_iterate_phdr, take the build ID the build system supplied.
    * Falling back to the package version keys the shader cache on the Mesa
    * release alone, so two drivers built from different source share it.
    */
   STATIC_ASSERT(sizeof(instance->driver_build_sha) == BLAKE3_KEY_LEN);
   memset(instance->driver_build_sha, 0, BLAKE3_KEY_LEN);
#ifdef NVK_BUILD_ID_OVERRIDE
   mesa_hex_to_bytes(instance->driver_build_sha, NVK_BUILD_ID_OVERRIDE,
                     MIN2(strlen(NVK_BUILD_ID_OVERRIDE) / 2,
                          (size_t)BLAKE3_KEY_LEN));
#else
   const char fallback_id[] = "nvk-" PACKAGE_VERSION;
   memcpy(instance->driver_build_sha, fallback_id,
          MIN2(sizeof(fallback_id) - 1, (size_t)BLAKE3_KEY_LEN));
#endif
#endif

   *pInstance = nvk_instance_to_handle(instance);
   return VK_SUCCESS;

#ifdef HAVE_DL_ITERATE_PHDR
fail_init:
   vk_instance_finish(&instance->vk);
#endif
fail_alloc:
   vk_free(pAllocator, instance);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyInstance(VkInstance _instance,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);

   if (!instance)
      return;

   driDestroyOptionCache(&instance->drirc.options);
   driDestroyOptionInfo(&instance->drirc.available_options);

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumeratePhysicalDevices(VkInstance instance,
                             uint32_t *pPhysicalDeviceCount,
                             VkPhysicalDevice *pPhysicalDevices)
{
   return vk_common_EnumeratePhysicalDevices(instance, pPhysicalDeviceCount,
                                             pPhysicalDevices);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nvk_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);
   return vk_instance_get_proc_addr(&instance->vk,
                                    &nvk_instance_entrypoints,
                                    pName);
}

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return nvk_GetInstanceProcAddr(instance, pName);
}
