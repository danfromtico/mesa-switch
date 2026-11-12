/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_MEMORY_BUDGET_H
#define TG_GPU_MEMORY_BUDGET_H

/* VidMm video-memory values and the rules that turn them into one Vulkan
 * heap budget. No Windows dependency: the same rules run in the host model.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* D3DKMT_MEMORY_SEGMENT_GROUP values. The groups are never merged: which one
 * VidMm charges for an allocation follows from the KMD's segment descriptors
 * and the running OS. The GM20B KMD's paging-aperture segment, which holds
 * every ordinary allocation, declares neither LocalBudgetGroup nor
 * NonLocalBudgetGroup, so the charged group must be measured on the device.
 */
enum {
   TG_KMT_MEMORY_LOCAL = 0,
   TG_KMT_MEMORY_NON_LOCAL = 1,
   TG_KMT_MEMORY_GROUPS = 2,
};
#define TG_KMT_MEMORY_UNMEASURED (-1)

/* D3DKMT_QUERYVIDEOMEMORYINFO outputs for the calling process, unmodified. */
struct tg_kmt_video_memory {
   uint64_t budget, usage, reservation, available_for_reservation;
};

struct tg_gpu_heap_budget {
   uint64_t budget, usage;
   bool measured; /* false: both values are the caller's estimates */
};

/* `measured` is the group that was shown to carry this heap, or NULL. VidMm
 * values are used only as a pair and only with a non-zero Budget, because
 * Vulkan requires a non-zero heapBudget and a zero Budget means the group
 * holds no segment. The adapter ceiling stays the upper bound. A reservation
 * is a guaranteed minimum, not consumption: it is never added to usage.
 */
static inline struct tg_gpu_heap_budget
tg_gpu_heap_budget(uint64_t ceiling, const struct tg_kmt_video_memory *measured,
                   uint64_t estimated_budget, uint64_t estimated_usage)
{
   if (measured && measured->budget && ceiling)
      return (struct tg_gpu_heap_budget) {
         .budget = measured->budget < ceiling ? measured->budget : ceiling,
         .usage = measured->usage, .measured = true,
      };
   return (struct tg_gpu_heap_budget) {
      .budget = estimated_budget, .usage = estimated_usage,
   };
}

/* Operator selection of the measured group. Anything but the two exact names
 * leaves the heap on its estimate.
 */
static inline int
tg_gpu_parse_budget_group(const char *text)
{
   if (text && !strcmp(text, "local"))
      return TG_KMT_MEMORY_LOCAL;
   if (text && !strcmp(text, "non-local"))
      return TG_KMT_MEMORY_NON_LOCAL;
   return TG_KMT_MEMORY_UNMEASURED;
}

enum tg_gpu_memory_charge {
   TG_GPU_CHARGE_NONE,      /* neither group grew by the allocation */
   TG_GPU_CHARGE_LOCAL,
   TG_GPU_CHARGE_NON_LOCAL,
   TG_GPU_CHARGE_BOTH,      /* cannot attribute; rerun on an idle process */
};

/* Which group's CurrentUsage grew by at least `bytes` between two samples
 * taken around one resident allocation of that size.
 */
static inline enum tg_gpu_memory_charge
tg_gpu_classify_charge(const struct tg_kmt_video_memory before[TG_KMT_MEMORY_GROUPS],
                       const struct tg_kmt_video_memory after[TG_KMT_MEMORY_GROUPS],
                       uint64_t bytes)
{
   bool grew[TG_KMT_MEMORY_GROUPS];
   for (int group = 0; group < TG_KMT_MEMORY_GROUPS; group++)
      grew[group] = bytes && after[group].usage >= before[group].usage &&
                    after[group].usage - before[group].usage >= bytes;
   if (grew[TG_KMT_MEMORY_LOCAL] && grew[TG_KMT_MEMORY_NON_LOCAL])
      return TG_GPU_CHARGE_BOTH;
   if (grew[TG_KMT_MEMORY_LOCAL])
      return TG_GPU_CHARGE_LOCAL;
   return grew[TG_KMT_MEMORY_NON_LOCAL] ? TG_GPU_CHARGE_NON_LOCAL : TG_GPU_CHARGE_NONE;
}

#endif
