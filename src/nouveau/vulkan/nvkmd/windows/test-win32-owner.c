/* SPDX-License-Identifier: MIT */
/* The production memory owner implementation, with KMT transport substitutes.
 * No adapter, Vulkan device or GPU is created by this control. */
#include "nvkmd_windows.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static struct {
   unsigned refs, aliases, opens, closes, maps, frees;
   unsigned failure;
   bool fail_close, fail_device;
} seen;
static char backing[8192];

LONG tg_kmt_open_device(struct tg_kmt_device *dev, const struct tg_gpu_adapter_info *info)
{
   (void)info;
   *dev = (struct tg_kmt_device){.handle = 2, .adapter = 1, .paging_queue = 3};
   InitializeSRWLock(&dev->memory_lock); return 0;
}
LONG tg_kmt_close_device(struct tg_kmt_device *dev)
{
   CHECK(dev->handle == 2);
   if (seen.fail_device) return TG_KMT_DEVICE_REMOVED;
   seen.aliases = 0; *dev = (struct tg_kmt_device){0}; return 0;
}
LONG tg_kmt_open_runtime_alias(struct tg_kmt_device *dev, uint32_t resource,
                              uint64_t bytes, struct tg_kmt_runtime_alias *alias)
{
   CHECK(dev->handle == 2 && resource == 90 && bytes == sizeof(backing) && !seen.aliases);
   ++seen.opens;
   *alias = (struct tg_kmt_runtime_alias){2, 101, 201}; ++seen.aliases;
   return seen.failure == 1 ? TG_KMT_DEVICE_REMOVED : 0;
}
LONG tg_kmt_close_runtime_alias(const struct tg_kmt_device *dev, struct tg_kmt_runtime_alias *alias)
{
   if (!alias->resource && !alias->allocation) return 0;
   CHECK(dev->handle == 2 && alias->device == 2 && alias->resource == 101 && alias->allocation == 201);
   ++seen.closes;
   if (seen.fail_close) return TG_KMT_DEVICE_REMOVED;
   CHECK(seen.aliases); --seen.aliases; *alias = (struct tg_kmt_runtime_alias){0}; return 0;
}
LONG tg_kmt_reserve(const struct tg_kmt_device *dev, uint64_t size, uint64_t alignment,
                    uint64_t fixed, struct tg_kmt_va *va)
{
   CHECK(dev->handle == 2 && size == sizeof(backing) && alignment == 4096 && !fixed);
   if (seen.failure == 2) return TG_KMT_NO_MEMORY;
   *va = (struct tg_kmt_va){0x100000, size, 0x100000, size}; return 0;
}
LONG tg_kmt_free_va(const struct tg_kmt_device *dev, struct tg_kmt_va *va)
{
   (void)dev; (void)va; CHECK(false); return TG_KMT_INVALID_PARAMETER;
}
#define UNEXPECTED() do { CHECK(false); return TG_KMT_INVALID_PARAMETER; } while (0)
LONG tg_kmt_free(struct tg_kmt_device *d, struct tg_kmt_allocation *a) { UNEXPECTED(); }
LONG tg_kmt_lock(const struct tg_kmt_device *d, const struct tg_kmt_allocation *a, void **p) { UNEXPECTED(); }
LONG tg_kmt_unlock(const struct tg_kmt_device *d, const struct tg_kmt_allocation *a) { UNEXPECTED(); }
LONG tg_kmt_allocate(struct tg_kmt_device *d, uint64_t s, bool c, struct tg_kmt_allocation *a) { UNEXPECTED(); }
LONG tg_kmt_allocate_shared(struct tg_kmt_device *d, uint64_t s, bool c, struct tg_kmt_allocation *a) { UNEXPECTED(); }
LONG tg_kmt_import_memory(struct tg_kmt_device *d, HANDLE h, struct tg_kmt_allocation *a) { UNEXPECTED(); }
LONG tg_kmt_export_memory(const struct tg_kmt_allocation *a, HANDLE *h) { UNEXPECTED(); }
LONG tg_kmt_export_memory_win32(const struct tg_kmt_allocation *a, const SECURITY_ATTRIBUTES *s,
                               DWORD access, LPCWSTR name, HANDLE *h) { UNEXPECTED(); }
LONG tg_kmt_map(const struct tg_kmt_device *d, const struct tg_kmt_va *v, uint64_t vo,
                const struct tg_kmt_allocation *a, uint64_t o, uint64_t s, uint8_t k, bool r) { UNEXPECTED(); }
LONG tg_gpu_kmt_scan_system(struct tg_kmt_scan *scan) { UNEXPECTED(); }
VkResult nvkmd_windows_create_ctx(struct nvkmd_dev *dev, struct vk_object_base *log,
                                 enum nvkmd_engines engines, struct nvkmd_ctx **out)
{
   CHECK(false); return VK_ERROR_FEATURE_NOT_PRESENT;
}
static void retain(void *context) { CHECK(context == &seen.refs); ++seen.refs; }
static void release(void *context)
{
   CHECK(context == &seen.refs && seen.refs);
   if (seen.refs == 1) CHECK(!seen.aliases);
   --seen.refs;
}
static VkResult map_gpu(void *context, uint64_t address, uint64_t offset, uint64_t bytes,
                        uint8_t kind, bool read_only, uint64_t reserved, uint64_t reserved_bytes)
{
   CHECK(context == &seen.refs && address == 0x100000 && !offset && bytes == sizeof(backing) &&
         kind == 0xfe && !read_only && reserved == address && reserved_bytes == bytes);
   ++seen.maps; return seen.failure == 3 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult free_gpu(void *context, uint64_t address, uint64_t bytes)
{
   CHECK(context == &seen.refs && address == 0x100000 && bytes == sizeof(backing)); ++seen.frees;
   return VK_SUCCESS;
}
static VkResult map_cpu(void *context, void **cpu) { CHECK(context == &seen.refs); *cpu = backing; return VK_SUCCESS; }
static void unmap_cpu(void *context) { CHECK(context == &seen.refs); }

int main(void)
{
   struct nvkmd_windows_pdev pdev = {.adapter.luid_low = 0x55};
   for (unsigned mode = 0; mode < 8; ++mode) {
      CHECK(!seen.refs && !seen.aliases); memset(&seen, 0, sizeof(seen));
      struct nvkmd_dev *base = NULL;
      CHECK(win_create_device(&pdev.base, NULL, &base) == VK_SUCCESS);
      struct nvkmd_windows_dev *dev = nvkmd_windows_dev(base);
      struct wsi_win32_memory owner = {
         .version = mode ? WSI_WIN32_MEMORY_DISPLAYABLE_VERSION : WSI_WIN32_MEMORY_VERSION,
         .allocation = 60, .adapter_luid = 0x55, .size = sizeof(backing),
         .context = &seen.refs, .retain = retain, .release = release,
         .map_gpu = map_gpu, .free_gpu = free_gpu, .map_cpu = map_cpu, .unmap_cpu = unmap_cpu,
         .kernel_resource = 90,
      };
      seen.failure = mode >= 2 && mode <= 4 ? mode - 1 : 0;
      if (mode == 5) owner.kernel_resource = 0;
      struct nvkmd_mem *memory = NULL;
      VkResult result = nvkmd_dev_import_wsi_win32_memory(base, NULL, &owner, sizeof(backing),
                                                        4096, 0xfe, NVKMD_MEM_CAN_MAP, &memory);
      if (mode >= 2 && mode <= 5) {
         CHECK(result != VK_SUCCESS && !memory && !seen.refs && !seen.aliases);
         if (mode == 5) CHECK(!seen.opens);
      } else {
         CHECK(result == VK_SUCCESS && memory && seen.refs == 2);
         CHECK(nvkmd_windows_written_allocation(memory) == (mode ? 201u : 0u));
         void *cpu = NULL;
         CHECK(nvkmd_mem_map(memory, NULL, NVKMD_MEM_MAP_RD, NULL, &cpu) == VK_SUCCESS && cpu == backing);
         nvkmd_mem_unmap(memory, 0);
         seen.fail_close = mode >= 6;
         nvkmd_mem_unref(memory);
         if (mode >= 6) {
            CHECK(seen.refs == 1 && seen.aliases == 1 && dev->pending_aliases);
            seen.fail_device = mode == 7;
            if (seen.fail_device) {
               win_destroy_device(base);
               CHECK(seen.refs == 1 && dev->kmt.handle == 2 && dev->pending_aliases);
               seen.fail_device = false;
            }
            /* With close still failing, successful device teardown releases
             * the remaining kernel aliases before the retained source owner. */
         } else CHECK(!seen.refs && !seen.aliases);
      }
      win_destroy_device(base);
      CHECK(!seen.refs && !seen.aliases);
   }
   /* Runtime-owned submission must use the original allocation, without
    * calling ShareObjects or reopening an NT resource on the allocator. */
   for (unsigned mode = 0; mode < 4; ++mode) {
      memset(&seen, 0, sizeof(seen));
      struct nvkmd_dev *base = NULL;
      CHECK(win_create_device(&pdev.base, NULL, &base) == VK_SUCCESS);
      struct nvkmd_windows_dev *dev = nvkmd_windows_dev(base);
      dev->runtime.version = mode == 1 ? 0 : WSI_WIN32_RUNTIME_VERSION;
      dev->runtime.device_cookie = &seen;
      struct wsi_win32_memory owner = {
         .version = WSI_WIN32_MEMORY_RUNTIME_VERSION,
         .allocation = 60, .adapter_luid = 0x55, .size = sizeof(backing),
         .context = &seen.refs, .retain = retain, .release = release,
         .map_gpu = map_gpu, .free_gpu = free_gpu, .map_cpu = map_cpu, .unmap_cpu = unmap_cpu,
         .runtime_device = mode == 2 ? (void *)&pdev : (void *)&seen,
      };
      if (mode == 3) owner.kernel_resource = 90;
      struct nvkmd_mem *memory = NULL;
      VkResult result = nvkmd_dev_import_wsi_win32_memory(base, NULL, &owner, sizeof(backing),
                                                        4096, 0xfe, NVKMD_MEM_CAN_MAP, &memory);
      if (mode) CHECK(result == VK_ERROR_INVALID_EXTERNAL_HANDLE && !memory && !seen.refs);
      else {
         CHECK(result == VK_SUCCESS && nvkmd_windows_written_allocation(memory) == 60);
         CHECK(seen.refs == 2 && seen.maps == 1);
         nvkmd_mem_unref(memory);
      }
      CHECK(!seen.opens && !seen.aliases && !seen.refs);
      win_destroy_device(base);
   }
   puts("PASS: production NVK owner import v1/v2, alias acknowledgement, mapping failures, retained failed cleanup and device teardown (KMT mocked, no GPU)");
   puts("PASS: runtime owner v3 uses same-device allocation without NT export; absent/foreign transport and mixed ownership rejected (CPU control)");
   return 0;
}
