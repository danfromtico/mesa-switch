/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_KMT_NATIVE_H
#define TG_GPU_KMT_NATIVE_H

#include <stdbool.h>
#include "kmt.h"

#define TG_KMT_PAGE_SIZE UINT64_C(4096)
#define TG_KMT_VA_START UINT64_C(0x10000)
#define TG_KMT_VA_END (UINT64_C(1) << 39)

#define TG_KMT_TIMEOUT ((LONG)0x102)
#define TG_KMT_INVALID_PARAMETER ((LONG)0xc000000d)
#define TG_KMT_NO_MEMORY ((LONG)0xc0000017)
#define TG_KMT_NO_VIDEO_MEMORY ((LONG)0xc01e0100)
#define TG_KMT_DEVICE_REMOVED ((LONG)0xc00002b6)

struct tg_kmt_fence {
   uint32_t handle;
   const volatile uint64_t *value;
};

struct tg_kmt_device {
   uint32_t adapter, handle, paging_queue;
   struct tg_kmt_fence paging;
   SRWLOCK memory_lock;
   uint64_t budget, allocated;
};

struct tg_kmt_allocation {
   uint32_t handle;
   uint64_t size;
};

struct tg_kmt_va {
   uint64_t address, size;
   uint64_t reserved_base, reserved_size;
};

LONG tg_kmt_open_device(struct tg_kmt_device *dev,
                        const struct tg_gpu_adapter_info *adapter);
LONG tg_kmt_close_device(struct tg_kmt_device *dev);
LONG tg_kmt_create_context(const struct tg_kmt_device *dev, uint32_t *context);
LONG tg_kmt_destroy_context(uint32_t context);
LONG tg_kmt_allocate(struct tg_kmt_device *dev, uint64_t size, bool cpu_visible,
                     struct tg_kmt_allocation *allocation);
LONG tg_kmt_free(struct tg_kmt_device *dev, struct tg_kmt_allocation *allocation);
LONG tg_kmt_lock(const struct tg_kmt_device *dev,
                 const struct tg_kmt_allocation *allocation, void **cpu);
LONG tg_kmt_unlock(const struct tg_kmt_device *dev,
                   const struct tg_kmt_allocation *allocation);
LONG tg_kmt_reserve(const struct tg_kmt_device *dev, uint64_t size,
                    uint64_t alignment, uint64_t fixed, struct tg_kmt_va *va);
LONG tg_kmt_free_va(const struct tg_kmt_device *dev, struct tg_kmt_va *va);
LONG tg_kmt_map(const struct tg_kmt_device *dev, const struct tg_kmt_va *va,
                uint64_t va_offset, const struct tg_kmt_allocation *allocation,
                uint64_t offset, uint64_t size, uint8_t kind);
LONG tg_kmt_create_fence(const struct tg_kmt_device *dev, uint64_t initial,
                         struct tg_kmt_fence *fence);
LONG tg_kmt_destroy_fence(struct tg_kmt_fence *fence);
LONG tg_kmt_wait_fences(const struct tg_kmt_device *dev, uint32_t count,
                        const uint32_t *handles, const uint64_t *values,
                        bool any, DWORD timeout_ms);
LONG tg_kmt_signal_cpu(const struct tg_kmt_device *dev, uint32_t fence,
                       uint64_t value);
LONG tg_kmt_wait_gpu(uint32_t context, uint32_t count, const uint32_t *handles,
                     const uint64_t *values);
LONG tg_kmt_signal_gpu(uint32_t context, uint32_t count,
                       const uint32_t *handles, const uint64_t *values);
LONG tg_kmt_submit(uint32_t context, uint64_t address, const void *words,
                    uint32_t bytes);

#endif
