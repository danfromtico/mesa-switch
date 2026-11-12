/* SPDX-License-Identifier: MIT */
/* Live VidMm budget probe. It measures which memory segment group the OS
 * charges for an ordinary resident GM20B allocation and prints every group
 * separately. It does not submit GPU work and changes no reservation.
 */
#include "kmt_native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const group_names[TG_KMT_MEMORY_GROUPS] = {"local", "non-local"};

static LONG
record(const char *operation, LONG status)
{
   printf("%s status=0x%08lx\n", operation, (unsigned long)status);
   return status;
}

static LONG
sample(const struct tg_gpu_adapter_info *adapter, const char *phase,
       struct tg_kmt_video_memory groups[TG_KMT_MEMORY_GROUPS])
{
   const LONG status = tg_kmt_query_video_memory(adapter, groups);
   if (status < 0) {
      printf("%s: D3DKMTQueryVideoMemoryInfo status=0x%08lx\n", phase, (unsigned long)status);
      return status;
   }
   for (int group = 0; group < TG_KMT_MEMORY_GROUPS; group++)
      printf("%s: group=%s Budget=%llu CurrentUsage=%llu CurrentReservation=%llu "
             "AvailableForReservation=%llu\n", phase, group_names[group],
             (unsigned long long)groups[group].budget,
             (unsigned long long)groups[group].usage,
             (unsigned long long)groups[group].reservation,
             (unsigned long long)groups[group].available_for_reservation);
   return status;
}

int main(int argc, char **argv)
{
   char *end = NULL;
   const unsigned long mib = argc == 3 ? strtoul(argv[2], &end, 10) : 32;
   if (argc < 2 || argc > 3 || strlen(argv[1]) != 16 ||
       strspn(argv[1], "0123456789abcdefABCDEF") != 16 ||
       (argc == 3 && (*end || !mib || mib > 256))) {
      fprintf(stderr, "usage: nvk-windows-memory-check.exe <16-hex-digit-GM20B-LUID> "
                      "[allocation MiB, 1..256, default 32]\n");
      return 1;
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   const uint64_t wanted = strtoull(argv[1], NULL, 16);
   const uint64_t bytes = (uint64_t)mib << 20;
   struct tg_kmt_scan scan;
   LONG status = record("scan", tg_gpu_kmt_scan_system(&scan));
   if (status < 0 || scan.matched != 1 ||
       ((uint64_t)(uint32_t)scan.first_match.luid_high << 32 | scan.first_match.luid_low) != wanted) {
      fprintf(stderr, "The requested live GM20B LUID was not found.\n");
      return 2;
   }
   const struct tg_gpu_adapter_info *adapter = &scan.first_match;
   printf("GM20B luid=%08lx%08lx ceiling=%llu allocation=%llu\n",
          (unsigned long)adapter->luid_high, (unsigned long)adapter->luid_low,
          (unsigned long long)adapter->memory_budget_bytes, (unsigned long long)bytes);

   /* Evidence only: segment numbering and property bits are the OS's. */
   for (uint32_t segment = 0; segment < 8; segment++) {
      struct tg_kmt_segment_statistics s;
      status = tg_kmt_query_segment(adapter, segment, &s);
      if (status < 0) {
         printf("segment %lu: D3DKMTQueryStatistics status=0x%08lx\n",
                (unsigned long)segment, (unsigned long)status);
         break;
      }
      printf("segment %lu: CommitLimit=%llu BytesCommitted=%llu BytesResident=%llu "
             "Aperture=%lu SegmentProperties=0x%016llx\n", (unsigned long)segment,
             (unsigned long long)s.commit_limit, (unsigned long long)s.committed,
             (unsigned long long)s.resident, (unsigned long)s.aperture,
             (unsigned long long)s.properties);
   }

   struct tg_kmt_video_memory idle[TG_KMT_MEMORY_GROUPS], opened[TG_KMT_MEMORY_GROUPS],
      resident[TG_KMT_MEMORY_GROUPS], released[TG_KMT_MEMORY_GROUPS];
   struct tg_kmt_device device = {0};
   struct tg_kmt_allocation allocation = {0};
   enum tg_gpu_memory_charge charge = TG_GPU_CHARGE_NONE;
   bool sampled = false, passed = true;
   if (sample(adapter, "before device", idle) < 0)
      return 3;
   status = record("tg_kmt_open_device", tg_kmt_open_device(&device, adapter));
   if (status < 0)
      return 3;
   if (sample(adapter, "device open", opened) < 0)
      goto cleanup;
   /* The same CPU-visible, immediately resident path as NVK's only Windows
    * memory type. */
   status = record("tg_kmt_allocate", tg_kmt_allocate(&device, bytes, true, &allocation));
   if (status < 0)
      goto cleanup;
   if (sample(adapter, "allocation resident", resident) < 0)
      goto cleanup;
   charge = tg_gpu_classify_charge(opened, resident, bytes);
   sampled = true;

cleanup:
   if (allocation.handle || allocation.resource)
      passed &= record("tg_kmt_free", tg_kmt_free(&device, &allocation)) >= 0;
   if (sampled && sample(adapter, "allocation freed", released) < 0)
      sampled = false;
   passed &= record("tg_kmt_close_device", tg_kmt_close_device(&device)) >= 0;
   if (!sampled || !passed) {
      puts("FAIL: the VidMm samples are incomplete");
      return 3;
   }

   if (charge != TG_GPU_CHARGE_LOCAL && charge != TG_GPU_CHARGE_NON_LOCAL) {
      printf("FAIL: %s; no group can be selected from this run\n",
             charge == TG_GPU_CHARGE_BOTH ?
             "both groups grew by the allocation" : "neither group grew by the allocation");
      return 4;
   }
   const int group = charge == TG_GPU_CHARGE_LOCAL ? TG_KMT_MEMORY_LOCAL : TG_KMT_MEMORY_NON_LOCAL;
   if (!resident[group].budget) {
      printf("FAIL: the %s group was charged but reports a zero Budget\n", group_names[group]);
      return 5;
   }
   if (released[group].usage + bytes > resident[group].usage)
      printf("NOTE: %s CurrentUsage did not drop by the allocation after the free; "
             "VidMm may release it lazily\n", group_names[group]);
   const struct tg_gpu_heap_budget heap =
      tg_gpu_heap_budget(adapter->memory_budget_bytes, &resident[group], 0, 0);
   printf("MEASURED: ordinary allocations are charged to the %s group; "
          "with NVK_WINDOWS_BUDGET_GROUP=%s the heap would report heapBudget=%llu "
          "heapUsage=%llu\n", group_names[group], group_names[group],
          (unsigned long long)heap.budget, (unsigned long long)heap.usage);
   return 0;
}
