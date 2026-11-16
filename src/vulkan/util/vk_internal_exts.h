/*
 * Copyright © 2025 Collabora Ltd
 * SPDX-License-Identifier: MIT
 */

/*
 * This file contains a variety of mesa-internal extension structs and
 * enumerants.  These are not exposed to apps but are instead used for the
 * runtime components (including meta and WSI) to communicate additional
 * information to drivers beyond what is provided through the Vulkan spec
 * itself.  Care should be taken when adding anything here to avoid
 * conflicting with existing Vulkan enums if at all possible.
 */

#ifndef VK_INTERNAL_EXTS_H
#define VK_INTERNAL_EXTS_H

#include <vulkan/vulkan_core.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VK_PRIMITIVE_TOPOLOGY_META_RECT_LIST_MESA (VkPrimitiveTopology)11
#define VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA \
   (VkImageViewCreateFlagBits)0x80000000


/* This is always chained to VkImageCreateInfo when a wsi image is created.
 * It indicates that the image can be transitioned to/from
 * VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.
 */
struct wsi_image_create_info {
    VkStructureType sType;
    const void *pNext;
    bool scanout;

    /* if true, the image is a blit source */
    bool blit_src;
#ifdef _WIN32
    /* Explicit pitch of a runtime-owned linear scanout allocation. Zero
     * leaves ordinary Vulkan/WSI image layout selection unchanged. */
    uint32_t win32_row_pitch;
#endif
};

struct wsi_win32_memory;

struct wsi_memory_allocate_info {
    VkStructureType sType;
    const void *pNext;
    /**
     * If set, then the driver needs to do implicit synchronization on this BO.
     *
     * For DRM drivers, this flag will only get set before linux 6.0, at which
     * point DMA_BUF_IOCTL_IMPORT_SYNC_FILE was added.
     */
    bool implicit_sync;

    /**
     * If set, then the WSI will attach implicit synchronization's fences to
     * (and reference implicit sync fences from) the dma-buf for this memory, to
     * meet the requirement for implicit synchronization with the window system.
     * This does mean that the kernel BO must be able to participate in implicit
     * sync still when used as part of a non-explicit-synced submit (e.g. by a
     * GL driver in the X server).
     */
    bool dma_buf_sync_file;

#ifdef _WIN32
    /* Optional runtime-owned WDDM memory, not a public external handle.
     * The importing driver retains this owner through its callbacks. */
    const struct wsi_win32_memory *win32_memory;
    bool *win32_imported;
    /* Read only for a v2 displayable owner. Written on successful import with
     * an allocation belonging to the ICD's submitting device. */
    uint32_t *win32_written_allocation;
#endif
};

/* To be chained into VkSurfaceCapabilities2KHR */
struct wsi_surface_supported_counters {
   VkStructureType sType;
   const void *pNext;

   VkSurfaceCounterFlagsEXT supported_surface_counters;

};

/* This is guaranteed to not collide with anything because it's in the
 * VK_KHR_swapchain namespace but not actually used by the extension.
 */
#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA \
   (VkStructureType)1000001002
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA \
   (VkStructureType)1000001003
#define VK_STRUCTURE_TYPE_WSI_SURFACE_SUPPORTED_COUNTERS_MESA \
   (VkStructureType)1000001005

#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA_cast \
   struct wsi_image_create_info
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA_cast \
   struct wsi_memory_allocate_info
#define VK_STRUCTURE_TYPE_WSI_SURFACE_SUPPORTED_COUNTERS_MESA_cast \
   struct wsi_surface_supported_counters

#ifdef _WIN32
/* Private tgd3d/NVK contract. Handles belong to NVK's submitting device and
 * their VkDeviceMemory owners outlive actual GPU completion. The runtime
 * copies the inline array; it never retains the caller's stack pointer. */
#define WSI_WIN32_MAX_WRITTEN_PRIMARIES 16u
struct wsi_win32_written_primaries {
   VkStructureType sType;
   const void *pNext;
   uint32_t version;
   uint32_t count;
   uint32_t allocations[WSI_WIN32_MAX_WRITTEN_PRIMARIES];
};
#define VK_STRUCTURE_TYPE_WSI_WIN32_WRITTEN_PRIMARIES_MESA (VkStructureType)1000001007
#define VK_STRUCTURE_TYPE_WSI_WIN32_WRITTEN_PRIMARIES_MESA_cast struct wsi_win32_written_primaries

struct wsi_win32_runtime;
struct wsi_win32_runtime_device_info {
   VkStructureType sType;
   const void *pNext;
   const struct wsi_win32_runtime *runtime;
   bool *accepted;
};
#define VK_STRUCTURE_TYPE_WSI_WIN32_RUNTIME_DEVICE_INFO_MESA (VkStructureType)1000001008
#define VK_STRUCTURE_TYPE_WSI_WIN32_RUNTIME_DEVICE_INFO_MESA_cast struct wsi_win32_runtime_device_info

struct wsi_win32_runtime_fence_import {
   VkStructureType sType;
   const void *pNext;
   uint32_t version;
   void *device_cookie;
   void *fence_token;
};
#define VK_STRUCTURE_TYPE_WSI_WIN32_RUNTIME_FENCE_IMPORT_MESA (VkStructureType)1000001009
#define VK_STRUCTURE_TYPE_WSI_WIN32_RUNTIME_FENCE_IMPORT_MESA_cast struct wsi_win32_runtime_fence_import
#endif

/* Chained to VkInstanceCreateInfo by an in-tree Windows user-mode driver that
 * loads the NVK ICD by path, without a Vulkan loader. It replaces the
 * NVK_I_WANT_A_BROKEN_VULKAN_DRIVER environment opt-in for that one matched
 * consumer on GM20B; it does not make the device conformant. This is a
 * private in-tree opt-in, not a public extension or an authorization check.
 */
struct nvk_embedded_instance_info {
    VkStructureType sType;
    const void *pNext;
    bool allow_experimental_device;
};

#define VK_STRUCTURE_TYPE_NVK_EMBEDDED_INSTANCE_INFO_MESA \
   (VkStructureType)1000001006
#define VK_STRUCTURE_TYPE_NVK_EMBEDDED_INSTANCE_INFO_MESA_cast \
   struct nvk_embedded_instance_info

/**
 * Pseudo-extension struct that may be chained into VkRenderingAttachmentInfo
 * to indicate an initial layout for the attachment.  This is only allowed if
 * all of the following conditions are met:
 *
 *    1. VkRenderingAttachmentInfo::loadOp == LOAD_OP_CLEAR
 *
 *    2. VkRenderingInfo::renderArea is the entire image view LOD
 *
 *    3. For 3D image attachments, VkRenderingInfo::viewMask == 0 AND
 *       VkRenderingInfo::layerCount references the entire bound image view
 *       OR VkRenderingInfo::viewMask is dense (no holes) and references the
 *       entire bound image view.  (2D and 2D array images have no such
 *       requirement.)
 *
 * If this struct is included in the pNext chain of a
 * VkRenderingAttachmentInfo, the driver is responsible for transitioning the
 * bound region of the image from
 * VkRenderingAttachmentInitialLayoutInfoMESA::initialLayout to
 * VkRenderingAttachmentInfo::imageLayout prior to rendering.
 */
typedef struct VkRenderingAttachmentInitialLayoutInfoMESA {
    VkStructureType    sType;
    const void*        pNext;

    /** Initial layout of the attachment */
    VkImageLayout      initialLayout;
} VkRenderingAttachmentInitialLayoutInfoMESA;

#define VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA \
   (VkStructureType)1000044901
#define VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA_cast \
   VkRenderingAttachmentInitialLayoutInfoMESA


struct nir_shader;

typedef struct VkPipelineShaderStageNirCreateInfoMESA {
   VkStructureType sType;
   const void *pNext;
   struct nir_shader *nir;
} VkPipelineShaderStageNirCreateInfoMESA;

#define VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA \
   (VkStructureType)1000290001

#define VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_NIR_CREATE_INFO_MESA_cast \
   VkPipelineShaderStageNirCreateInfoMESA


static const VkPipelineCreateFlagBits2
   VK_PIPELINE_CREATE_2_UNALIGNED_DISPATCH_BIT_MESA = 0x20000000000ull;


#define VK_SHADER_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_MESA 0x00080000
#define VK_SHADER_CREATE_UNALIGNED_DISPATCH_BIT_MESA               0x00100000

#ifdef __cplusplus
}
#endif

#endif /* VK_INTERNAL_EXTS_H */
