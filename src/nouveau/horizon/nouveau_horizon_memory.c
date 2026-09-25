/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nouveau_horizon_private.h"

#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/os_time.h"

#include <assert.h>
#include <malloc.h>
#include <string.h>

/* Defined with the backing-store cache below; the identity release path
 * needs it first. */
static bool
nouveau_horizon_bo_cache_put(struct nouveau_horizon_device *device,
                             struct nouveau_horizon_memory_identity *identity);

#define NOUVEAU_HORIZON_MEMORY_IDENTITY_FLAGS                              \
   NOUVEAU_HORIZON_MEMORY_GPU_CACHED

static void
nouveau_horizon_memory_record_create_call(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.memory_create_calls++;
   const uint64_t calls = device->debug_stats.memory_create_calls;
   simple_mtx_unlock(&device->debug_stats_mutex);

   /* In-session rate beacon.  Attributing process-heap fragmentation needs
    * the create rate while the workload runs; a session that crashes or is
    * force-quit never reaches the destroy-time summary.
    */
   if (calls % 1024 == 0) {
      simple_mtx_lock(&device->bo_cache_mutex);
      const uint64_t hits = device->bo_cache_hits;
      const uint64_t misses = device->bo_cache_misses;
      const uint64_t held_B = device->bo_cache_held_B;
      simple_mtx_unlock(&device->bo_cache_mutex);
      nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_INFO,
                          "memory: %llu creates, bo-cache %llu hits / "
                          "%llu misses, %llu MiB held",
                          (unsigned long long)calls,
                          (unsigned long long)hits,
                          (unsigned long long)misses,
                          (unsigned long long)(held_B >> 20));
   }
}

static void
nouveau_horizon_memory_record_create_failure(
   struct nouveau_horizon_device *device)
{
   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.memory_create_failures++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static void
nouveau_horizon_memory_record_import_call(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.memory_import_calls++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static void
nouveau_horizon_memory_record_import_failure(
   struct nouveau_horizon_device *device)
{
   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.memory_import_failures++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static bool
nouveau_horizon_memory_flags_valid(uint32_t flags)
{
   const uint32_t valid =
      NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE |
      NOUVEAU_HORIZON_MEMORY_CPU_CACHED |
      NOUVEAU_HORIZON_MEMORY_GPU_CACHED |
      NOUVEAU_HORIZON_MEMORY_ZERO;
   return (flags & ~valid) == 0 &&
          (!(flags & NOUVEAU_HORIZON_MEMORY_CPU_CACHED) ||
           (flags & NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE));
}

static bool
nouveau_horizon_memory_layout_valid(
   const struct nouveau_horizon_memory_layout *layout)
{
   if (!layout->valid)
      return layout->pte_kind == 0 && layout->tile_mode == 0;

   return layout->pte_kind != NvKind_Pitch || layout->tile_mode == 0;
}

static bool
nouveau_horizon_memory_try_ref(struct nouveau_horizon_memory *memory)
{
   uint32_t refcnt = p_atomic_read(&memory->refcnt);
   while (refcnt > 0) {
      const uint32_t old =
         p_atomic_cmpxchg(&memory->refcnt, refcnt, refcnt + 1);
      if (old == refcnt)
         return true;
      refcnt = old;
   }
   return false;
}

/* Called with device->memory_mutex held. */
static struct nouveau_horizon_memory *
nouveau_horizon_memory_lookup_locked(
   struct nouveau_horizon_device *device, uint32_t nvmap_id)
{
   list_for_each_entry(struct nouveau_horizon_memory, memory,
                       &device->memories, device_link) {
      if (memory->identity->nvmap_id == nvmap_id &&
          nouveau_horizon_memory_try_ref(memory))
         return memory;
   }
   return NULL;
}

/* Called with device->memory_mutex held. */
static void
nouveau_horizon_memory_register_locked(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_memory *memory)
{
   list_addtail(&memory->device_link, &device->memories);
   memory->registered = true;

   if (device->enable_timing) {
      simple_mtx_lock(&device->debug_stats_mutex);
      device->debug_stats.memory_wrappers_live++;
      device->debug_stats.memory_wrappers_peak =
         MAX2(device->debug_stats.memory_wrappers_peak,
              device->debug_stats.memory_wrappers_live);
      simple_mtx_unlock(&device->debug_stats_mutex);
   }
}

/* Called with runtime->memory_identity_mutex held. */
static struct nouveau_horizon_memory_identity *
nouveau_horizon_memory_identity_lookup_locked(
   struct nouveau_horizon_runtime *runtime, uint32_t nvmap_id)
{
   struct nouveau_horizon_memory_identity *identity =
      _mesa_hash_table_u64_search(runtime->memory_identities, nvmap_id);
   if (identity != NULL)
      identity->refcnt++;
   return identity;
}

/* Called with runtime->memory_identity_mutex held. */
static bool
nouveau_horizon_memory_identity_register_locked(
   struct nouveau_horizon_memory_identity *identity)
{
   struct nouveau_horizon_runtime *runtime = identity->runtime;
   assert(!identity->registered);
   assert(_mesa_hash_table_u64_search(runtime->memory_identities,
                                      identity->nvmap_id) == NULL);

   _mesa_hash_table_u64_insert(runtime->memory_identities,
                               identity->nvmap_id, identity);
   if (_mesa_hash_table_u64_search(runtime->memory_identities,
                                   identity->nvmap_id) != identity)
      return false;

   identity->registered = true;
   return true;
}

static struct nouveau_horizon_memory_identity *
nouveau_horizon_memory_identity_lookup(
   struct nouveau_horizon_runtime *runtime, uint32_t nvmap_id)
{
   simple_mtx_lock(&runtime->memory_identity_mutex);
   struct nouveau_horizon_memory_identity *identity =
      nouveau_horizon_memory_identity_lookup_locked(runtime, nvmap_id);
   simple_mtx_unlock(&runtime->memory_identity_mutex);
   return identity;
}

static bool
nouveau_horizon_memory_identity_matches_import(
   const struct nouveau_horizon_memory_identity *identity,
   const struct nouveau_horizon_memory_import_info *import_info)
{
   if (!import_info->has_metadata)
      return true;

   return (import_info->expected_size_B == 0 ||
           import_info->expected_size_B == identity->size_B) &&
          import_info->backing_kind == identity->backing_kind &&
          (import_info->flags & NOUVEAU_HORIZON_MEMORY_IDENTITY_FLAGS) ==
             identity->flags &&
          import_info->layout.valid == identity->layout.valid &&
          import_info->layout.pte_kind == identity->layout.pte_kind &&
          import_info->layout.tile_mode == identity->layout.tile_mode;
}

static void
nouveau_horizon_memory_identity_put(
   struct nouveau_horizon_memory_identity *identity)
{
   if (identity == NULL)
      return;

   struct nouveau_horizon_runtime *runtime = identity->runtime;
   simple_mtx_lock(&runtime->memory_identity_mutex);
   assert(identity->refcnt > 0);
   identity->refcnt--;
   if (identity->refcnt != 0) {
      simple_mtx_unlock(&runtime->memory_identity_mutex);
      return;
   }

   assert(identity->mapping_count == 0);
   /* A published NvMap ID may still be referenced by an importer, so its
    * handle must genuinely close rather than resurface under a new logical
    * allocation.
    */
   const bool recyclable = !identity->imported && !identity->registered &&
                           identity->accounting_device != NULL;
   if (identity->registered) {
      assert(_mesa_hash_table_u64_search(runtime->memory_identities,
                                         identity->nvmap_id) == identity);
      _mesa_hash_table_u64_remove(runtime->memory_identities,
                                  identity->nvmap_id);
      identity->registered = false;
   }
   simple_mtx_unlock(&runtime->memory_identity_mutex);

   if (!recyclable ||
       !nouveau_horizon_bo_cache_put(identity->accounting_device, identity)) {
      nvMapClose(&identity->map);
      if (!identity->imported)
         free(identity->cpu_addr);
   }

   if (identity->accounting_device != NULL) {
      struct nouveau_horizon_device *device = identity->accounting_device;
      nouveau_horizon_device_account_free(device, identity->size_B);
      nouveau_horizon_device_put(device);
   }

   FREE(identity);
}


/* Recycle whole backing stores to reduce 64 KiB alignment fragmentation,
 * NvMap handle churn, and kernel cache-attribute transitions.
 */
struct nouveau_horizon_bo_cache_entry {
   struct list_head bucket_link;
   struct list_head lru_link;
   NvMap map;
   void *cpu_addr;
   uint64_t size_B;
   uint32_t align_B;
   uint8_t backing_kind;
   bool cpu_cacheable;
};

static unsigned
nouveau_horizon_bo_cache_bucket(uint64_t size_B)
{
   assert(size_B >= NOUVEAU_HORIZON_BIND_ALIGN_B &&
          size_B <= NOUVEAU_HORIZON_BO_CACHE_MAX_ENTRY_B);
   return util_logbase2_ceil64(size_B / NOUVEAU_HORIZON_BIND_ALIGN_B);
}

static void
nouveau_horizon_bo_cache_entry_free(
   struct nouveau_horizon_bo_cache_entry *entry)
{
   nvMapClose(&entry->map);
   free(entry->cpu_addr);
   FREE(entry);
}

static void
nouveau_horizon_bo_cache_free_list(struct list_head *evicted)
{
   list_for_each_entry_safe(struct nouveau_horizon_bo_cache_entry, entry,
                            evicted, lru_link)
      nouveau_horizon_bo_cache_entry_free(entry);
}

/* Called with device->bo_cache_mutex held.  Evicted entries are collected on
 * a caller list so no NvMap ioctl ever runs under the lock.
 */
static void
nouveau_horizon_bo_cache_evict_locked(struct nouveau_horizon_device *device,
                                      struct list_head *evicted)
{
   while (device->bo_cache_entry_count > 0 &&
          (device->bo_cache_held_B > device->bo_cache_cap_B ||
           device->bo_cache_entry_count >
              NOUVEAU_HORIZON_BO_CACHE_MAX_ENTRIES)) {
      struct nouveau_horizon_bo_cache_entry *entry =
         list_last_entry(&device->bo_cache_lru,
                         struct nouveau_horizon_bo_cache_entry, lru_link);
      list_del(&entry->bucket_link);
      list_del(&entry->lru_link);
      device->bo_cache_held_B -= entry->size_B;
      device->bo_cache_entry_count--;
      device->bo_cache_evictions++;
      list_addtail(&entry->lru_link, evicted);
   }
}

static struct nouveau_horizon_bo_cache_entry *
nouveau_horizon_bo_cache_take(struct nouveau_horizon_device *device,
                              uint64_t size_B, uint64_t align_B,
                              uint8_t backing_kind, bool cpu_cacheable)
{
   if (device->bo_cache_cap_B == 0 ||
       size_B > NOUVEAU_HORIZON_BO_CACHE_MAX_ENTRY_B)
      return NULL;

   const unsigned bucket = nouveau_horizon_bo_cache_bucket(size_B);
   struct nouveau_horizon_bo_cache_entry *match = NULL;

   simple_mtx_lock(&device->bo_cache_mutex);
   list_for_each_entry(struct nouveau_horizon_bo_cache_entry, entry,
                       &device->bo_cache_buckets[bucket], bucket_link) {
      /* The NvMap object bakes in exactly these four parameters.  A larger
       * alignment still satisfies a smaller request; the size must match
       * exactly because the extent of the handle is fixed.
       */
      if (entry->size_B == size_B && entry->backing_kind == backing_kind &&
          entry->cpu_cacheable == cpu_cacheable &&
          entry->align_B >= align_B) {
         list_del(&entry->bucket_link);
         list_del(&entry->lru_link);
         device->bo_cache_held_B -= entry->size_B;
         device->bo_cache_entry_count--;
         match = entry;
         break;
      }
   }
   if (match != NULL)
      device->bo_cache_hits++;
   else
      device->bo_cache_misses++;
   simple_mtx_unlock(&device->bo_cache_mutex);
   return match;
}

static bool
nouveau_horizon_bo_cache_put(struct nouveau_horizon_device *device,
                             struct nouveau_horizon_memory_identity *identity)
{
   if (device->bo_cache_cap_B == 0 ||
       identity->size_B > NOUVEAU_HORIZON_BO_CACHE_MAX_ENTRY_B)
      return false;

   struct nouveau_horizon_bo_cache_entry *entry =
      CALLOC_STRUCT(nouveau_horizon_bo_cache_entry);
   if (entry == NULL)
      return false;

   entry->map = identity->map;
   entry->cpu_addr = identity->cpu_addr;
   entry->size_B = identity->size_B;
   entry->align_B = identity->align_B;
   entry->backing_kind = identity->backing_kind;
   entry->cpu_cacheable = identity->cpu_cacheable;

   const unsigned bucket = nouveau_horizon_bo_cache_bucket(entry->size_B);
   struct list_head evicted;
   list_inithead(&evicted);

   simple_mtx_lock(&device->bo_cache_mutex);
   list_add(&entry->bucket_link, &device->bo_cache_buckets[bucket]);
   list_add(&entry->lru_link, &device->bo_cache_lru);
   device->bo_cache_held_B += entry->size_B;
   device->bo_cache_entry_count++;
   nouveau_horizon_bo_cache_evict_locked(device, &evicted);
   simple_mtx_unlock(&device->bo_cache_mutex);

   nouveau_horizon_bo_cache_free_list(&evicted);
   return true;
}

void
nouveau_horizon_device_bo_cache_init(struct nouveau_horizon_device *device)
{
   simple_mtx_init(&device->bo_cache_mutex, mtx_plain);
   for (unsigned i = 0; i < NOUVEAU_HORIZON_BO_CACHE_BUCKETS; i++)
      list_inithead(&device->bo_cache_buckets[i]);
   list_inithead(&device->bo_cache_lru);

   const int64_t cap_MB =
      debug_get_num_option("NOUVEAU_HORIZON_BO_CACHE_MB",
                           NOUVEAU_HORIZON_BO_CACHE_DEFAULT_MB);
   device->bo_cache_cap_B = cap_MB > 0 ? (uint64_t)cap_MB << 20 : 0;
}

void
nouveau_horizon_device_bo_cache_trim(struct nouveau_horizon_device *device)
{
   struct list_head evicted;
   list_inithead(&evicted);

   simple_mtx_lock(&device->bo_cache_mutex);
   list_for_each_entry_safe(struct nouveau_horizon_bo_cache_entry, entry,
                            &device->bo_cache_lru, lru_link) {
      list_del(&entry->bucket_link);
      list_del(&entry->lru_link);
      device->bo_cache_evictions++;
      list_addtail(&entry->lru_link, &evicted);
   }
   device->bo_cache_held_B = 0;
   device->bo_cache_entry_count = 0;
   simple_mtx_unlock(&device->bo_cache_mutex);

   nouveau_horizon_bo_cache_free_list(&evicted);
}

void
nouveau_horizon_device_bo_cache_finish(struct nouveau_horizon_device *device)
{
   nouveau_horizon_device_bo_cache_trim(device);
   simple_mtx_destroy(&device->bo_cache_mutex);
}

static struct nouveau_horizon_memory *
nouveau_horizon_memory_wrapper_create(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_memory_identity *identity,
   uint32_t flags, bool imported)
{
   struct nouveau_horizon_memory *memory =
      CALLOC_STRUCT(nouveau_horizon_memory);
   if (memory == NULL)
      return NULL;

   memory->refcnt = 1;
   memory->device = nouveau_horizon_device_ref(device);
   memory->identity = identity;
   memory->flags = flags;
   memory->imported = imported;
   list_inithead(&memory->device_link);
   return memory;
}

static uint32_t
nouveau_horizon_memory_identity_wrapper_flags(
   const struct nouveau_horizon_memory_identity *identity)
{
   uint32_t flags = identity->flags;
   if (identity->cpu_addr != NULL)
      flags |= NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE;
   if (identity->cpu_cacheable)
      flags |= NOUVEAU_HORIZON_MEMORY_CPU_CACHED;
   return flags;
}

/* libnx's nvMapCreate flushes the CPU cache before making an uncached map,
 * but ignores the result of svcSetMemoryAttribute. Check it ourselves so a
 * coherent allocation can never succeed with cacheable CPU pages. Closing
 * the NvMap restores caching before the caller can reuse the backing.
 */
static Result
nouveau_horizon_memory_nvmap_create(NvMap *map, void *addr, uint32_t size,
                                     uint32_t align, NvKind kind, bool cached)
{
   Result rc = nvMapCreate(map, addr, size, align, kind, cached);
   if (R_SUCCEEDED(rc) && !cached) {
      rc = svcSetMemoryAttribute(addr, size, 8, 8);
      if (R_FAILED(rc))
         nvMapClose(map);
   }
   return rc;
}

enum nouveau_horizon_status
nouveau_horizon_memory_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_memory_create_info *create_info,
   struct nouveau_horizon_memory **memory_out)
{
   if (device == NULL || create_info == NULL || memory_out == NULL ||
       create_info->size_B == 0 ||
       !nouveau_horizon_memory_flags_valid(create_info->flags) ||
       !nouveau_horizon_memory_layout_valid(&create_info->layout))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   nouveau_horizon_memory_record_create_call(device);

   *memory_out = NULL;
   const bool is_host_import = create_info->import_host_ptr != NULL;
   uint64_t align_B, size_B;
   if (is_host_import) {
      /* Wrap the range exactly: rounding up to the bind alignment would pin
       * pages beyond it.
       */
      if (((uintptr_t)create_info->import_host_ptr & 0xFFFu) != 0 ||
          (create_info->size_B & 0xFFFu) != 0) {
         nouveau_horizon_memory_record_create_failure(device);
         return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
      }
      align_B = 0x1000;
      size_B = create_info->size_B;
      if (size_B > UINT32_MAX) {
         nouveau_horizon_memory_record_create_failure(device);
         return NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   } else {
      const uint32_t bind_align_B = nouveau_horizon_device_bind_align(device);
      align_B = MAX2(create_info->align_B, (uint64_t)bind_align_B);
      if (!util_is_power_of_two_nonzero64(align_B)) {
         nouveau_horizon_memory_record_create_failure(device);
         return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
      }

      size_B = nouveau_horizon_align_u64(create_info->size_B, bind_align_B);
      if (size_B == 0 || size_B > UINT32_MAX || align_B > UINT32_MAX) {
         nouveau_horizon_memory_record_create_failure(device);
         return NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }

   struct nouveau_horizon_memory_identity *identity =
      CALLOC_STRUCT(nouveau_horizon_memory_identity);
   if (identity == NULL) {
      nouveau_horizon_memory_record_create_failure(device);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
   }

   const bool cpu_cacheable =
      (create_info->flags & NOUVEAU_HORIZON_MEMORY_CPU_CACHED) != 0;

   struct nouveau_horizon_bo_cache_entry *recycled = is_host_import ?
      NULL :
      nouveau_horizon_bo_cache_take(device, size_B, align_B,
                                    create_info->backing_kind, cpu_cacheable);
   if (is_host_import) {
      identity->cpu_addr = create_info->import_host_ptr;
      /* Trim the cache once and retry, as the allocation path below does. */
      Result rc = 0;
      for (unsigned attempt = 0;; attempt++) {
         rc = nouveau_horizon_memory_nvmap_create(&identity->map, identity->cpu_addr,
                          (uint32_t)size_B, (uint32_t)align_B,
                          (NvKind)create_info->backing_kind,
                          cpu_cacheable);
         if (R_SUCCEEDED(rc))
            break;
         if (attempt == 0) {
            nouveau_horizon_device_bo_cache_trim(device);
            continue;
         }
         nouveau_horizon_memory_record_create_failure(device);
         nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                             "nvMapCreate(import=%p, size=0x%llx) failed: 0x%x",
                             identity->cpu_addr,
                             (unsigned long long)size_B, R_VALUE(rc));
         FREE(identity);
         return nouveau_horizon_status_from_result(
            rc, NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY);
      }
   } else if (recycled != NULL) {
      identity->cpu_addr = recycled->cpu_addr;
      identity->map = recycled->map;
      FREE(recycled);
   } else {
      /* Trim and retry once for backing or NvMap allocation failure so
       * cached storage cannot prevent recovery.
       */
      for (unsigned attempt = 0;; attempt++) {
         identity->cpu_addr = memalign((size_t)align_B, (size_t)size_B);
         if (identity->cpu_addr == NULL) {
            if (attempt == 0) {
               nouveau_horizon_device_bo_cache_trim(device);
               continue;
            }
            nouveau_horizon_memory_record_create_failure(device);
            FREE(identity);
            return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
         }

         Result rc = nouveau_horizon_memory_nvmap_create(&identity->map, identity->cpu_addr,
                                 (uint32_t)size_B, (uint32_t)align_B,
                                 (NvKind)create_info->backing_kind,
                                 cpu_cacheable);
         if (R_SUCCEEDED(rc))
            break;

         free(identity->cpu_addr);
         identity->cpu_addr = NULL;
         if (attempt == 0) {
            nouveau_horizon_device_bo_cache_trim(device);
            continue;
         }

         nouveau_horizon_memory_record_create_failure(device);
         struct nouveau_horizon_device_debug_stats stats = {0};
         nouveau_horizon_device_get_debug_stats(device, &stats);
         nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                              "nvMapCreate(size=0x%llx, align=0x%llx) failed: "
                              "0x%x (mem=%llu/%llu wrappers=%llu/%llu "
                              "VA=%llu/%llu mappings=%llu/%llu failures=%llu)",
                              (unsigned long long)size_B,
                              (unsigned long long)align_B, R_VALUE(rc),
                              (unsigned long long)stats.native_memories_live,
                              (unsigned long long)stats.native_memories_peak,
                              (unsigned long long)stats.memory_wrappers_live,
                              (unsigned long long)stats.memory_wrappers_peak,
                              (unsigned long long)stats.vas_live,
                              (unsigned long long)stats.vas_peak,
                              (unsigned long long)stats.mappings_live,
                              (unsigned long long)stats.mappings_peak,
                              (unsigned long long)
                                 stats.memory_create_failures);
         FREE(identity);
         return nouveau_horizon_status_from_result(
            rc, NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY);
      }
   }

   /* Zero new and recycled backing, even without ZERO, before possible
    * export. Clean cached mappings so the GPU sees the zeros; uncached
    * stores need no clean.
    */
   if (is_host_import) {
      /* Imported memory keeps its contents; clean so the GPU's first read
       * observes them.
       */
      if (cpu_cacheable)
         armDCacheClean(identity->cpu_addr, (size_t)size_B);
   } else {
      memset(identity->cpu_addr, 0, (size_t)size_B);
      if (cpu_cacheable)
         armDCacheClean(identity->cpu_addr, (size_t)size_B);
   }

   identity->runtime = device->runtime;
   identity->refcnt = 1;
   identity->size_B = size_B;
   identity->align_B = (uint32_t)align_B;
   identity->flags =
      create_info->flags & NOUVEAU_HORIZON_MEMORY_IDENTITY_FLAGS;
   identity->backing_kind = create_info->backing_kind;
   identity->layout = create_info->layout;
   identity->cpu_cacheable = cpu_cacheable;
   /* Imported identities never free or recycle the caller's pages and stay
    * out of the heap accounting.
    */
   identity->imported = is_host_import;
   identity->nvmap_id = nvMapGetId(&identity->map);
   identity->accounting_device =
      is_host_import ? NULL : nouveau_horizon_device_ref(device);

   if (identity->nvmap_id == 0) {
      nouveau_horizon_memory_record_create_failure(device);
      nvMapClose(&identity->map);
      if (!identity->imported)
         free(identity->cpu_addr);
      if (identity->accounting_device != NULL)
         nouveau_horizon_device_put(identity->accounting_device);
      FREE(identity);
      return NOUVEAU_HORIZON_ERROR_SYSTEM;
   }

   if (!is_host_import)
      nouveau_horizon_device_account_alloc(device, size_B);
   struct nouveau_horizon_memory *memory =
      nouveau_horizon_memory_wrapper_create(device, identity,
                                             create_info->flags, false);
   if (memory == NULL) {
      nouveau_horizon_memory_record_create_failure(device);
      nouveau_horizon_memory_identity_put(identity);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
   }

   simple_mtx_lock(&device->memory_mutex);
   nouveau_horizon_memory_register_locked(device, memory);
   simple_mtx_unlock(&device->memory_mutex);

   *memory_out = memory;
   return NOUVEAU_HORIZON_SUCCESS;
}

static enum nouveau_horizon_status
nouveau_horizon_memory_identity_import(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_memory_import_info *import_info,
   struct nouveau_horizon_memory_identity **identity_out)
{
   struct nouveau_horizon_runtime *runtime = device->runtime;
   struct nouveau_horizon_memory_identity *identity =
      nouveau_horizon_memory_identity_lookup(runtime, import_info->nvmap_id);
   if (identity != NULL) {
      if (!nouveau_horizon_memory_identity_matches_import(identity,
                                                           import_info)) {
         nouveau_horizon_memory_identity_put(identity);
         return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
      }
      *identity_out = identity;
      return NOUVEAU_HORIZON_SUCCESS;
   }

   if (import_info->require_existing)
      return NOUVEAU_HORIZON_ERROR_NOT_SUPPORTED;

   struct nouveau_horizon_memory_identity *candidate =
      CALLOC_STRUCT(nouveau_horizon_memory_identity);
   if (candidate == NULL)
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;

   Result rc = nvMapLoadRemote(&candidate->map, import_info->nvmap_id);
   if (R_FAILED(rc)) {
      FREE(candidate);
      return nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   candidate->runtime = runtime;
   candidate->refcnt = 1;
   candidate->nvmap_id = import_info->nvmap_id;
   candidate->size_B = nvMapGetSize(&candidate->map);
   candidate->align_B = nouveau_horizon_device_bind_align(device);
   candidate->flags =
      import_info->flags & NOUVEAU_HORIZON_MEMORY_IDENTITY_FLAGS;
   candidate->backing_kind = (uint8_t)nvMapGetKind(&candidate->map);
   candidate->layout = import_info->layout;
   candidate->imported = true;

   if (!nouveau_horizon_memory_identity_matches_import(candidate,
                                                        import_info) ||
       candidate->size_B == 0) {
      nvMapClose(&candidate->map);
      FREE(candidate);
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
   }

   /* A creator or another importing device may publish the same identity
    * while LoadRemote is in flight.  Keep exactly one native NvMap owner and
    * compare the complete descriptor before sharing it.
    */
   simple_mtx_lock(&runtime->memory_identity_mutex);
   identity = nouveau_horizon_memory_identity_lookup_locked(
      runtime, import_info->nvmap_id);
   bool registered = true;
   if (identity == NULL)
      registered =
         nouveau_horizon_memory_identity_register_locked(candidate);
   simple_mtx_unlock(&runtime->memory_identity_mutex);

   if (!registered) {
      nvMapClose(&candidate->map);
      FREE(candidate);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
   }

   if (identity != NULL) {
      nvMapClose(&candidate->map);
      FREE(candidate);
      if (!nouveau_horizon_memory_identity_matches_import(identity,
                                                           import_info)) {
         nouveau_horizon_memory_identity_put(identity);
         return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
      }
      *identity_out = identity;
   } else {
      *identity_out = candidate;
   }

   return NOUVEAU_HORIZON_SUCCESS;
}

enum nouveau_horizon_status
nouveau_horizon_memory_import(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_memory_import_info *import_info,
   struct nouveau_horizon_memory **memory_out)
{
   if (device == NULL || import_info == NULL || memory_out == NULL ||
       import_info->nvmap_id == 0 ||
       (!import_info->has_metadata && !import_info->require_existing) ||
       (import_info->has_metadata &&
        ((import_info->flags & ~NOUVEAU_HORIZON_MEMORY_IDENTITY_FLAGS) != 0 ||
         !import_info->layout.valid ||
         !nouveau_horizon_memory_layout_valid(&import_info->layout))))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   nouveau_horizon_memory_record_import_call(device);

   *memory_out = NULL;

   simple_mtx_lock(&device->memory_mutex);
   struct nouveau_horizon_memory *existing =
      nouveau_horizon_memory_lookup_locked(device, import_info->nvmap_id);
   simple_mtx_unlock(&device->memory_mutex);
   if (existing != NULL) {
      if (!nouveau_horizon_memory_identity_matches_import(existing->identity,
                                                           import_info)) {
         nouveau_horizon_memory_put(existing);
         nouveau_horizon_memory_record_import_failure(device);
         return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
      }
      *memory_out = existing;
      return NOUVEAU_HORIZON_SUCCESS;
   }

   struct nouveau_horizon_memory_identity *identity = NULL;
   enum nouveau_horizon_status status =
      nouveau_horizon_memory_identity_import(device, import_info, &identity);
   if (status != NOUVEAU_HORIZON_SUCCESS)
   {
      nouveau_horizon_memory_record_import_failure(device);
      return status;
   }

   struct nouveau_horizon_memory *memory =
      nouveau_horizon_memory_wrapper_create(
         device, identity,
         import_info->require_existing ?
            nouveau_horizon_memory_identity_wrapper_flags(identity) : 0,
         true);
   if (memory == NULL) {
      nouveau_horizon_memory_record_import_failure(device);
      nouveau_horizon_memory_identity_put(identity);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
   }

   simple_mtx_lock(&device->memory_mutex);
   existing = nouveau_horizon_memory_lookup_locked(device,
                                                    import_info->nvmap_id);
   if (existing == NULL)
      nouveau_horizon_memory_register_locked(device, memory);
   simple_mtx_unlock(&device->memory_mutex);

   if (existing != NULL) {
      nouveau_horizon_memory_identity_put(memory->identity);
      nouveau_horizon_device_put(memory->device);
      FREE(memory);
      if (!nouveau_horizon_memory_identity_matches_import(existing->identity,
                                                           import_info)) {
         nouveau_horizon_memory_put(existing);
         nouveau_horizon_memory_record_import_failure(device);
         return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
      }
      *memory_out = existing;
   } else {
      *memory_out = memory;
   }

   return NOUVEAU_HORIZON_SUCCESS;
}

struct nouveau_horizon_memory *
nouveau_horizon_memory_ref(struct nouveau_horizon_memory *memory)
{
   if (memory != NULL)
      p_atomic_inc(&memory->refcnt);
   return memory;
}

void
nouveau_horizon_memory_put(struct nouveau_horizon_memory *memory)
{
   if (memory == NULL || !p_atomic_dec_zero(&memory->refcnt))
      return;

   struct nouveau_horizon_device *device = memory->device;
   simple_mtx_lock(&device->memory_mutex);
   if (memory->registered) {
      list_delinit(&memory->device_link);
      memory->registered = false;

      if (device->enable_timing) {
         simple_mtx_lock(&device->debug_stats_mutex);
         assert(device->debug_stats.memory_wrappers_live > 0);
         device->debug_stats.memory_wrappers_live--;
         simple_mtx_unlock(&device->debug_stats_mutex);
      }
   }
   simple_mtx_unlock(&device->memory_mutex);

   nouveau_horizon_memory_identity_put(memory->identity);
   nouveau_horizon_device_put(device);
   FREE(memory);
}

enum nouveau_horizon_status
nouveau_horizon_memory_map(struct nouveau_horizon_memory *memory,
                           void **map_out)
{
   if (memory == NULL || map_out == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   *map_out = NULL;
   if (memory->identity->cpu_addr == NULL ||
       !(memory->flags & NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE))
      return NOUVEAU_HORIZON_ERROR_NOT_SUPPORTED;

   *map_out = memory->identity->cpu_addr;
   return NOUVEAU_HORIZON_SUCCESS;
}

static bool
nouveau_horizon_memory_sync_range(
   struct nouveau_horizon_memory *memory, uint64_t offset_B,
   uint64_t range_B, void **addr_out, size_t *range_out)
{
   if (memory == NULL ||
       !(memory->flags & NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE) ||
       !(memory->flags & NOUVEAU_HORIZON_MEMORY_CPU_CACHED) ||
       memory->identity->cpu_addr == NULL || range_B == 0 ||
       offset_B >= memory->identity->size_B)
      return false;

   range_B = MIN2(range_B, memory->identity->size_B - offset_B);
   uint64_t atom_B = memory->device->info.nc_atom_size_B;
   if (!util_is_power_of_two_nonzero64(atom_B))
      atom_B = 128;

   /* NvMap allocations are at least bind-aligned, so expanding an offset to
    * a non-coherent atom also produces a real virtual-address atom boundary.
    */
   assert(((uintptr_t)memory->identity->cpu_addr & (atom_B - 1)) == 0);
   const uint64_t start_B = offset_B & ~(atom_B - 1);
   const uint64_t requested_end_B = offset_B + range_B;
   uint64_t end_B = nouveau_horizon_align_u64(requested_end_B, atom_B);
   if (end_B == 0 || end_B > memory->identity->size_B)
      end_B = memory->identity->size_B;

   *addr_out = (char *)memory->identity->cpu_addr + start_B;
   *range_out = (size_t)(end_B - start_B);
   return true;
}

void
nouveau_horizon_memory_sync_to_gpu(
   struct nouveau_horizon_memory *memory,
   uint64_t offset_B, uint64_t range_B)
{
   void *addr = NULL;
   size_t expanded_range_B = 0;
   if (!nouveau_horizon_memory_sync_range(memory, offset_B, range_B,
                                           &addr, &expanded_range_B))
      return;

   const uint64_t start_ns = memory->device->enable_timing ?
      os_time_get_nano() : 0;
   armDCacheClean(addr, expanded_range_B);
   if (memory->device->enable_timing) {
      nouveau_horizon_device_record_cache_sync(
         memory->device, true, expanded_range_B,
         os_time_get_nano() - start_ns);
   }
}

static bool
nouveau_horizon_try_invalidate_cpu_cache(void *addr, size_t range_B)
{
   if (!envIsSyscallHinted(
          NOUVEAU_HORIZON_SVC_INVALIDATE_PROCESS_DATA_CACHE))
      return false;

   const Handle process = envGetOwnProcessHandle();
   if (process == INVALID_HANDLE)
      return false;

   return R_SUCCEEDED(svcInvalidateProcessDataCache(
      process, (uintptr_t)addr, range_B));
}

void
nouveau_horizon_memory_sync_from_gpu(
   struct nouveau_horizon_memory *memory,
   uint64_t offset_B, uint64_t range_B)
{
   void *addr = NULL;
   size_t expanded_range_B = 0;
   if (!nouveau_horizon_memory_sync_range(memory, offset_B, range_B,
                                           &addr, &expanded_range_B))
      return;

   const uint64_t start_ns = memory->device->enable_timing ?
      os_time_get_nano() : 0;
   if (!nouveau_horizon_try_invalidate_cpu_cache(addr, expanded_range_B))
      armDCacheFlush(addr, expanded_range_B);
   if (memory->device->enable_timing) {
      nouveau_horizon_device_record_cache_sync(
         memory->device, false, expanded_range_B,
         os_time_get_nano() - start_ns);
   }
}

uint64_t
nouveau_horizon_memory_get_size(struct nouveau_horizon_memory *memory)
{
   return memory != NULL ? memory->identity->size_B : 0;
}

uint32_t
nouveau_horizon_memory_get_flags(struct nouveau_horizon_memory *memory)
{
   return memory != NULL ? memory->flags : 0;
}

uint8_t
nouveau_horizon_memory_get_backing_kind(
   struct nouveau_horizon_memory *memory)
{
   return memory != NULL ? memory->identity->backing_kind : 0;
}

void
nouveau_horizon_memory_get_layout(
   struct nouveau_horizon_memory *memory,
   struct nouveau_horizon_memory_layout *layout_out)
{
   if (layout_out == NULL)
      return;

   *layout_out = memory != NULL ? memory->identity->layout :
                                  (struct nouveau_horizon_memory_layout) {0};
}

uint32_t
nouveau_horizon_memory_get_nvmap_handle(
   struct nouveau_horizon_memory *memory)
{
   return memory != NULL ? nvMapGetHandle(&memory->identity->map) : 0;
}

uint32_t
nouveau_horizon_memory_get_nvmap_id(struct nouveau_horizon_memory *memory)
{
   return memory != NULL ? memory->identity->nvmap_id : 0;
}

uint32_t
nouveau_horizon_memory_export_nvmap_id(
   struct nouveau_horizon_memory *memory)
{
   if (memory == NULL)
      return 0;

   struct nouveau_horizon_memory_identity *identity = memory->identity;
   struct nouveau_horizon_runtime *runtime = identity->runtime;
   struct nouveau_horizon_memory_identity *duplicate = NULL;
   bool registered = true;

   simple_mtx_lock(&runtime->memory_identity_mutex);
   if (!identity->registered) {
      duplicate = nouveau_horizon_memory_identity_lookup_locked(
         runtime, identity->nvmap_id);
      if (duplicate == NULL)
         registered =
            nouveau_horizon_memory_identity_register_locked(identity);
   }
   simple_mtx_unlock(&runtime->memory_identity_mutex);

   if (duplicate != NULL) {
      nouveau_horizon_memory_identity_put(duplicate);
      nouveau_horizon_log(
         memory->device, NOUVEAU_HORIZON_LOG_ERROR,
         "NvMap ID %u collided with an existing process identity",
         identity->nvmap_id);
      return 0;
   }

   if (!registered) {
      nouveau_horizon_log(memory->device, NOUVEAU_HORIZON_LOG_ERROR,
                           "failed to publish NvMap ID %u",
                           identity->nvmap_id);
      return 0;
   }

   return identity->nvmap_id;
}

enum nouveau_horizon_status
nouveau_horizon_memory_acquire_mapping(
   struct nouveau_horizon_memory *memory, uint8_t pte_kind)
{
   if (memory == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   struct nouveau_horizon_memory_identity *identity = memory->identity;
   struct nouveau_horizon_runtime *runtime = identity->runtime;
   simple_mtx_lock(&runtime->memory_identity_mutex);
   if (identity->layout.valid && identity->layout.pte_kind != pte_kind) {
      simple_mtx_unlock(&runtime->memory_identity_mutex);
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
   }
   identity->mapping_count++;
   simple_mtx_unlock(&runtime->memory_identity_mutex);
   return NOUVEAU_HORIZON_SUCCESS;
}

void
nouveau_horizon_memory_release_mapping(
   struct nouveau_horizon_memory *memory, uint8_t pte_kind)
{
   if (memory == NULL)
      return;

   struct nouveau_horizon_memory_identity *identity = memory->identity;
   struct nouveau_horizon_runtime *runtime = identity->runtime;
   simple_mtx_lock(&runtime->memory_identity_mutex);
   assert(identity->mapping_count > 0);
   if (identity->layout.valid)
      assert(identity->layout.pte_kind == pte_kind);
   identity->mapping_count--;
   simple_mtx_unlock(&runtime->memory_identity_mutex);
}

enum nouveau_horizon_status
nouveau_horizon_memory_map_gpu_small(struct nouveau_horizon_memory *memory,
                                     uint64_t *gpu_addr_out)
{
   if (memory == NULL || gpu_addr_out == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   struct nouveau_horizon_device *device = memory->device;
   struct nouveau_horizon_memory_identity *identity = memory->identity;

   enum nouveau_horizon_status status =
      nouveau_horizon_memory_acquire_mapping(memory, identity->backing_kind);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return status;

   /* Kernel-chosen VA with 4 KiB pages, from the small-page region disjoint
    * from the fixed big-page heap.
    */
   u64 gpu_addr = 0;
   simple_mtx_lock(&device->va_mutex);
   Result rc = nvioctlNvhostAsGpu_MapBufferEx(
      device->addr_space.fd,
      nouveau_horizon_memory_is_gpu_cacheable(memory) ?
         NvMapBufferFlags_IsCacheable : 0,
      identity->backing_kind, nvMapGetHandle(&identity->map),
      0x1000, 0, 0, 0, &gpu_addr);
   simple_mtx_unlock(&device->va_mutex);
   if (R_FAILED(rc)) {
      nouveau_horizon_memory_release_mapping(memory, identity->backing_kind);
      nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                          "small-page GPU map of NvMap %u (size=0x%llx) "
                          "failed: 0x%x",
                          identity->nvmap_id,
                          (unsigned long long)identity->size_B, R_VALUE(rc));
      return nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   *gpu_addr_out = gpu_addr;
   return NOUVEAU_HORIZON_SUCCESS;
}

enum nouveau_horizon_status
nouveau_horizon_memory_unmap_gpu_small(struct nouveau_horizon_memory *memory,
                                       uint64_t gpu_addr)
{
   if (memory == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   struct nouveau_horizon_device *device = memory->device;
   simple_mtx_lock(&device->va_mutex);
   const Result rc = nvAddressSpaceUnmap(&device->addr_space, gpu_addr);
   simple_mtx_unlock(&device->va_mutex);
   if (R_FAILED(rc)) {
      nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                          "small-page GPU unmap at 0x%llx failed: 0x%x",
                          (unsigned long long)gpu_addr, R_VALUE(rc));
      return nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_SYSTEM);
   }
   nouveau_horizon_memory_release_mapping(memory,
                                          memory->identity->backing_kind);
   return NOUVEAU_HORIZON_SUCCESS;
}

NvMap *
nouveau_horizon_memory_get_native_map(
   struct nouveau_horizon_memory *memory)
{
   return memory != NULL ? &memory->identity->map : NULL;
}

bool
nouveau_horizon_memory_is_gpu_cacheable(
   struct nouveau_horizon_memory *memory)
{
   return memory != NULL &&
          (memory->identity->flags & NOUVEAU_HORIZON_MEMORY_GPU_CACHED);
}
