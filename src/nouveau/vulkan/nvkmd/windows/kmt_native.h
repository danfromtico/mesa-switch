/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_KMT_NATIVE_H
#define TG_GPU_KMT_NATIVE_H

#include <stdbool.h>
#include "kmt.h"
#include "memory_budget.h"

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
   uint32_t resource;
   uint32_t flags;
};

struct tg_kmt_va {
   uint64_t address, size;
   uint64_t reserved_base, reserved_size;
};

/* A second device's reference to the same runtime-owned backing. It does not
 * allocate memory or own the producer's handles, and is not charged twice to
 * the local allocator's byte counter. VidMm still admits this device's own
 * residency reference. Close only after the actual submitting GPU completes. */
struct tg_kmt_runtime_alias {
   uint32_t device, resource, allocation;
};

LONG tg_kmt_open_runtime_alias(struct tg_kmt_device *dev, uint32_t resource,
                              uint64_t expected_bytes, struct tg_kmt_runtime_alias *alias);
/* Failed close preserves the handles. Open may also return a live alias when
 * its unwind fails; the caller must retain it for cleanup, never use it. */
LONG tg_kmt_close_runtime_alias(const struct tg_kmt_device *dev,
                               struct tg_kmt_runtime_alias *alias);

/* D3DKMT_QUERYSTATISTICS_SEGMENT outputs, unmodified. `properties` holds the
 * raw SegmentProperties bits; their layout depends on the OS WDDM level.
 */
struct tg_kmt_segment_statistics {
   uint64_t commit_limit, committed, resident, properties;
   uint32_t aperture;
};

/* Both queries open and close their own adapter handle and need no device.
 * A failure returns the first failing NTSTATUS and leaves the outputs zero.
 */
LONG tg_kmt_query_video_memory(const struct tg_gpu_adapter_info *adapter,
                               struct tg_kmt_video_memory groups[TG_KMT_MEMORY_GROUPS]);
LONG tg_kmt_query_segment(const struct tg_gpu_adapter_info *adapter, uint32_t segment,
                          struct tg_kmt_segment_statistics *statistics);

LONG tg_kmt_open_device(struct tg_kmt_device *dev,
                        const struct tg_gpu_adapter_info *adapter);
LONG tg_kmt_close_device(struct tg_kmt_device *dev);
LONG tg_kmt_create_context(const struct tg_kmt_device *dev, uint32_t *context);
LONG tg_kmt_destroy_context(uint32_t context);
LONG tg_kmt_allocate(struct tg_kmt_device *dev, uint64_t size, bool cpu_visible,
                     struct tg_kmt_allocation *allocation);
LONG tg_kmt_allocate_shared(struct tg_kmt_device *dev, uint64_t size, bool cpu_visible,
                            struct tg_kmt_allocation *allocation);
/* NT handles belong to the caller. An opened resource retains its own
 * reference and is released with tg_kmt_free, including partial failures.
 */
LONG tg_kmt_export_memory(const struct tg_kmt_allocation *allocation, HANDLE *handle);
LONG tg_kmt_export_memory_win32(const struct tg_kmt_allocation *allocation,
                               const SECURITY_ATTRIBUTES *security, DWORD access,
                               LPCWSTR name, HANDLE *handle);
LONG tg_kmt_open_memory_name(LPCWSTR name, HANDLE *handle);
LONG tg_kmt_import_memory(struct tg_kmt_device *dev, HANDLE handle,
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
                uint64_t offset, uint64_t size, uint8_t kind, bool read_only);
LONG tg_kmt_create_fence(const struct tg_kmt_device *dev, uint64_t initial,
                         struct tg_kmt_fence *fence);
LONG tg_kmt_create_shared_fence(const struct tg_kmt_device *dev, uint64_t initial,
                                struct tg_kmt_fence *fence);
/* Imports retain their own KMT reference without consuming the caller's NT
 * handle. Exported NT handles belong to the caller, including named exports. */
LONG tg_kmt_import_fence(const struct tg_kmt_device *dev, HANDLE handle,
                         LPCWSTR name, struct tg_kmt_fence *fence);
LONG tg_kmt_export_fence_win32(const struct tg_kmt_fence *fence,
                              const SECURITY_ATTRIBUTES *security, DWORD access,
                              LPCWSTR name, HANDLE *handle);
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
/* Allocation handles must belong to the device owning context. */
LONG tg_kmt_submit_primaries(uint32_t context, uint64_t address, const void *words,
                           uint32_t bytes, uint32_t count, const uint32_t *allocations);

#endif
