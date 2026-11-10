/* SPDX-License-Identifier: MIT */
#include "nvkmd_windows.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"
#include "util/u_math.h"
#include "vk_log.h"
#include "vk_sync_binary.h"

struct nvkmd_windows_pdev {
   struct nvkmd_pdev base;
   struct tg_gpu_adapter_info adapter;
   struct vk_sync_binary_type binary_type;
   const struct vk_sync_type *sync_types[3];
};

struct nvkmd_windows_mem {
   struct nvkmd_mem base;
   struct tg_kmt_allocation allocation;
   SRWLOCK map_lock;
   uint32_t maps;
   void *cpu;
};

struct nvkmd_windows_va {
   struct nvkmd_va base;
   struct tg_kmt_va va;
};

VkResult
nvkmd_windows_status(struct vk_object_base *log, const char *operation,
                     LONG status, VkResult failure)
{
   if (status >= 0)
      return status == TG_KMT_TIMEOUT ? VK_TIMEOUT : VK_SUCCESS;
   if (status == TG_KMT_NO_MEMORY)
      failure = VK_ERROR_OUT_OF_HOST_MEMORY;
   else if (status == TG_KMT_NO_VIDEO_MEMORY)
      failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
   else if (status == TG_KMT_DEVICE_REMOVED)
      failure = VK_ERROR_DEVICE_LOST;
   return vk_errorf(log, failure, "%s returned 0x%08lx", operation,
                    (unsigned long)status);
}

const struct tg_gpu_adapter_info *
nvkmd_windows_adapter(const struct nvkmd_pdev *pdev)
{
   return &container_of(pdev, const struct nvkmd_windows_pdev, base)->adapter;
}

static void
win_free_va(struct nvkmd_va *base)
{
   struct nvkmd_windows_va *va = container_of(base, struct nvkmd_windows_va, base);
   tg_kmt_free_va(&nvkmd_windows_dev(base->dev)->kmt, &va->va);
   free(va);
}

static VkResult
win_bind_mem(struct nvkmd_va *base, struct vk_object_base *log, uint64_t va_offset,
             struct nvkmd_mem *mem, uint64_t mem_offset, uint64_t size)
{
   struct nvkmd_windows_va *va = container_of(base, struct nvkmd_windows_va, base);
   struct nvkmd_windows_mem *memory = container_of(mem, struct nvkmd_windows_mem, base);
   LONG status = tg_kmt_map(&nvkmd_windows_dev(base->dev)->kmt, &va->va, va_offset,
                            &memory->allocation, mem_offset, size, base->pte_kind);
   return nvkmd_windows_status(log, "D3DKMTMapGpuVirtualAddress", status,
                               VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

static VkResult
win_unbind_mem(struct nvkmd_va *va, struct vk_object_base *log,
               uint64_t offset, uint64_t size)
{
   (void)va;
   (void)offset;
   (void)size;
   return vk_error(log, VK_ERROR_FEATURE_NOT_PRESENT);
}

static const struct nvkmd_va_ops win_va_ops = {
   .free = win_free_va,
   .bind_mem = win_bind_mem,
   .unbind = win_unbind_mem,
};

static VkResult
win_alloc_va(struct nvkmd_dev *dev, struct vk_object_base *log,
             enum nvkmd_va_flags flags, uint8_t kind, uint64_t size,
             uint64_t alignment, uint64_t fixed, struct nvkmd_va **out)
{
   if (flags & NVKMD_VA_SPARSE)
      return vk_error(log, VK_ERROR_FEATURE_NOT_PRESENT);
   struct nvkmd_windows_va *va = calloc(1, sizeof(*va));
   if (!va)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   LONG status = tg_kmt_reserve(&nvkmd_windows_dev(dev)->kmt, size,
                               MAX2(alignment, TG_KMT_PAGE_SIZE), fixed, &va->va);
   if (status < 0) {
      free(va);
      return nvkmd_windows_status(log, "D3DKMTReserveGpuVirtualAddress", status,
                                  fixed ? VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS :
                                          VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   va->base = (struct nvkmd_va) {
      .ops = &win_va_ops, .dev = dev, .flags = flags, .pte_kind = kind,
      .addr = va->va.address, .size_B = size,
   };
   *out = &va->base;
   return VK_SUCCESS;
}

static void
win_free_mem(struct nvkmd_mem *base)
{
   struct nvkmd_windows_mem *mem = container_of(base, struct nvkmd_windows_mem, base);
   nvkmd_va_free(base->va);
   tg_kmt_free(&nvkmd_windows_dev(base->dev)->kmt, &mem->allocation);
   simple_mtx_destroy(&base->map_mutex);
   free(mem);
}

static VkResult
win_map_mem(struct nvkmd_mem *base, struct vk_object_base *log,
            enum nvkmd_mem_map_flags flags, void *fixed, void **cpu)
{
   struct nvkmd_windows_mem *mem = container_of(base, struct nvkmd_windows_mem, base);
   if (fixed || (flags & NVKMD_MEM_MAP_FIXED))
      return vk_error(log, VK_ERROR_MEMORY_MAP_FAILED);

   AcquireSRWLockExclusive(&mem->map_lock);
   LONG status = 0;
   if (!mem->maps)
      status = tg_kmt_lock(&nvkmd_windows_dev(base->dev)->kmt, &mem->allocation, &mem->cpu);
   if (status >= 0) {
      mem->maps++;
      *cpu = mem->cpu;
   }
   ReleaseSRWLockExclusive(&mem->map_lock);
   return nvkmd_windows_status(log, "D3DKMTLock2", status, VK_ERROR_MEMORY_MAP_FAILED);
}

static void
win_unmap_mem(struct nvkmd_mem *base, enum nvkmd_mem_map_flags flags, void *cpu)
{
   (void)flags;
   (void)cpu;
   struct nvkmd_windows_mem *mem = container_of(base, struct nvkmd_windows_mem, base);
   AcquireSRWLockExclusive(&mem->map_lock);
   if (--mem->maps == 0) {
      tg_kmt_unlock(&nvkmd_windows_dev(base->dev)->kmt, &mem->allocation);
      mem->cpu = NULL;
   }
   ReleaseSRWLockExclusive(&mem->map_lock);
}

static uint32_t
win_log_mem(struct nvkmd_mem *base)
{
   return container_of(base, struct nvkmd_windows_mem, base)->allocation.handle;
}

static const struct nvkmd_mem_ops win_mem_ops = {
   .free = win_free_mem,
   .map = win_map_mem,
   .unmap = win_unmap_mem,
   .log_handle = win_log_mem,
};

static VkResult
win_alloc_tiled_mem(struct nvkmd_dev *dev, struct vk_object_base *log, uint64_t size,
                    uint64_t alignment, uint8_t kind, uint16_t tile_mode,
                    enum nvkmd_mem_flags flags, struct nvkmd_mem **out)
{
   (void)tile_mode;
   if (flags & (NVKMD_MEM_SHARED | NVKMD_MEM_VRAM))
      return vk_error(log, VK_ERROR_FEATURE_NOT_PRESENT);

   struct nvkmd_windows_mem *mem = calloc(1, sizeof(*mem));
   if (!mem)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   struct tg_kmt_device *kmt = &nvkmd_windows_dev(dev)->kmt;
   const bool cpu_visible = flags & NVKMD_MEM_CAN_MAP;
   LONG status = tg_kmt_allocate(kmt, size, cpu_visible, &mem->allocation);
   if (status < 0) {
      free(mem);
      return nvkmd_windows_status(log, "KMT allocation and residency", status,
                                  VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   struct nvkmd_va *va = NULL;
   size = mem->allocation.size;
   VkResult result = win_alloc_va(dev, log, 0, kind, size, alignment, 0, &va);
   if (result != VK_SUCCESS)
      goto fail_allocation;

   nvkmd_mem_init(dev, &mem->base, &win_mem_ops,
                  flags | (cpu_visible ? NVKMD_MEM_COHERENT : 0),
                  size, TG_KMT_PAGE_SIZE);
   InitializeSRWLock(&mem->map_lock);
   mem->base.va = va;
   result = win_bind_mem(va, log, 0, &mem->base, 0, size);
   if (result != VK_SUCCESS) {
      win_free_mem(&mem->base);
      return result;
   }
   *out = &mem->base;
   return VK_SUCCESS;

fail_allocation:
   tg_kmt_free(kmt, &mem->allocation);
   free(mem);
   return result;
}

static VkResult
win_alloc_mem(struct nvkmd_dev *dev, struct vk_object_base *log, uint64_t size,
              uint64_t alignment, enum nvkmd_mem_flags flags, struct nvkmd_mem **out)
{
   return win_alloc_tiled_mem(dev, log, size, alignment, 0, 0, flags, out);
}

static VkResult
win_get_timestamp(struct nvkmd_dev *dev, uint64_t *timestamp)
{
   (void)dev;
   (void)timestamp;
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static void
win_destroy_device(struct nvkmd_dev *base)
{
   struct nvkmd_windows_dev *dev = nvkmd_windows_dev(base);
   tg_kmt_close_device(&dev->kmt);
   simple_mtx_destroy(&base->mems_mutex);
   free(dev);
}

static const struct nvkmd_dev_ops win_dev_ops = {
   .destroy = win_destroy_device,
   .get_gpu_timestamp = win_get_timestamp,
   .alloc_mem = win_alloc_mem,
   .alloc_tiled_mem = win_alloc_tiled_mem,
   .alloc_va = win_alloc_va,
   .create_ctx = nvkmd_windows_create_ctx,
};

static VkResult
win_create_device(struct nvkmd_pdev *pdev, struct vk_object_base *log, struct nvkmd_dev **out)
{
   struct nvkmd_windows_dev *dev = calloc(1, sizeof(*dev));
   if (!dev)
      return vk_error(log, VK_ERROR_OUT_OF_HOST_MEMORY);
   LONG status = tg_kmt_open_device(&dev->kmt, nvkmd_windows_adapter(pdev));
   if (status < 0) {
      free(dev);
      return nvkmd_windows_status(log, "open KMT device and paging queue", status,
                                  VK_ERROR_INITIALIZATION_FAILED);
   }
   dev->base = (struct nvkmd_dev) {
      .ops = &win_dev_ops, .pdev = pdev, .va_start = TG_KMT_VA_START, .va_end = TG_KMT_VA_END,
   };
   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   *out = &dev->base;
   return VK_SUCCESS;
}

static void
win_destroy_pdev(struct nvkmd_pdev *pdev)
{
   free(container_of(pdev, struct nvkmd_windows_pdev, base));
}

static const struct nvkmd_pdev_ops win_pdev_ops = {
   .destroy = win_destroy_pdev,
   .create_dev = win_create_device,
};

VkResult nvk_enumerate_windows_physical_devices(struct vk_instance *instance);

VkResult
nvk_enumerate_windows_physical_devices(struct vk_instance *vk)
{
   struct nvk_instance *instance = container_of(vk, struct nvk_instance, vk);
   struct tg_kmt_scan scan;
   LONG status = tg_gpu_kmt_scan_system(&scan);
   if (status < 0)
      return nvkmd_windows_status(&vk->base, "KMT adapter scan", status,
                                  VK_ERROR_INITIALIZATION_FAILED);
   if (!scan.native_ready)
      return VK_SUCCESS;

   struct nvkmd_windows_pdev *pdev = calloc(1, sizeof(*pdev));
   if (!pdev)
      return vk_error(vk, VK_ERROR_OUT_OF_HOST_MEMORY);
   pdev->adapter = scan.first_ready;
   pdev->base.ops = &win_pdev_ops;
   pdev->base.debug_flags = instance->debug_flags;
   pdev->base.bind_align_B = TG_KMT_PAGE_SIZE;
   pdev->base.dev_info = (struct nv_device_info) {
      .type = NV_DEVICE_TYPE_SOC, .device_id = 0x0fe0, .chipset = pdev->adapter.chipset,
      .device_name = "NVIDIA Tegra X1 (GM20B)", .chipset_name = "GM20B",
      .sm = pdev->adapter.sm, .gpc_count = 1, .tpc_count = 2, .mp_per_tpc = 1,
      .max_warps_per_mp = 64, .nc_atom_size_B = 128,
      .cls_eng3d = pdev->adapter.gr_class, .cls_compute = 0xb1c0,
      .cls_copy = 0xb0b5, .cls_eng2d = 0x902d, .cls_m2mf = 0xa140, .cls_gpfifo = 0xb06f,
      .max_smem_per_wg_kB = 48, .sm_smem_sizes_kB = {64}, .sm_smem_size_count = 1,
   };
   pdev->binary_type = vk_sync_binary_get_type(&nvkmd_windows_sync_type);
   pdev->sync_types[0] = &pdev->binary_type.sync;
   pdev->sync_types[1] = &nvkmd_windows_sync_type;
   pdev->base.sync_types = pdev->sync_types;

   struct vk_physical_device *device;
   VkResult result = nvk_create_physical_device_from_nvkmd(instance, &pdev->base, &device);
   if (result == VK_SUCCESS)
      list_addtail(&device->link, &vk->physical_devices.list);
   return result;
}
