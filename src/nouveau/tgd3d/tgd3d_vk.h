/* SPDX-License-Identifier: MIT
 *
 * Vulkan entry points used by the native D3D10 UMD. The UMD talks to the
 * Vulkan driver through its API and paired Mesa-internal structures for
 * runtime-owned memory, queue/fence transport, pitch and embedded admission.
 */
#ifndef TGD3D_VK_H
#define TGD3D_VK_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include "vulkan/util/vk_wsi_win32_runtime.h"

struct disk_cache;

namespace tgd3d {

#define TGD3D_VK_INSTANCE_FUNCS(X) \
   X(DestroyInstance) \
   X(EnumeratePhysicalDevices) \
   X(GetPhysicalDeviceProperties2) \
   X(GetPhysicalDeviceFeatures2) \
   X(GetPhysicalDeviceMemoryProperties) \
   X(GetPhysicalDeviceQueueFamilyProperties) \
   X(GetPhysicalDeviceFormatProperties) \
   X(GetPhysicalDeviceImageFormatProperties2) \
   X(EnumerateDeviceExtensionProperties) \
   X(CreateDevice) \
   X(GetDeviceProcAddr)

#define TGD3D_VK_DEVICE_FUNCS(X) \
   X(DestroyDevice) \
   X(GetDeviceQueue) \
   X(DeviceWaitIdle) \
   X(CreateCommandPool) \
   X(DestroyCommandPool) \
   X(AllocateCommandBuffers) \
   X(FreeCommandBuffers) \
   X(BeginCommandBuffer) \
   X(EndCommandBuffer) \
   X(ResetCommandBuffer) \
   X(QueueSubmit2) \
   X(CreateSemaphore) \
   X(DestroySemaphore) \
   X(GetSemaphoreCounterValue) \
   X(WaitSemaphores) \
   X(SignalSemaphore) \
   X(CreateBuffer) \
   X(DestroyBuffer) \
   X(GetBufferMemoryRequirements) \
   X(CreateImage) \
   X(DestroyImage) \
   X(GetImageMemoryRequirements) \
   X(GetImageSubresourceLayout) \
   X(AllocateMemory) \
   X(FreeMemory) \
   X(BindBufferMemory) \
   X(BindImageMemory) \
   X(MapMemory) \
   X(UnmapMemory) \
   X(CreateImageView) \
   X(DestroyImageView) \
   X(CreateBufferView) \
   X(DestroyBufferView) \
   X(CreateSampler) \
   X(DestroySampler) \
   X(CreateDescriptorSetLayout) \
   X(DestroyDescriptorSetLayout) \
   X(CreatePipelineLayout) \
   X(DestroyPipelineLayout) \
   X(CmdPushConstants) \
   X(CreateDescriptorPool) \
   X(DestroyDescriptorPool) \
   X(ResetDescriptorPool) \
   X(AllocateDescriptorSets) \
   X(UpdateDescriptorSets) \
   X(CreateShadersEXT) \
   X(GetShaderBinaryDataEXT) \
   X(DestroyShaderEXT) \
   X(CreateQueryPool) \
   X(DestroyQueryPool) \
   X(GetQueryPoolResults) \
   X(ResetQueryPool) \
   X(CmdBindShadersEXT) \
   X(CmdBeginRendering) \
   X(CmdEndRendering) \
   X(CmdClearAttachments) \
   X(CmdPipelineBarrier2) \
   X(CmdCopyBuffer) \
   X(CmdCopyImage) \
   X(CmdCopyBufferToImage) \
   X(CmdCopyImageToBuffer) \
   X(CmdBlitImage) \
   X(CmdDraw) \
   X(CmdDrawIndexed) \
   X(CmdDrawIndirect) \
   X(CmdDrawIndexedIndirect) \
   X(CmdBindVertexBuffers2) \
   X(CmdBindIndexBuffer) \
   X(CmdBindDescriptorSets) \
   X(CmdBeginQuery) \
   X(CmdEndQuery) \
   X(CmdWriteTimestamp) \
   X(CmdSetViewportWithCount) \
   X(CmdSetScissorWithCount) \
   X(CmdSetPrimitiveTopology) \
   X(CmdSetPrimitiveRestartEnable) \
   X(CmdSetRasterizerDiscardEnable) \
   X(CmdSetCullMode) \
   X(CmdSetFrontFace) \
   X(CmdSetDepthBiasEnable) \
   X(CmdSetDepthBias) \
   X(CmdSetLineWidth) \
   X(CmdSetDepthTestEnable) \
   X(CmdSetDepthWriteEnable) \
   X(CmdSetDepthCompareOp) \
   X(CmdSetDepthBoundsTestEnable) \
   X(CmdSetStencilTestEnable) \
   X(CmdSetStencilOp) \
   X(CmdSetStencilCompareMask) \
   X(CmdSetStencilWriteMask) \
   X(CmdSetStencilReference) \
   X(CmdSetBlendConstants) \
   X(CmdSetVertexInputEXT) \
   X(CmdSetPolygonModeEXT) \
   X(CmdSetRasterizationSamplesEXT) \
   X(CmdSetSampleMaskEXT) \
   X(CmdSetAlphaToCoverageEnableEXT) \
   X(CmdSetAlphaToOneEnableEXT) \
   X(CmdSetLogicOpEnableEXT) \
   X(CmdSetColorBlendEnableEXT) \
   X(CmdSetColorBlendEquationEXT) \
   X(CmdSetColorWriteMaskEXT) \
   X(CmdSetDepthClampEnableEXT) \
   X(CmdSetDepthClipEnableEXT)

/* Loaded only when the matching extension is enabled. */
#define TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X) \
   X(ImportSemaphoreWin32HandleKHR) \
   X(GetMemoryHostPointerPropertiesEXT) \
   X(CmdSetTessellationDomainOriginEXT) \
   X(CmdSetPatchControlPointsEXT) \
   X(CmdSetRasterizationStreamEXT)

struct VkInstanceApi {
#define X(n) PFN_vk##n n = nullptr;
   TGD3D_VK_INSTANCE_FUNCS(X)
#undef X
};

struct VkDeviceApi {
#define X(n) PFN_vk##n n = nullptr;
   TGD3D_VK_DEVICE_FUNCS(X)
   TGD3D_VK_OPTIONAL_DEVICE_FUNCS(X)
#undef X
};

/* D3D10 fixed stage order used for descriptor binding ranges. */
enum Stage : uint32_t { STAGE_VS = 0, STAGE_GS = 1, STAGE_PS = 2, STAGE_COUNT = 3 };

enum : uint32_t {
   D3D_CB_SLOTS = 14,       /* D3D10_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT */
   D3D_SAMPLER_SLOTS = 16,
   D3D_SRV_SLOTS = 128,
   /* Set 0: uniform buffers, binding = stage * cbSlots + slot.
    * Set 1: stage * SET1_STRIDE + [0,16) samplers, + 16 + [0,128) sampled images.
    * Set 2: stage * 128 + slot, uniform texel buffers (Buffer<> SRVs). */
   SET_CB = 0,
   SET_TEX = 1,
   SET_TBUF = 2,
   SET_COUNT = 3,
   SET1_STRIDE = D3D_SAMPLER_SLOTS + D3D_SRV_SLOTS,
};

struct VulkanCaps {
   uint32_t apiVersion = 0;
   uint32_t driverVersion = 0;
   uint8_t deviceUUID[VK_UUID_SIZE] = {}, driverUUID[VK_UUID_SIZE] = {};
   uint32_t queueFamily = 0;
   uint32_t cbSlots = 0;            /* full D3D10 constant-buffer slots per stage */
   uint32_t maxImageDimension2D = 0;
   uint32_t maxImageDimension3D = 0;
   uint32_t maxImageArrayLayers = 0;
   uint32_t maxColorAttachments = 0;
   uint32_t maxViewports = 0;
   float maxSamplerAnisotropy = 1.0f;
   float timestampPeriod = 0.0f;
   uint32_t timestampValidBits = 0;
   VkDeviceSize minHostPointerAlignment = 0;
   VkDeviceSize minUniformBufferOffsetAlignment = 0;
   VkPhysicalDeviceMemoryProperties memory = {};
   /* Optional features that change the reported D3D capabilities. */
   bool geometryShader = false;
   bool drawIndirectFirstInstance = false;
   bool depthClamp = false;
   bool alphaToOne = false;
   bool tessellationShader = false;
   bool transformFeedback = false;
   bool pipelineStatistics = false;
   bool textureCompressionBC = false;
   bool instanceDivisor = false;
   bool instanceZeroDivisor = false;
   bool formatA4B4G4R4 = false;
   bool externalSemaphoreWin32 = false;
   bool externalMemoryHost = false;
   bool occlusionQueryPrecise = false;
   bool scalarBlockLayout = false;
   /* Float controls the shader compiler may rely on. */
   bool roundRTE = false, roundRTZ = false;
   bool denormPreserve = false, denormFlush = false;
   bool zeroInfNanPreserve = false;
};

struct VulkanAdapter {
   HMODULE module = nullptr;
   PFN_vkGetInstanceProcAddr gipa = nullptr;
   VkInstance instance = VK_NULL_HANDLE;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkInstanceApi vk;
   VulkanCaps caps;
   disk_cache *shaderCache = nullptr;
   /* True for the Tegra NVK ICD: runtime-owned WDDM allocations are imported
    * through the owner callbacks and the queue fence is a shared monitored
    * fence. False only in the controlled test build, where host pointers and
    * a device-local timeline semaphore stand in for both. */
   bool nvk = false;
   char failure[160] = {};

   /* Opens the Vulkan driver and selects the physical device with this
    * adapter LUID. Fails closed when a required capability is missing. */
   HRESULT open(uint64_t luid);
   void close();
   VkResult createDevice(VkDevice *device, VkDeviceApi *api, VkQueue *queue,
                         const struct wsi_win32_runtime *runtime = nullptr) const;
   uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
};

disk_cache *create_shader_cache(const VkPhysicalDeviceShaderObjectPropertiesEXT &properties);

}

#endif
