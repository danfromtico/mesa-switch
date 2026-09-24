/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef NOUVEAU_HORIZON_H
#define NOUVEAU_HORIZON_H 1

#include "nouveau/headers/nv_device_info.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nouveau_horizon_channel;
struct nouveau_horizon_device;
struct nouveau_horizon_memory;
struct nouveau_horizon_runtime;
struct nouveau_horizon_va;

/* Ownership and threading:
 *
 * Successful get/create/import returns one strong reference; ref/put
 * adjusts it. Devices retain the runtime; memory, VA and channels retain
 * their device; bindings retain memory. Refcounts are thread-safe and
 * device/VA/channel operations are serialized. Callers must retain objects
 * throughout each call.
 *
 * Channel teardown submits and waits before freeing command storage, or
 * quarantines the channel and GPU-visible storage if completion is
 * unknown. VA teardown unmaps bindings before releasing memory.
 */

enum nouveau_horizon_status {
   NOUVEAU_HORIZON_SUCCESS = 0,
   NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT = -1,
   NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY = -2,
   NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY = -3,
   NOUVEAU_HORIZON_ERROR_NOT_SUPPORTED = -4,
   NOUVEAU_HORIZON_ERROR_TIMEOUT = -5,
   NOUVEAU_HORIZON_ERROR_DEVICE_LOST = -6,
   NOUVEAU_HORIZON_ERROR_NO_SPACE = -7,
   NOUVEAU_HORIZON_ERROR_BUSY = -8,
   NOUVEAU_HORIZON_ERROR_SYSTEM = -9,
};

const char *
nouveau_horizon_status_string(enum nouveau_horizon_status status);

enum nouveau_horizon_log_level {
   NOUVEAU_HORIZON_LOG_DEBUG,
   NOUVEAU_HORIZON_LOG_INFO,
   NOUVEAU_HORIZON_LOG_WARNING,
   NOUVEAU_HORIZON_LOG_ERROR,
};

typedef void (*nouveau_horizon_log_func)(
   void *data, enum nouveau_horizon_log_level level, const char *message);

struct nouveau_horizon_logger {
   nouveau_horizon_log_func log;
   void *data;
};

/* Native error details are copied into an adapter-neutral structure.  The
 * native_result field contains the Horizon Result value for diagnostics only;
 * consumers should make decisions using status.
 */
struct nouveau_horizon_error {
   enum nouveau_horizon_status status;
   uint32_t native_result;
   uint64_t notification_timestamp;
   uint32_t notification_type;
   uint16_t notification_info;
   uint16_t notification_status;
   uint32_t channel_error_type;
   uint32_t channel_error_info[31];
};

struct nouveau_horizon_fence {
   uint32_t id;
   uint32_t value;
};

#define NOUVEAU_HORIZON_INVALID_FENCE_ID UINT32_MAX

static inline bool
nouveau_horizon_fence_is_valid(const struct nouveau_horizon_fence *fence)
{
   return fence != NULL && fence->id != NOUVEAU_HORIZON_INVALID_FENCE_ID;
}

struct nouveau_horizon_device_create_info {
   /* Insert a GPU wait on the device's previous successful submission for
    * clients without per-resource cross-channel dependencies.
    */
   bool total_order_channels;

   /* Enable optional diagnostics: aggregate counters, CPU-side timing and
    * informational lifecycle messages.  Retail applications leave this
    * false, so hot paths retain only state required for correctness and
    * warnings/errors.
    */
   bool enable_timing;
   struct nouveau_horizon_logger logger;
};

struct nouveau_horizon_device_properties {
   uint32_t page_size_B;
   uint32_t bind_align_B;
   uint64_t va_start;
   uint64_t va_end;
   bool has_compression;
   bool total_order_channels;
};

struct nouveau_horizon_memory_info {
   uint64_t total_B;
   uint64_t available_B;
   uint64_t allocated_B;
   uint64_t peak_allocated_B;
};

/* GM20B has one process-wide ZBC active-slot mask. Program its inverse
 * into both color and depth methods. A disabled snapshot masks all slots
 * for ordinary uncompressed clears.
 */
#define NOUVEAU_HORIZON_ZBC_SLOT_COUNT 15u

struct nouveau_horizon_zbc_state {
   uint32_t active_slot_mask;
   uint32_t slot_disable_mask;
   uint32_t queried_slot;
   bool enabled;
   uint64_t generation;
};

enum nouveau_horizon_memory_flags {
   NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE = 1u << 0,
   NOUVEAU_HORIZON_MEMORY_CPU_CACHED = 1u << 1,
   NOUVEAU_HORIZON_MEMORY_GPU_CACHED = 1u << 2,
   NOUVEAU_HORIZON_MEMORY_ZERO = 1u << 3,
};

struct nouveau_horizon_memory_layout {
   bool valid;
   uint8_t pte_kind;
   uint16_t tile_mode;
};

struct nouveau_horizon_memory_create_info {
   uint64_t size_B;
   uint64_t align_B;

   /* Physical NvMap kind.  Keep this independent from the PTE kind selected
    * on a VA object.  Pitch (zero) is the normal physical backing kind even
    * for block-linear or compressed GPU mappings.
    */
   uint8_t backing_kind;
   uint32_t flags;

   /* Immutable layout required for exportable allocations: other devices
    * must not reinterpret the NvMap's PTE kind or tiling. Private
    * allocations may leave valid false.
    */
   struct nouveau_horizon_memory_layout layout;

   /* When set, wrap this caller-owned range instead of allocating.  Pointer
    * and size_B must be 4 KiB aligned and outlive the identity; the range is
    * never freed, recycled or cleared. Without CPU_CACHED, the range is made
    * uncached for the identity's lifetime and restored to cached on release.
    */
   void *import_host_ptr;
};

struct nouveau_horizon_memory_import_info {
   uint32_t nvmap_id;

   /* Restrict legacy ID-only imports to identities registered in this
    * runtime, reusing their canonical metadata.
    */
   bool require_existing;

   /* Required unless require_existing is set. Supply all metadata exactly;
    * expected_size_B may be zero to accept the kernel-reported size.
    */
   bool has_metadata;
   uint64_t expected_size_B;
   uint8_t backing_kind;
   /* Exact cross-device GPU mapping flags.  Only GPU_CACHED is accepted for
    * imports; a newly created import wrapper has no CPU mapping, so producer
    * CPU cacheability is intentionally not guessed.
    */
   uint32_t flags;
   struct nouveau_horizon_memory_layout layout;
};

enum nouveau_horizon_va_flags {
   NOUVEAU_HORIZON_VA_FIXED = 1u << 0,
   NOUVEAU_HORIZON_VA_SPARSE = 1u << 1,
};

struct nouveau_horizon_va_create_info {
   uint64_t size_B;
   uint64_t align_B;
   uint64_t fixed_addr;
   uint32_t flags;

   /* When false, mappings inherit the memory object's physical backing kind.
    * When true, pte_kind is applied only to the GPU VA mapping.
    */
   bool has_pte_kind;
   uint8_t pte_kind;
};

enum nouveau_horizon_channel_priority {
   NOUVEAU_HORIZON_CHANNEL_PRIORITY_LOW,
   NOUVEAU_HORIZON_CHANNEL_PRIORITY_MEDIUM,
   NOUVEAU_HORIZON_CHANNEL_PRIORITY_HIGH,
};

enum nouveau_horizon_mapped_completion_mode {
   NOUVEAU_HORIZON_MAPPED_COMPLETION_DISABLED,

   /* SET_REPORT_SEMAPHORE is a 3D-class method on GM20B subchannel zero.
    * Select this only when the client guarantees that class/subchannel bind;
    * video, copy-only and otherwise untyped channels must stay disabled.
    */
   NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0,
};

struct nouveau_horizon_channel_create_info {
   enum nouveau_horizon_channel_priority priority;

   /* Mapped progress avoids native polling ioctls; blocking waits and
    * fault detection still use syncpoints. Enable only for a validated
    * GM20B 3D channel bound on subchannel zero. Other layouts must use
    * DISABLED.
    */
   enum nouveau_horizon_mapped_completion_mode mapped_completion_mode;

   /* Limits on kernel-accepted work. Zero selects submission defaults (256
    * high/128 low). Zero entry/byte high watermarks disable those limits,
    * not occupancy tracking. A zero low watermark defaults to half its
    * nonzero high watermark.
    */
   uint32_t inflight_submit_high_watermark;
   uint32_t inflight_submit_low_watermark;
   uint64_t inflight_entry_high_watermark;
   uint64_t inflight_entry_low_watermark;
   uint64_t inflight_command_byte_high_watermark;
   uint64_t inflight_command_byte_low_watermark;

   /* Wait slice under native resource pressure; zero selects 5 ms. Each
    * slice checks channel errors within the overall recovery watchdog.
    */
   uint64_t resource_wait_slice_ns;
};

struct nouveau_horizon_exec {
   uint64_t addr;
   uint32_t size_B;
   bool incomplete;
   bool no_prefetch;
};

enum nouveau_horizon_completion_mode {
   /* Queue ordering only. */
   NOUVEAU_HORIZON_COMPLETION_GPU,

   /* Completion may be observed by the CPU or compositor and includes the
    * GM20B cache-clean syncpoint workaround.
    */
   NOUVEAU_HORIZON_COMPLETION_CPU,
};

struct nouveau_horizon_channel_stats {
   uint64_t submissions;
   uint64_t submitted_entries;
   uint64_t submitted_dwords;
   uint64_t waits_enqueued;
   uint64_t full_barriers_enqueued;
   uint64_t inflight_throttle_waits;
   uint64_t peak_inflight_submissions;
   uint64_t current_inflight_submissions;
   uint64_t current_inflight_entries;
   uint64_t current_inflight_command_bytes;
   uint64_t peak_inflight_entries;
   uint64_t peak_inflight_command_bytes;
   uint64_t inflight_retired_submissions;
   uint64_t inflight_proactive_polls;
   uint64_t inflight_proactive_retired_submissions;
   uint64_t inflight_native_pressure_polls;
   uint64_t inflight_native_pressure_retired_submissions;
   uint64_t inflight_credit_waits;
   uint64_t inflight_submission_watermark_waits;
   uint64_t inflight_entry_watermark_waits;
   uint64_t inflight_command_byte_watermark_waits;
   uint64_t resource_recovery_waits;
   uint64_t resource_retries;
   uint64_t submit_failures;

   uint64_t mapped_completion_polls;
   uint64_t mapped_completion_hits;
   uint64_t mapped_completion_retired_submissions;
   uint64_t mapped_completion_native_fallbacks;
   uint64_t mapped_completion_report_lag_events;

   uint64_t exec_calls;
   uint64_t exec_cpu_ns;
   uint64_t exec_max_cpu_ns;
   uint64_t submit_calls;
   uint64_t submit_cpu_ns;
   uint64_t submit_max_cpu_ns;
   uint64_t channel_lock_wait_ns;
   uint64_t channel_lock_max_wait_ns;
   uint64_t submit_lock_wait_ns;
   uint64_t submit_lock_max_wait_ns;
   uint64_t kickoff_calls;
   uint64_t kickoff_cpu_ns;
   uint64_t kickoff_max_cpu_ns;
   uint64_t inflight_throttle_wait_ns;
   uint64_t inflight_throttle_max_wait_ns;
   uint64_t inflight_credit_wait_ns;
   uint64_t inflight_credit_max_wait_ns;
   uint64_t inflight_submission_watermark_wait_ns;
   uint64_t inflight_entry_watermark_wait_ns;
   uint64_t inflight_command_byte_watermark_wait_ns;
   uint64_t resource_recovery_wait_ns;
   uint64_t resource_recovery_max_wait_ns;
   uint64_t inflight_native_pressure_poll_ns;
   uint64_t inflight_native_pressure_poll_max_ns;
   uint64_t mapped_completion_poll_ns;
   uint64_t mapped_completion_poll_max_ns;
   uint64_t mapped_completion_report_lag_wait_ns;
   uint64_t mapped_completion_report_lag_max_wait_ns;
};

/* Process/backend diagnostics.  These fields are populated only when
 * device_create_info::enable_timing is true.  They are intentionally not
 * correctness or lifetime state.
 */
struct nouveau_horizon_device_debug_stats {
   uint64_t memory_create_calls;
   uint64_t memory_create_failures;
   uint64_t memory_import_calls;
   uint64_t memory_import_failures;
   uint64_t native_memories_live;
   uint64_t native_memories_peak;
   /* Live native allocations grouped as <=64 KiB, <=256 KiB, <=1 MiB,
    * <=4 MiB and >4 MiB.  These bins distinguish object-count pressure from
    * byte pressure and guide any future backing-pool design.
    */
   uint64_t native_memory_live_by_size[5];
   uint64_t native_memory_created_by_size[5];
   uint64_t memory_wrappers_live;
   uint64_t memory_wrappers_peak;

   uint64_t va_create_calls;
   uint64_t va_create_failures;
   uint64_t vas_live;
   uint64_t vas_peak;
   uint64_t va_bind_calls;
   uint64_t va_bind_failures;
   uint64_t mappings_live;
   uint64_t mappings_peak;

   uint64_t fence_wait_calls;
   uint64_t fence_wait_timeouts;
   uint64_t fence_wait_failures;
   uint64_t fence_wait_ns;
   uint64_t fence_wait_max_ns;

   uint64_t cache_to_gpu_calls;
   uint64_t cache_to_gpu_bytes;
   uint64_t cache_to_gpu_ns;
   uint64_t cache_to_gpu_max_ns;
   uint64_t cache_from_gpu_calls;
   uint64_t cache_from_gpu_bytes;
   uint64_t cache_from_gpu_ns;
   uint64_t cache_from_gpu_max_ns;

   /* Process-wide ZBC state and per-device activity. Query errors disable
    * all slots without marking device loss. Registration requires a
    * validated value/format contract.
    */
   uint32_t zbc_active_slot_mask;
   uint32_t zbc_slot_disable_mask;
   uint32_t zbc_queried_slot;
   bool zbc_enabled;
   uint64_t zbc_generation;
   uint64_t zbc_query_calls;
   uint64_t zbc_query_failures;
   uint64_t zbc_state_changes;
   uint64_t zbc_refresh_calls;
   uint64_t zbc_programs;
   uint64_t zbc_add_failures;

   /* Backing-store cache statistics. Reusing 64 KiB-aligned allocations
    * reduces shared-heap fragmentation and NvMap churn.
    */
   uint64_t bo_cache_hits;
   uint64_t bo_cache_misses;
   uint64_t bo_cache_evictions;
   uint64_t bo_cache_held_B;
   uint64_t bo_cache_entries;
};

/* The runtime is a process-wide, reference-counted libnx service guard. */
enum nouveau_horizon_status
nouveau_horizon_runtime_get(struct nouveau_horizon_runtime **runtime_out);

struct nouveau_horizon_runtime *
nouveau_horizon_runtime_ref(struct nouveau_horizon_runtime *runtime);

void
nouveau_horizon_runtime_put(struct nouveau_horizon_runtime *runtime);

/* Closes the driver sessions whatever still references them, so nvservices
 * gives back every buffer and mapping this process holds. For a process that is
 * closing: nothing may touch the GPU afterwards. step, which may be NULL, is
 * told which session is about to be closed. */
void
nouveau_horizon_runtime_shutdown(void (*step)(const char *what));

/* Fill all GM20B information which is independent of a logical address
 * space.  This call does not acquire libnx services.
 */
void
nouveau_horizon_get_gm20b_info(struct nv_device_info *info_out);

enum nouveau_horizon_status
nouveau_horizon_device_create(
   struct nouveau_horizon_runtime *runtime,
   const struct nouveau_horizon_device_create_info *create_info,
   struct nouveau_horizon_device **device_out);

struct nouveau_horizon_device *
nouveau_horizon_device_ref(struct nouveau_horizon_device *device);

void
nouveau_horizon_device_put(struct nouveau_horizon_device *device);

const struct nv_device_info *
nouveau_horizon_device_get_info(struct nouveau_horizon_device *device);

void
nouveau_horizon_device_get_properties(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_device_properties *properties_out);

void
nouveau_horizon_device_get_memory_info(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_memory_info *memory_info_out);

void
nouveau_horizon_device_get_debug_stats(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_device_debug_stats *stats_out);

/* Copy the process-wide ZBC snapshot.  Adapters may call this at cheap 3D
 * work boundaries and reprogram both disable-mask methods only when the
 * generation differs from their channel-local cached generation.
 */
void
nouveau_horizon_device_get_zbc_state(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_zbc_state *state_out);

/* Single atomic load for hot paths.  Fetch the full seqlock snapshot only
 * when this value differs from the channel-local generation.
 */
uint64_t
nouveau_horizon_device_get_zbc_generation(
   struct nouveau_horizon_device *device);

/* Re-query the public libnx active-slot mask and publish a new generation
 * when its effective state changes.  Query failure publishes the safe
 * all-disabled state and is non-fatal to the device.
 */
void
nouveau_horizon_device_refresh_zbc_state(
   struct nouveau_horizon_device *device);

/* Adapter telemetry hook called after both 3D disable-mask methods have been
 * emitted for a channel generation.
 */
void
nouveau_horizon_device_record_zbc_program(
   struct nouveau_horizon_device *device);

enum nouveau_horizon_status
nouveau_horizon_memory_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_memory_create_info *create_info,
   struct nouveau_horizon_memory **memory_out);

/* Import using process-wide canonical NvMap ownership/cache/layout
 * metadata and a per-device VA wrapper. New wrappers are not CPU-mappable;
 * same-device reuse may return a mapped producer wrapper. Unknown IDs
 * require complete metadata and are rejected by require_existing.
 */
enum nouveau_horizon_status
nouveau_horizon_memory_import(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_memory_import_info *import_info,
   struct nouveau_horizon_memory **memory_out);

struct nouveau_horizon_memory *
nouveau_horizon_memory_ref(struct nouveau_horizon_memory *memory);

void
nouveau_horizon_memory_put(struct nouveau_horizon_memory *memory);

enum nouveau_horizon_status
nouveau_horizon_memory_map(struct nouveau_horizon_memory *memory,
                           void **map_out);

/* Map the whole allocation at a kernel-chosen GPU VA with 4 KiB pages, in
 * the small-page VA region disjoint from the fixed big-page heap.
 */
enum nouveau_horizon_status
nouveau_horizon_memory_map_gpu_small(struct nouveau_horizon_memory *memory,
                                     uint64_t *gpu_addr_out);

enum nouveau_horizon_status
nouveau_horizon_memory_unmap_gpu_small(struct nouveau_horizon_memory *memory,
                                       uint64_t gpu_addr);

void
nouveau_horizon_memory_sync_to_gpu(
   struct nouveau_horizon_memory *memory,
   uint64_t offset_B, uint64_t range_B);

void
nouveau_horizon_memory_sync_from_gpu(
   struct nouveau_horizon_memory *memory,
   uint64_t offset_B, uint64_t range_B);

/* Cached-memory synchronization:
 *
 * Ranges expand to whole non-coherent atoms (128 bytes on GM20B). Callers
 * own those atoms: sync_to_gpu follows CPU writes and precedes GPU access;
 * no CPU thread may access them while GPU work is pending. After GPU
 * writes complete, call sync_from_gpu.
 *
 * This prevents stale CPU writeback when invalidation falls back to
 * flushing. Coherent clients should use CPU-uncached memory.
 */

uint64_t
nouveau_horizon_memory_get_size(struct nouveau_horizon_memory *memory);

uint32_t
nouveau_horizon_memory_get_flags(struct nouveau_horizon_memory *memory);

uint8_t
nouveau_horizon_memory_get_backing_kind(
   struct nouveau_horizon_memory *memory);

void
nouveau_horizon_memory_get_layout(
   struct nouveau_horizon_memory *memory,
   struct nouveau_horizon_memory_layout *layout_out);

uint32_t
nouveau_horizon_memory_get_nvmap_handle(
   struct nouveau_horizon_memory *memory);

uint32_t
nouveau_horizon_memory_get_nvmap_id(struct nouveau_horizon_memory *memory);

/* Publish this memory's complete process-wide identity before returning its
 * NvMap ID for export.  Internal callers which only compare an already-owned
 * allocation should use nouveau_horizon_memory_get_nvmap_id().
 */
uint32_t
nouveau_horizon_memory_export_nvmap_id(
   struct nouveau_horizon_memory *memory);

enum nouveau_horizon_status
nouveau_horizon_va_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_va_create_info *create_info,
   struct nouveau_horizon_va **va_out);

struct nouveau_horizon_va *
nouveau_horizon_va_ref(struct nouveau_horizon_va *va);

void
nouveau_horizon_va_put(struct nouveau_horizon_va *va);

uint64_t
nouveau_horizon_va_get_addr(struct nouveau_horizon_va *va);

uint64_t
nouveau_horizon_va_get_size(struct nouveau_horizon_va *va);

enum nouveau_horizon_status
nouveau_horizon_va_bind(struct nouveau_horizon_va *va,
                        uint64_t va_offset_B,
                        struct nouveau_horizon_memory *memory,
                        uint64_t memory_offset_B,
                        uint64_t range_B);

enum nouveau_horizon_status
nouveau_horizon_va_unbind(struct nouveau_horizon_va *va,
                          uint64_t va_offset_B,
                          uint64_t range_B);

enum nouveau_horizon_status
nouveau_horizon_channel_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_channel_create_info *create_info,
   struct nouveau_horizon_channel **channel_out);

/* Release adapter-owned command/residency storage only after PUT_COMPLETE.
 * PUT_RETAINED leaves another reference and does not prove completion.
 * PUT_QUARANTINED marks device loss and retains the native channel and
 * backend storage because final completion is unknown.
 */
enum nouveau_horizon_channel_put_result {
   NOUVEAU_HORIZON_CHANNEL_PUT_COMPLETE,
   NOUVEAU_HORIZON_CHANNEL_PUT_RETAINED,
   NOUVEAU_HORIZON_CHANNEL_PUT_QUARANTINED,
};

struct nouveau_horizon_channel *
nouveau_horizon_channel_ref(struct nouveau_horizon_channel *channel);

enum nouveau_horizon_channel_put_result
nouveau_horizon_channel_put(struct nouveau_horizon_channel *channel);

enum nouveau_horizon_status
nouveau_horizon_channel_bind_zcull(
   struct nouveau_horizon_channel *channel, uint64_t addr);

enum nouveau_horizon_status
nouveau_horizon_channel_enqueue_waits(
   struct nouveau_horizon_channel *channel,
   uint32_t wait_count,
   const struct nouveau_horizon_fence *waits);

/* Order queued work before subsequent work with a GM20B host-WFI barrier:
 * isolated SET_REFERENCE entry, a following 3D no-op, then
 * L2/shader/descriptor acquire ending at a NO_PREFETCH boundary.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_full_barrier(
   struct nouveau_horizon_channel *channel);

enum nouveau_horizon_status
nouveau_horizon_channel_exec(
   struct nouveau_horizon_channel *channel,
   uint32_t exec_count,
   const struct nouveau_horizon_exec *execs);

/* Append execs, the optional full barrier, and completion under one
 * channel lock. Other producers cannot insert or submit work within this
 * transaction.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_exec_submit(
   struct nouveau_horizon_channel *channel,
   uint32_t exec_count,
   const struct nouveau_horizon_exec *execs,
   bool full_barrier,
   enum nouveau_horizon_completion_mode completion_mode,
   struct nouveau_horizon_fence *fence_out);

enum nouveau_horizon_status
nouveau_horizon_channel_submit(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_completion_mode completion_mode,
   struct nouveau_horizon_fence *fence_out);

/* Change mapped completion only on a pristine channel, before any command or
 * wait has been enqueued.  This lets ABI-constrained adapters declare their
 * engine binding immediately after constructing a private channel.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_set_mapped_completion(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_mapped_completion_mode mode);

enum nouveau_horizon_status
nouveau_horizon_channel_wait_idle(
   struct nouveau_horizon_channel *channel, uint64_t timeout_ns);

/* Use mapped completion for this channel's tracked fences; otherwise use
 * native syncpoints. A zero timeout polls without blocking.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_fence_wait(
   struct nouveau_horizon_channel *channel,
   const struct nouveau_horizon_fence *fence,
   uint64_t timeout_ns);

enum nouveau_horizon_status
nouveau_horizon_channel_get_error(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_error *error_out);

void
nouveau_horizon_channel_get_stats(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_channel_stats *stats_out);

enum nouveau_horizon_status
nouveau_horizon_fence_wait(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_fence *fence,
   uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif /* NOUVEAU_HORIZON_H */
