/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_WINDOWS_H
#define NVKMD_WINDOWS_H

#include "nvkmd/nvkmd.h"
#include "kmt_native.h"
#include <vulkan/vulkan_win32.h>
#include "vk_wsi_win32_runtime.h"

struct nvkmd_windows_mem;

struct nvkmd_windows_dev {
   struct nvkmd_dev base;
   struct tg_kmt_device kmt;
   SRWLOCK alias_cleanup_lock;
   struct nvkmd_windows_mem *pending_aliases;
   struct wsi_win32_runtime runtime;
};

static inline struct nvkmd_windows_dev *
nvkmd_windows_dev(struct nvkmd_dev *dev)
{
   return container_of(dev, struct nvkmd_windows_dev, base);
}

const struct tg_gpu_adapter_info *nvkmd_windows_adapter(const struct nvkmd_pdev *pdev);
/* VidMm's values for the segment group that NVK_WINDOWS_BUDGET_GROUP names
 * as the measured home of this adapter's heap. False when no group was
 * selected or the query failed; the caller then keeps its own estimate.
 */
bool nvkmd_windows_measured_memory(struct nvkmd_pdev *pdev, struct vk_object_base *log,
                                   struct tg_kmt_video_memory *memory);
VkResult nvkmd_windows_export_memory(struct nvkmd_mem *mem, struct vk_object_base *log,
                                     const VkExportMemoryWin32HandleInfoKHR *info,
                                     HANDLE *handle);
uint32_t nvkmd_windows_written_allocation(struct nvkmd_mem *mem);
VkResult nvkmd_windows_set_runtime(struct nvkmd_dev *dev, const struct wsi_win32_runtime *runtime);
VkResult nvkmd_windows_status(struct vk_object_base *log, const char *operation,
                             LONG status, VkResult failure);
VkResult nvkmd_windows_create_ctx(struct nvkmd_dev *dev, struct vk_object_base *log,
                                 enum nvkmd_engines engines, struct nvkmd_ctx **out);
extern const struct vk_sync_type nvkmd_windows_sync_type;

#endif
