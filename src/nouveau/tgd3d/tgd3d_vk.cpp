/* SPDX-License-Identifier: MIT */
#include "tgd3d_vk.h"
#include "util/disk_cache.h"
#include "vulkan/util/vk_internal_exts.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace tgd3d {

namespace {

struct ExtensionSet {
   std::vector<VkExtensionProperties> props;
   bool has(const char *name) const
   {
      for (const auto &p : props)
         if (!strcmp(p.extensionName, name))
            return true;
      return false;
   }
};

/* The feature chain is queried once and replayed with only the bits this
 * driver uses, so enabling a feature never silently widens the set of
 * dynamic state a draw requires. */
struct FeatureChain {
   VkPhysicalDeviceFeatures2 core = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
   VkPhysicalDeviceVulkan11Features v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
   VkPhysicalDeviceVulkan12Features v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
   VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
   VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
   VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
   VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
   VkPhysicalDeviceDepthClipEnableFeaturesEXT depthClip = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
   VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisor = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
   VkPhysicalDevice4444FormatsFeaturesEXT fmt4444 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT};

   void link(bool with_divisor, bool with_4444)
   {
      core.pNext = &v11;
      v11.pNext = &v12;
      v12.pNext = &v13;
      v13.pNext = &shaderObject;
      shaderObject.pNext = &robustness2;
      robustness2.pNext = &border;
      border.pNext = &depthClip;
      void **tail = &depthClip.pNext;
      *tail = nullptr;
      if (with_divisor) {
         *tail = &divisor;
         tail = &divisor.pNext;
         *tail = nullptr;
      }
      if (with_4444) {
         *tail = &fmt4444;
         tail = &fmt4444.pNext;
         *tail = nullptr;
      }
   }
};

HMODULE load_driver(PFN_vkGetInstanceProcAddr *gipa, bool *nvk)
{
#ifdef TGD3D_GENERIC_VULKAN
   /* Controlled test build only: any Vulkan 1.3 driver through the loader. */
   HMODULE module = LoadLibraryW(L"vulkan-1.dll");
   if (!module)
      return nullptr;
   *gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      reinterpret_cast<void *>(GetProcAddress(module, "vkGetInstanceProcAddr")));
   *nvk = false;
#else
   /* The matching NVK ICD is installed beside this DLL. Loading it by
    * absolute path needs no Vulkan loader, registry entry or environment
    * in the composing process, and cannot recurse into a D3D driver. */
   HMODULE self = nullptr;
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&load_driver), &self))
      return nullptr;
   wchar_t path[MAX_PATH + 32];
   DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
   if (!length || length >= MAX_PATH)
      return nullptr;
   while (length && path[length - 1] != L'\\' && path[length - 1] != L'/')
      length--;
   static const wchar_t name[] = L"libvulkan_nouveau.dll";
   memcpy(path + length, name, sizeof(name));
   HMODULE module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
   if (!module)
      return nullptr;
   *gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      reinterpret_cast<void *>(GetProcAddress(module, "vk_icdGetInstanceProcAddr")));
   *nvk = true;
#endif
   if (!*gipa) {
      FreeLibrary(module);
      return nullptr;
   }
   return module;
}

}

uint32_t VulkanAdapter::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const
{
   for (uint32_t i = 0; i < caps.memory.memoryTypeCount; i++) {
      if ((typeBits & (1u << i)) &&
          (caps.memory.memoryTypes[i].propertyFlags & required) == required)
         return i;
   }
   return UINT32_MAX;
}

void VulkanAdapter::close()
{
   disk_cache_destroy(shaderCache);
   shaderCache = nullptr;
   if (instance && vk.DestroyInstance)
      vk.DestroyInstance(instance, nullptr);
   instance = VK_NULL_HANDLE;
   pdev = VK_NULL_HANDLE;
   if (module)
      FreeLibrary(module);
   module = nullptr;
}

#define FAIL(hr, ...) do { snprintf(failure, sizeof(failure), __VA_ARGS__); close(); return hr; } while (0)

HRESULT VulkanAdapter::open(uint64_t luid)
{
   module = load_driver(&gipa, &nvk);
   if (!module)
      FAIL(DXGI_ERROR_UNSUPPORTED, "Vulkan driver not loadable (error %lu)", GetLastError());

   auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
   if (!create)
      FAIL(DXGI_ERROR_UNSUPPORTED, "vkCreateInstance missing");
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.pApplicationName = "tgd3d";
   app.pEngineName = "tgd3d";
   app.apiVersion = VK_API_VERSION_1_3;
   VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   info.pApplicationInfo = &app;
   /* GM20B is an experimental NVK device. This UMD is its matched, in-tree
    * consumer, so it asks for the device explicitly instead of editing the
    * environment of dwm.exe. */
   struct nvk_embedded_instance_info embedded = {
      VK_STRUCTURE_TYPE_NVK_EMBEDDED_INSTANCE_INFO_MESA, nullptr, true};
   if (nvk)
      info.pNext = &embedded;
   VkResult result = create(&info, nullptr, &instance);
   if (result != VK_SUCCESS)
      FAIL(DXGI_ERROR_UNSUPPORTED, "vkCreateInstance %d", result);
#define X(n) vk.n = reinterpret_cast<PFN_vk##n>(gipa(instance, "vk" #n)); \
   if (!vk.n) FAIL(DXGI_ERROR_UNSUPPORTED, "vk" #n " missing");
   TGD3D_VK_INSTANCE_FUNCS(X)
#undef X

   uint32_t count = 0;
   vk.EnumeratePhysicalDevices(instance, &count, nullptr);
   std::vector<VkPhysicalDevice> devices(count);
   if (count)
      vk.EnumeratePhysicalDevices(instance, &count, devices.data());
   VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
   VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
   for (uint32_t i = 0; i < count && !pdev; i++) {
      VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      props.pNext = &id;
      vk.GetPhysicalDeviceProperties2(devices[i], &props);
      if (id.deviceLUIDValid && !memcmp(id.deviceLUID, &luid, sizeof(luid))) {
         pdev = devices[i];
         caps.driverVersion = props.properties.driverVersion;
         memcpy(caps.deviceUUID, id.deviceUUID, VK_UUID_SIZE);
         memcpy(caps.driverUUID, id.driverUUID, VK_UUID_SIZE);
      }
   }
#ifdef TGD3D_GENERIC_VULKAN
   /* The test loader's software device has no WDDM adapter identity. */
   if (!pdev && count)
      pdev = devices[0];
#endif
   if (!pdev)
      FAIL(DXGI_ERROR_UNSUPPORTED, "no Vulkan device with adapter LUID %08lx%08lx",
           (unsigned long)(luid >> 32), (unsigned long)luid);

   ExtensionSet ext;
   count = 0;
   vk.EnumerateDeviceExtensionProperties(pdev, nullptr, &count, nullptr);
   ext.props.resize(count);
   if (count)
      vk.EnumerateDeviceExtensionProperties(pdev, nullptr, &count, ext.props.data());
   static const char *const required[] = {
      VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
      VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
      VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
   };
   for (const char *name : required)
      if (!ext.has(name))
         FAIL(DXGI_ERROR_UNSUPPORTED, "missing %s", name);
   caps.externalSemaphoreWin32 = ext.has(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
   caps.externalMemoryHost = ext.has(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
   if (nvk && !caps.externalSemaphoreWin32)
      FAIL(DXGI_ERROR_UNSUPPORTED, "NVK lacks the shared monitored-fence semaphore");
   if (!nvk && !caps.externalMemoryHost)
      FAIL(DXGI_ERROR_UNSUPPORTED, "test driver lacks host-pointer import");
   const bool has_divisor = ext.has(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
   const bool has_4444 = ext.has(VK_EXT_4444_FORMATS_EXTENSION_NAME);

   props.pNext = caps.externalMemoryHost ? &host : nullptr;
   vk.GetPhysicalDeviceProperties2(pdev, &props);
   const VkPhysicalDeviceLimits &limits = props.properties.limits;
   caps.apiVersion = props.properties.apiVersion;
   if (caps.apiVersion < VK_API_VERSION_1_3)
      FAIL(DXGI_ERROR_UNSUPPORTED, "Vulkan 1.3 required");
   caps.cbSlots = D3D_CB_SLOTS;
   caps.maxImageDimension2D = limits.maxImageDimension2D;
   caps.maxImageDimension3D = limits.maxImageDimension3D;
   caps.maxImageArrayLayers = limits.maxImageArrayLayers;
   caps.maxColorAttachments = limits.maxColorAttachments;
   caps.maxViewports = limits.maxViewports;
   caps.maxSamplerAnisotropy = limits.maxSamplerAnisotropy;
   caps.timestampPeriod = limits.timestampPeriod;
   caps.minUniformBufferOffsetAlignment = limits.minUniformBufferOffsetAlignment;
   caps.minHostPointerAlignment = caps.externalMemoryHost ? host.minImportedHostPointerAlignment : 0;
   if (caps.maxImageDimension2D < 8192 || caps.maxColorAttachments < 8)
      FAIL(DXGI_ERROR_UNSUPPORTED, "device limits below Direct3D 10");

   FeatureChain have;
   have.link(has_divisor, has_4444);
   vk.GetPhysicalDeviceFeatures2(pdev, &have.core);
   const VkPhysicalDeviceFeatures &f = have.core.features;
   struct { VkBool32 value; const char *name; } needed[] = {
      {f.independentBlend, "independentBlend"}, {f.depthBiasClamp, "depthBiasClamp"},
      {f.fillModeNonSolid, "fillModeNonSolid"}, {f.samplerAnisotropy, "samplerAnisotropy"},
      {f.dualSrcBlend, "dualSrcBlend"}, {f.fullDrawIndexUint32, "fullDrawIndexUint32"},
      {f.robustBufferAccess, "robustBufferAccess"},
      {have.v11.shaderDrawParameters, "shaderDrawParameters"},
      {have.v12.timelineSemaphore, "timelineSemaphore"},
      {have.v12.hostQueryReset, "hostQueryReset"},
      {have.v12.vulkanMemoryModel, "vulkanMemoryModel"},
      {have.v12.bufferDeviceAddress, "bufferDeviceAddress"},
      {have.v12.samplerMirrorClampToEdge, "samplerMirrorClampToEdge"},
      {have.v13.dynamicRendering, "dynamicRendering"},
      {have.v13.synchronization2, "synchronization2"},
      {have.v13.maintenance4, "maintenance4"},
      {have.v13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation"},
      {have.shaderObject.shaderObject, "shaderObject"},
      {have.robustness2.nullDescriptor, "nullDescriptor"},
      {have.robustness2.robustBufferAccess2, "robustBufferAccess2"},
      {have.robustness2.robustImageAccess2, "robustImageAccess2"},
      {have.border.customBorderColors, "customBorderColors"},
      {have.border.customBorderColorWithoutFormat, "customBorderColorWithoutFormat"},
      {have.depthClip.depthClipEnable, "depthClipEnable"},
   };
   for (const auto &n : needed)
      if (!n.value)
         FAIL(DXGI_ERROR_UNSUPPORTED, "missing feature %s", n.name);
   if (!have.v12.scalarBlockLayout)
      FAIL(DXGI_ERROR_UNSUPPORTED, "missing feature scalarBlockLayout");
   caps.scalarBlockLayout = true;
   caps.occlusionQueryPrecise = f.occlusionQueryPrecise;
   VkPhysicalDeviceVulkan12Properties props12 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
   props.pNext = &props12;
   vk.GetPhysicalDeviceProperties2(pdev, &props);
   caps.roundRTE = props12.shaderRoundingModeRTEFloat32;
   caps.roundRTZ = props12.shaderRoundingModeRTZFloat32;
   caps.denormPreserve = props12.shaderDenormPreserveFloat32;
   caps.denormFlush = props12.shaderDenormFlushToZeroFloat32;
   caps.zeroInfNanPreserve = props12.shaderSignedZeroInfNanPreserveFloat32;
   caps.geometryShader = f.geometryShader;
   caps.drawIndirectFirstInstance = f.drawIndirectFirstInstance;
   caps.depthClamp = f.depthClamp;
   caps.pipelineStatistics = f.pipelineStatisticsQuery;
   caps.textureCompressionBC = f.textureCompressionBC;
   caps.instanceDivisor = has_divisor && have.divisor.vertexAttributeInstanceRateDivisor;
   caps.instanceZeroDivisor = has_divisor && have.divisor.vertexAttributeInstanceRateZeroDivisor;
   caps.formatA4B4G4R4 = has_4444 && have.fmt4444.formatA4B4G4R4;

   vk.GetPhysicalDeviceMemoryProperties(pdev, &caps.memory);
   count = 0;
   vk.GetPhysicalDeviceQueueFamilyProperties(pdev, &count, nullptr);
   std::vector<VkQueueFamilyProperties> families(count);
   vk.GetPhysicalDeviceQueueFamilyProperties(pdev, &count, families.data());
   caps.queueFamily = UINT32_MAX;
   for (uint32_t i = 0; i < count; i++) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         caps.queueFamily = i;
         caps.timestampValidBits = families[i].timestampValidBits;
         break;
      }
   }
   if (caps.queueFamily == UINT32_MAX)
      FAIL(DXGI_ERROR_UNSUPPORTED, "no graphics queue");
   VkPhysicalDeviceShaderObjectPropertiesEXT shader = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
   props.pNext = &shader;
   vk.GetPhysicalDeviceProperties2(pdev, &props);
   shaderCache = create_shader_cache(shader);
   return S_OK;
}

#undef FAIL

VkResult VulkanAdapter::createDevice(VkDevice *device, VkDeviceApi *api, VkQueue *queue) const
{
   FeatureChain have, want;
   have.link(caps.instanceDivisor, caps.formatA4B4G4R4);
   vk.GetPhysicalDeviceFeatures2(pdev, &have.core);
   want.link(caps.instanceDivisor, caps.formatA4B4G4R4);
   const VkPhysicalDeviceFeatures &hf = have.core.features;
   VkPhysicalDeviceFeatures &f = want.core.features;
   f.independentBlend = f.depthBiasClamp = f.fillModeNonSolid = f.samplerAnisotropy = VK_TRUE;
   f.dualSrcBlend = f.fullDrawIndexUint32 = f.robustBufferAccess = VK_TRUE;
   f.geometryShader = hf.geometryShader;
   f.drawIndirectFirstInstance = hf.drawIndirectFirstInstance;
   f.depthClamp = hf.depthClamp;
   f.occlusionQueryPrecise = hf.occlusionQueryPrecise;
   f.pipelineStatisticsQuery = hf.pipelineStatisticsQuery;
   f.textureCompressionBC = hf.textureCompressionBC;
   f.multiViewport = hf.multiViewport;
   f.shaderClipDistance = hf.shaderClipDistance;
   f.shaderCullDistance = hf.shaderCullDistance;
   f.imageCubeArray = hf.imageCubeArray;
   f.sampleRateShading = hf.sampleRateShading;
   f.shaderImageGatherExtended = hf.shaderImageGatherExtended;
   want.v11.shaderDrawParameters = VK_TRUE;
   want.v12.timelineSemaphore = want.v12.hostQueryReset = VK_TRUE;
   want.v12.vulkanMemoryModel = want.v12.bufferDeviceAddress = VK_TRUE;
   want.v12.samplerMirrorClampToEdge = VK_TRUE;
   want.v12.scalarBlockLayout = VK_TRUE;
   want.v12.uniformBufferStandardLayout = have.v12.uniformBufferStandardLayout;
   want.v12.shaderOutputLayer = have.v12.shaderOutputLayer;
   want.v12.shaderOutputViewportIndex = have.v12.shaderOutputViewportIndex;
   want.v13.dynamicRendering = want.v13.synchronization2 = want.v13.maintenance4 = VK_TRUE;
   want.v13.shaderDemoteToHelperInvocation = VK_TRUE;
   want.shaderObject.shaderObject = VK_TRUE;
   want.robustness2.nullDescriptor = VK_TRUE;
   want.robustness2.robustBufferAccess2 = want.robustness2.robustImageAccess2 = VK_TRUE;
   want.border.customBorderColors = want.border.customBorderColorWithoutFormat = VK_TRUE;
   want.depthClip.depthClipEnable = VK_TRUE;
   want.divisor.vertexAttributeInstanceRateDivisor = caps.instanceDivisor;
   want.divisor.vertexAttributeInstanceRateZeroDivisor = caps.instanceZeroDivisor;
   want.fmt4444.formatA4B4G4R4 = caps.formatA4B4G4R4;

   std::vector<const char *> extensions = {
      VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
      VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
      VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
   };
   if (caps.instanceDivisor)
      extensions.push_back(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
   if (caps.formatA4B4G4R4)
      extensions.push_back(VK_EXT_4444_FORMATS_EXTENSION_NAME);
   if (nvk)
      extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
   else
      extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);

   const float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   queue_info.queueFamilyIndex = caps.queueFamily;
   queue_info.queueCount = 1;
   queue_info.pQueuePriorities = &priority;
   VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   info.pNext = &want.core;
   info.queueCreateInfoCount = 1;
   info.pQueueCreateInfos = &queue_info;
   info.enabledExtensionCount = (uint32_t)extensions.size();
   info.ppEnabledExtensionNames = extensions.data();
   VkResult result = vk.CreateDevice(pdev, &info, nullptr, device);
   if (result != VK_SUCCESS)
      return result;
#define X(n) api->n = reinterpret_cast<PFN_vk##n>(vk.GetDeviceProcAddr(*device, "vk" #n)); \
   if (!api->n) result = VK_ERROR_INITIALIZATION_FAILED;
   TGD3D_VK_DEVICE_FUNCS(X)
#undef X
#define X(n) api->n = reinterpret_cast<PFN_vk##n>(vk.GetDeviceProcAddr(*device, "vk" #n));
   TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
   if (nvk && !api->ImportSemaphoreWin32HandleKHR)
      result = VK_ERROR_INITIALIZATION_FAILED;
   if (result != VK_SUCCESS) {
      if (api->DestroyDevice)
         api->DestroyDevice(*device, nullptr);
      *device = VK_NULL_HANDLE;
      return result;
   }
   api->GetDeviceQueue(*device, caps.queueFamily, 0, queue);
   return VK_SUCCESS;
}

}
