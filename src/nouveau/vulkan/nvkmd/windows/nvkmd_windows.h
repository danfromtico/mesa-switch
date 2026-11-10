/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_WINDOWS_H
#define NVKMD_WINDOWS_H

#include "nvkmd/nvkmd.h"
#include "kmt_native.h"

struct nvkmd_windows_dev {
   struct nvkmd_dev base;
   struct tg_kmt_device kmt;
};

static inline struct nvkmd_windows_dev *
nvkmd_windows_dev(struct nvkmd_dev *dev)
{
   return container_of(dev, struct nvkmd_windows_dev, base);
}

const struct tg_gpu_adapter_info *nvkmd_windows_adapter(const struct nvkmd_pdev *pdev);
VkResult nvkmd_windows_status(struct vk_object_base *log, const char *operation,
                             LONG status, VkResult failure);
VkResult nvkmd_windows_create_ctx(struct nvkmd_dev *dev, struct vk_object_base *log,
                                 enum nvkmd_engines engines, struct nvkmd_ctx **out);
extern const struct vk_sync_type nvkmd_windows_sync_type;

#endif
