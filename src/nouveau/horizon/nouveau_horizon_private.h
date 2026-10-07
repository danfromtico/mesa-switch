/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef NOUVEAU_HORIZON_PRIVATE_H
#define NOUVEAU_HORIZON_PRIVATE_H 1

#include "nouveau_horizon.h"

#include "util/hash_table.h"
#include "util/list.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/vma.h"
#include "c11/threads.h"

#include <switch.h>

#include <stdarg.h>

#define NOUVEAU_HORIZON_BIND_ALIGN_B 0x10000u
#define NOUVEAU_HORIZON_WAIT_SLICE_COUNT 64u
#define NOUVEAU_HORIZON_CLIENT_WAIT_SLICE_COUNT \
   (NOUVEAU_HORIZON_WAIT_SLICE_COUNT - 1u)
#define NOUVEAU_HORIZON_ORDER_WAIT_SLICE \
   (NOUVEAU_HORIZON_WAIT_SLICE_COUNT - 1u)
#define NOUVEAU_HORIZON_WAIT_SLICE_WORDS 128u
#define NOUVEAU_HORIZON_BUILTIN_SIZE_B 0x10000u
/* Native pre/post fences and the ring's empty slot. */
#define NOUVEAU_HORIZON_GPFIFO_SUBMIT_CAPACITY (GPFIFO_QUEUE_SIZE - 3u)
#define NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT 512u
#define NOUVEAU_HORIZON_REPORT_SLICE_COUNT \
   NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT
#define NOUVEAU_HORIZON_REPORT_ALIGN_WORDS 4u
#define NOUVEAU_HORIZON_DEFAULT_SUBMIT_HIGH_WATERMARK 256u
#define NOUVEAU_HORIZON_DEFAULT_SUBMIT_LOW_WATERMARK 128u
#define NOUVEAU_HORIZON_DEFAULT_RESOURCE_WAIT_SLICE_NS UINT64_C(5000000)
#define NOUVEAU_HORIZON_MAX_RESOURCE_WAIT_SLICE_NS UINT64_C(50000000)
#define NOUVEAU_HORIZON_RESOURCE_RECOVERY_TIMEOUT_NS UINT64_C(10000000000)
#define NOUVEAU_HORIZON_SVC_INVALIDATE_PROCESS_DATA_CACHE 0x5d

/* Cache 64 KiB through 32 MiB size buckets; free larger allocations
 * directly. Byte and entry caps bound shared-heap and NvMap-handle usage.
 */
#define NOUVEAU_HORIZON_BO_CACHE_BUCKETS 10u
#define NOUVEAU_HORIZON_BO_CACHE_MAX_ENTRY_B (32ull << 20)
#define NOUVEAU_HORIZON_BO_CACHE_DEFAULT_MB 128
#define NOUVEAU_HORIZON_BO_CACHE_MAX_ENTRIES 256u

struct nouveau_horizon_runtime {
   uint32_t refcnt;
   bool initialized;

   /* Keep the shared GL/Vulkan ZBC snapshot with the process-wide libnx
    * runtime.
    */
   simple_mtx_t zbc_mutex;
   uint32_t zbc_sequence;
   struct nouveau_horizon_zbc_state zbc_state;
   uint64_t zbc_query_calls;
   uint64_t zbc_query_failures;
   uint64_t zbc_state_changes;
   uint64_t zbc_refresh_calls;
   uint64_t zbc_add_failures;

   simple_mtx_t memory_identity_mutex;
   struct hash_table_u64 *memory_identities;

   /* Every live device, so a process that is closing can give their GPU
    * address spaces back (nouveau_horizon_runtime_shutdown). */
   struct list_head devices;
};

struct nouveau_horizon_memory_identity {
   uint32_t refcnt;
   struct nouveau_horizon_runtime *runtime;
   NvMap map;
   uint32_t nvmap_id;
   void *cpu_addr;
   uint64_t size_B;
   uint32_t align_B;
   uint32_t flags;
   uint8_t backing_kind;
   struct nouveau_horizon_memory_layout layout;
   /* Part of the physical NvMap contract, so part of the recycling key. */
   bool cpu_cacheable;
   bool imported;
   bool registered;

   struct nouveau_horizon_device *accounting_device;
   uint32_t mapping_count;
};

struct nouveau_horizon_device {
   uint32_t refcnt;
   struct nouveau_horizon_runtime *runtime;
   struct list_head runtime_link;
   struct nouveau_horizon_logger logger;

   struct nv_device_info info;
   NvAddressSpace addr_space;
   uint32_t page_size_B;
   uint32_t bind_align_B;
   uint64_t va_start;
   uint64_t va_end;

   simple_mtx_t va_mutex;
   struct util_vma_heap va_heap;

   simple_mtx_t memory_mutex;
   struct list_head memories;
   uint64_t total_memory_B;
   uint64_t allocated_memory_B;
   uint64_t peak_allocated_memory_B;

   simple_mtx_t debug_stats_mutex;
   struct nouveau_horizon_device_debug_stats debug_stats;
   bool enable_timing;

   simple_mtx_t bo_cache_mutex;
   struct list_head bo_cache_buckets[NOUVEAU_HORIZON_BO_CACHE_BUCKETS];
   struct list_head bo_cache_lru;
   uint64_t bo_cache_cap_B;
   uint64_t bo_cache_held_B;
   uint32_t bo_cache_entry_count;
   uint64_t bo_cache_hits;
   uint64_t bo_cache_misses;
   uint64_t bo_cache_evictions;

   simple_mtx_t submit_mutex;
   simple_mtx_t channel_mutex;
   struct list_head channels;
   int lost;
   bool total_order_channels;
   bool global_fence_valid;
   struct nouveau_horizon_fence global_fence;
   uint64_t global_channel_id;
   uint64_t next_channel_id;
};

struct nouveau_horizon_memory {
   uint32_t refcnt;
   struct nouveau_horizon_device *device;
   struct list_head device_link;
   struct nouveau_horizon_memory_identity *identity;
   uint32_t flags;
   bool imported;
   bool registered;
};

struct nouveau_horizon_va_mapping {
   struct list_head link;
   uint64_t addr;
   uint64_t range_B;
   struct nouveau_horizon_memory *memory;
   uint8_t pte_kind;
};

struct nouveau_horizon_va {
   uint32_t refcnt;
   struct nouveau_horizon_device *device;
   simple_mtx_t mutex;
   struct list_head mappings;
   uint64_t addr;
   uint64_t size_B;
   uint32_t flags;
   bool has_pte_kind;
   uint8_t pte_kind;
   bool quarantined;
};

struct nouveau_horizon_channel {
   uint32_t refcnt;
   struct nouveau_horizon_device *device;
   mtx_t mutex;
   struct list_head device_link;
   uint64_t id;
   uint32_t syncpoint_id;

   NvGpuChannel gpu_channel;
   bool channel_ready;
   bool registered;
   bool quarantined;
   bool pending_work;
   bool cache_acquire_emitted;
   bool last_fence_valid;
   bool last_fence_cpu_visible;
   bool lost;
   bool native_error_logged;
   uint64_t pending_dwords;
   uint64_t pending_command_bytes;
   struct nouveau_horizon_fence last_fence;
   struct nouveau_horizon_error error;

   struct nouveau_horizon_memory *builtin_memory;
   struct nouveau_horizon_va *builtin_va;
   uint32_t *builtin_cpu;
   uint64_t builtin_addr;

   uint32_t cache_acquire_offset_words;
   uint32_t cache_acquire_words;
   uint32_t cache_acquire_sync_offset_words;
   uint32_t full_barrier_offset_words;
   uint32_t full_barrier_words;
   uint32_t full_barrier_acquire_offset_words;
   uint32_t full_barrier_acquire_words;
   uint32_t gpu_fence_offset_words;
   uint32_t gpu_fence_words;
   uint32_t cpu_fence_offset_words;
   uint32_t cpu_fence_words;

   /* CPU/GPU-uncached report storage. Each accepted submission retains its
    * matching ring slice until the mapped completion is observed; never
    * reuse it earlier.
    */
   bool mapped_completion_enabled;
   uint32_t report_offset_words;
   volatile uint32_t *report_cpu;
   uint64_t report_addr;
   uint32_t report_slices_offset_words;
   uint32_t next_report_value;

   NvFence wait_slice_fences[NOUVEAU_HORIZON_WAIT_SLICE_COUNT];
   uint64_t pending_wait_slices;
   uint32_t next_wait_slice;

   /* Keep accepted jobs below nvhost's finite in-flight submission budget.
    * Unlike the command GPFIFO, this ring tracks kernel-accepted batches and
    * the exact entry/command-byte credits retained by each batch.
    */
   struct {
      struct nouveau_horizon_fence fence;
      uint64_t entries;
      uint64_t command_bytes;
      uint32_t report_value;
      uint32_t report_slice;
   } inflight_submissions[NOUVEAU_HORIZON_INFLIGHT_SUBMISSION_COUNT];
   uint32_t inflight_head;
   uint32_t inflight_count;
   uint64_t inflight_entries;
   uint64_t inflight_command_bytes;
   uint32_t inflight_submit_high_watermark;
   uint32_t inflight_submit_low_watermark;
   uint64_t inflight_entry_high_watermark;
   uint64_t inflight_entry_low_watermark;
   uint64_t inflight_command_byte_high_watermark;
   uint64_t inflight_command_byte_low_watermark;
   uint64_t resource_wait_slice_ns;

   struct nouveau_horizon_channel_stats stats;
};

void
nouveau_horizon_log(struct nouveau_horizon_device *device,
                    enum nouveau_horizon_log_level level,
                    const char *format, ...) PRINTFLIKE(3, 4);

enum nouveau_horizon_status
nouveau_horizon_status_from_result(Result rc,
                                   enum nouveau_horizon_status fallback);

void
nouveau_horizon_error_reset(struct nouveau_horizon_error *error);

void
nouveau_horizon_error_set(struct nouveau_horizon_error *error,
                          enum nouveau_horizon_status status,
                          Result native_result);

uint64_t
nouveau_horizon_align_u64(uint64_t value, uint64_t align);

void
nouveau_horizon_runtime_refresh_zbc(
   struct nouveau_horizon_runtime *runtime);

void
nouveau_horizon_runtime_get_zbc_state(
   struct nouveau_horizon_runtime *runtime,
   struct nouveau_horizon_zbc_state *state_out);

NvMap *
nouveau_horizon_memory_get_native_map(
   struct nouveau_horizon_memory *memory);

bool
nouveau_horizon_memory_is_gpu_cacheable(
   struct nouveau_horizon_memory *memory);

uint32_t
nouveau_horizon_device_bind_align(
   struct nouveau_horizon_device *device);

void
nouveau_horizon_device_bo_cache_init(
   struct nouveau_horizon_device *device);

/* Drops every cached backing store.  Called on allocation failure so a
 * populated cache can never turn a recoverable allocation into an OOM, and
 * before device teardown.
 */
void
nouveau_horizon_device_bo_cache_trim(
   struct nouveau_horizon_device *device);

void
nouveau_horizon_device_bo_cache_finish(
   struct nouveau_horizon_device *device);

void
nouveau_horizon_device_account_alloc(
   struct nouveau_horizon_device *device, uint64_t size_B);

void
nouveau_horizon_device_account_free(
   struct nouveau_horizon_device *device, uint64_t size_B);

void
nouveau_horizon_device_record_fence_wait(
   struct nouveau_horizon_device *device,
   enum nouveau_horizon_status status, uint64_t elapsed_ns);

void
nouveau_horizon_device_record_cache_sync(
   struct nouveau_horizon_device *device, bool to_gpu,
   uint64_t bytes, uint64_t elapsed_ns);

enum nouveau_horizon_status
nouveau_horizon_memory_acquire_mapping(
   struct nouveau_horizon_memory *memory, uint8_t pte_kind);

void
nouveau_horizon_memory_release_mapping(
   struct nouveau_horizon_memory *memory, uint8_t pte_kind);

void
nouveau_horizon_device_mark_lost(struct nouveau_horizon_device *device);

bool
nouveau_horizon_device_is_lost(struct nouveau_horizon_device *device);

static inline NvFence
nouveau_horizon_native_fence(struct nouveau_horizon_fence fence)
{
   return (NvFence) { .id = fence.id, .value = fence.value };
}

static inline struct nouveau_horizon_fence
nouveau_horizon_public_fence(NvFence fence)
{
   return (struct nouveau_horizon_fence) {
      .id = fence.id,
      .value = fence.value,
   };
}

#endif /* NOUVEAU_HORIZON_PRIVATE_H */
