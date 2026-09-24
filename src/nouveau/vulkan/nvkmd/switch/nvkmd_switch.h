/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_SWITCH_H
#define NVKMD_SWITCH_H 1

#include "nouveau/horizon/nouveau_horizon.h"
#include "nvkmd/nvkmd.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"

#include <switch.h>

/* Horizon / Tegra X1 nvkmd backend using libnx nv services. */

struct nvkmd_switch_pdev {
   struct nvkmd_pdev base;

   struct vk_sync_timeline_type timeline_sync_type;

   /* sync_types is a NULL-terminated array referenced by base.sync_types */
   const struct vk_sync_type *sync_types[3];
};

struct nvkmd_switch_dev {
   struct nvkmd_dev base;

   struct nouveau_horizon_runtime *runtime;
   struct nouveau_horizon_device *horizon;
};

/* Horizon uses 64 KiB GPU pages. Report that bind alignment so
 * suballocators cannot supply unsupported 4 KiB-offset mappings.
 */
#define NVKMD_SWITCH_BIND_ALIGN_B ((uint32_t)0x10000)

VkResult nvkmd_switch_try_create_pdev(struct vk_object_base *log_obj,
                                      enum nvk_debug debug_flags,
                                      struct nvkmd_pdev **pdev_out);

VkResult nvkmd_switch_create_dev(struct nvkmd_pdev *pdev,
                                 struct vk_object_base *log_obj,
                                 struct nvkmd_dev **dev_out);

/* Install native completion on a Switch point sync from a vk_sync_signal
 * chain. Subsequent full waits must wait for the GPU fence.
 */
void nvkmd_switch_sync_import_nvmultifence(struct vk_sync *sync,
                                           const NvMultiFence *fence);

/* Whether the given sync type is the Switch nvfence binary sync. Lets
 * the ctx layer skip non-native sync objects (e.g. timeline wrappers,
 * dummy syncs) when importing fences. */
bool nvkmd_switch_sync_is_nvfence(const struct vk_sync_type *type);

/* Adapter-neutral forms used by the nvkmd context path. The NvMultiFence
 * entrypoint above remains the WSI boundary only.
 */
void nvkmd_switch_sync_import_horizon_fence(
   struct vk_sync *sync, const struct nouveau_horizon_fence *fence);
uint32_t nvkmd_switch_sync_peek_horizon_fences(
   struct vk_sync *sync, uint32_t max_fences,
   struct nouveau_horizon_fence *fences_out);

/* Peek at an imported native payload without waiting. Return false for
 * other sync types or CPU-only signals; callers must then wait on the
 * host.
 */
bool nvkmd_switch_sync_peek_nvmultifence(struct vk_sync *sync,
                                         NvMultiFence *out);

VkResult nvkmd_switch_sync_copy_payloads(struct vk_device *device,
                                         uint32_t wait_count,
                                         const struct vk_sync_wait *waits,
                                         uint32_t signal_count,
                                         const struct vk_sync_signal *signals);

/* Return the public NvMap ID backing this nvkmd_mem.  WSI passes the numeric
 * ID to the compositor without exposing Horizon backend internals.
 */
uint32_t nvkmd_switch_mem_get_nvmap_id(struct nvkmd_mem *mem);

VkResult nvkmd_switch_dev_import_nvmap(struct nvkmd_dev *dev,
                                       struct vk_object_base *log_obj,
                                       uint32_t nvmap_id,
                                       uint64_t size_B,
                                       uint64_t align_B,
                                       uint8_t pte_kind,
                                       uint16_t tile_mode,
                                       enum nvkmd_mem_flags flags,
                                       struct nvkmd_mem **mem_out);

bool nvkmd_switch_mem_export(struct nvkmd_mem *mem,
                             uint32_t *nvmap_id_out,
                             void **reference_out);

void nvkmd_switch_memory_reference_release(void *reference);

/* Thin queue adapters for the process-wide Horizon ZBC snapshot. */
void nvkmd_switch_dev_get_zbc_state(
   struct nvkmd_dev *dev, struct nouveau_horizon_zbc_state *state_out);
uint64_t nvkmd_switch_dev_get_zbc_generation(struct nvkmd_dev *dev);
void nvkmd_switch_dev_record_zbc_program(struct nvkmd_dev *dev);

/* False consumes the channel reference but retains the context because
 * completion is unknown. Callers must also retain all potentially
 * referenced resources.
 */
bool nvkmd_switch_ctx_try_destroy(struct nvkmd_ctx *ctx,
                                  struct vk_object_base *log_obj);

#endif /* NVKMD_SWITCH_H */
