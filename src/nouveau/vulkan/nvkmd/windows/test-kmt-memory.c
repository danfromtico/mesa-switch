/* SPDX-License-Identifier: MIT */
/* VidMm budget queries and heap rules with controlled KMT substitutes. */
#include "kmt_native.h"
#include <d3dkmthk.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) ExitProcess(__LINE__); } while (0)

#define MIB (UINT64_C(1) << 20)
#define DENIED ((LONG)0xc0000022)
#define UNSUPPORTED ((LONG)0xc00000bb)
#define BAD_HANDLE ((LONG)0xc0000008)
static const struct tg_gpu_adapter_info adapter = {
   .memory_budget_bytes = TG_GPU_MEMORY_BUDGET_BYTES, .luid_low = 0x89abcdef, .luid_high = 0x1234,
};
static UINT opens, closes, queries, statistics;
static LONG open_status, close_status, query_status[TG_KMT_MEMORY_GROUPS], statistics_status;
static struct tg_kmt_video_memory live[TG_KMT_MEMORY_GROUPS];

NTSTATUS APIENTRY
D3DKMTOpenAdapterFromLuid(CONST D3DKMT_OPENADAPTERFROMLUID *in)
{
   D3DKMT_OPENADAPTERFROMLUID *args = (D3DKMT_OPENADAPTERFROMLUID *)in;
   CHECK(args->AdapterLuid.LowPart == 0x89abcdef && args->AdapterLuid.HighPart == 0x1234);
   if (open_status < 0)
      return open_status;
   CHECK(opens++ == closes);
   args->hAdapter = 0x40000040 + opens;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *args)
{
   CHECK(opens == closes + 1 && args->hAdapter == 0x40000040 + opens);
   closes++;
   return close_status;
}

NTSTATUS APIENTRY
D3DKMTQueryVideoMemoryInfo(D3DKMT_QUERYVIDEOMEMORYINFO *args)
{
   /* Current process, adapter 0 of this open, and exactly one group. */
   CHECK(opens == closes + 1 && args->hAdapter == 0x40000040 + opens &&
         !args->hProcess && !args->PhysicalAdapterIndex &&
         (UINT)args->MemorySegmentGroup < TG_KMT_MEMORY_GROUPS &&
         (UINT)args->MemorySegmentGroup == queries % TG_KMT_MEMORY_GROUPS);
   CHECK(!args->Budget && !args->CurrentUsage && !args->CurrentReservation &&
         !args->AvailableForReservation);
   const UINT group = args->MemorySegmentGroup;
   queries++;
   if (query_status[group] < 0)
      return query_status[group];
   args->Budget = live[group].budget;
   args->CurrentUsage = live[group].usage;
   args->CurrentReservation = live[group].reservation;
   args->AvailableForReservation = live[group].available_for_reservation;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTQueryStatistics(const D3DKMT_QUERYSTATISTICS *args)
{
   D3DKMT_QUERYSTATISTICS *out = (D3DKMT_QUERYSTATISTICS *)args;
   CHECK(args->Type == D3DKMT_QUERYSTATISTICS_SEGMENT && !args->hProcess &&
         args->AdapterLuid.LowPart == 0x89abcdef && args->AdapterLuid.HighPart == 0x1234 &&
         args->QuerySegment.SegmentId == 2);
   statistics++;
   if (statistics_status < 0)
      return statistics_status;
   out->QueryResult.SegmentInformation.CommitLimit = 11 * MIB;
   out->QueryResult.SegmentInformation.BytesCommitted = 7 * MIB;
   out->QueryResult.SegmentInformation.BytesResident = 5 * MIB;
   out->QueryResult.SegmentInformation.Aperture = 1;
   out->QueryResult.SegmentInformation.SegmentProperties.SegmentType = 2;
   return 0;
}

static bool
same(const struct tg_kmt_video_memory *a, const struct tg_kmt_video_memory *b)
{
   return !memcmp(a, b, sizeof(*a));
}

int main(void)
{
   struct tg_kmt_video_memory groups[TG_KMT_MEMORY_GROUPS];
   const struct tg_kmt_video_memory zero[TG_KMT_MEMORY_GROUPS] = {0};

   /* Four distinct fields per group reach the caller unmerged. */
   live[TG_KMT_MEMORY_LOCAL] = (struct tg_kmt_video_memory){300 * MIB, 10 * MIB, 3 * MIB, 150 * MIB};
   live[TG_KMT_MEMORY_NON_LOCAL] = (struct tg_kmt_video_memory){700 * MIB, 20 * MIB, 5 * MIB, 350 * MIB};
   CHECK(tg_kmt_query_video_memory(&adapter, groups) == 0);
   CHECK(opens == 1 && closes == 1 && queries == 2);
   CHECK(same(&groups[TG_KMT_MEMORY_LOCAL], &live[TG_KMT_MEMORY_LOCAL]) &&
         same(&groups[TG_KMT_MEMORY_NON_LOCAL], &live[TG_KMT_MEMORY_NON_LOCAL]));

   /* Each failure keeps its own NTSTATUS, zeroes the outputs and still
    * closes the adapter exactly once. */
   memset(groups, 0xa5, sizeof(groups));
   open_status = DENIED;
   CHECK(tg_kmt_query_video_memory(&adapter, groups) == DENIED);
   CHECK(opens == 1 && closes == 1 && queries == 2 && !memcmp(groups, zero, sizeof(zero)));
   open_status = 0;

   memset(groups, 0xa5, sizeof(groups));
   query_status[TG_KMT_MEMORY_LOCAL] = TG_KMT_INVALID_PARAMETER;
   CHECK(tg_kmt_query_video_memory(&adapter, groups) == TG_KMT_INVALID_PARAMETER);
   CHECK(opens == 2 && closes == 2 && queries == 3 && !memcmp(groups, zero, sizeof(zero)));
   query_status[TG_KMT_MEMORY_LOCAL] = 0;
   queries = 0;

   memset(groups, 0xa5, sizeof(groups));
   query_status[TG_KMT_MEMORY_NON_LOCAL] = UNSUPPORTED;
   close_status = BAD_HANDLE; /* the earlier failure wins */
   CHECK(tg_kmt_query_video_memory(&adapter, groups) == UNSUPPORTED);
   CHECK(opens == 3 && closes == 3 && queries == 2 && !memcmp(groups, zero, sizeof(zero)));
   query_status[TG_KMT_MEMORY_NON_LOCAL] = 0;

   memset(groups, 0xa5, sizeof(groups));
   CHECK(tg_kmt_query_video_memory(&adapter, groups) == BAD_HANDLE);
   CHECK(opens == 4 && closes == 4 && queries == 4 && !memcmp(groups, zero, sizeof(zero)));
   close_status = 0;

   struct tg_kmt_segment_statistics segment;
   CHECK(tg_kmt_query_segment(&adapter, 2, &segment) == 0 && statistics == 1);
   CHECK(segment.commit_limit == 11 * MIB && segment.committed == 7 * MIB &&
         segment.resident == 5 * MIB && segment.aperture == 1 &&
         segment.properties == UINT64_C(2) << 2);
   statistics_status = TG_KMT_INVALID_PARAMETER;
   memset(&segment, 0xa5, sizeof(segment));
   CHECK(tg_kmt_query_segment(&adapter, 2, &segment) == TG_KMT_INVALID_PARAMETER &&
         !segment.commit_limit && !segment.committed && !segment.resident &&
         !segment.aperture && !segment.properties);
   CHECK(opens == 4 && closes == 4);

   /* Heap rules. Unmeasured: the estimates pass through untouched. */
   const uint64_t ceiling = TG_GPU_MEMORY_BUDGET_BYTES;
   struct tg_gpu_heap_budget heap = tg_gpu_heap_budget(ceiling, NULL, 400 * MIB, 9 * MIB);
   CHECK(!heap.measured && heap.budget == 400 * MIB && heap.usage == 9 * MIB);
   /* Measured below the ceiling: VidMm's pair, not the estimate, and the
    * reservation is not usage. */
   heap = tg_gpu_heap_budget(ceiling, &live[TG_KMT_MEMORY_LOCAL], 400 * MIB, 9 * MIB);
   CHECK(heap.measured && heap.budget == 300 * MIB && heap.usage == 10 * MIB);
   /* Measured above the ceiling: 512 MiB stays the bound; usage is kept. */
   heap = tg_gpu_heap_budget(ceiling, &live[TG_KMT_MEMORY_NON_LOCAL], 400 * MIB, 9 * MIB);
   CHECK(heap.measured && heap.budget == UINT64_C(536870912) && heap.usage == 20 * MIB);
   /* Usage above budget is reported as measured, not clamped. */
   struct tg_kmt_video_memory over = {64 * MIB, 96 * MIB, 0, 0};
   heap = tg_gpu_heap_budget(ceiling, &over, 400 * MIB, 9 * MIB);
   CHECK(heap.measured && heap.budget == 64 * MIB && heap.usage == 96 * MIB);
   /* A zero Budget means the group holds no segment: neither value is used. */
   struct tg_kmt_video_memory empty = {0, 96 * MIB, 5 * MIB, 0};
   heap = tg_gpu_heap_budget(ceiling, &empty, 400 * MIB, 9 * MIB);
   CHECK(!heap.measured && heap.budget == 400 * MIB && heap.usage == 9 * MIB);

   CHECK(tg_gpu_parse_budget_group(NULL) == TG_KMT_MEMORY_UNMEASURED &&
         tg_gpu_parse_budget_group("") == TG_KMT_MEMORY_UNMEASURED &&
         tg_gpu_parse_budget_group("Local") == TG_KMT_MEMORY_UNMEASURED &&
         tg_gpu_parse_budget_group("both") == TG_KMT_MEMORY_UNMEASURED &&
         tg_gpu_parse_budget_group("local") == TG_KMT_MEMORY_LOCAL &&
         tg_gpu_parse_budget_group("non-local") == TG_KMT_MEMORY_NON_LOCAL);

   /* Charge attribution around one 32 MiB allocation. */
   struct tg_kmt_video_memory before[TG_KMT_MEMORY_GROUPS] = {{.usage = 10 * MIB}, {.usage = 20 * MIB}};
   struct tg_kmt_video_memory after[TG_KMT_MEMORY_GROUPS] = {{.usage = 42 * MIB}, {.usage = 20 * MIB}};
   CHECK(tg_gpu_classify_charge(before, after, 32 * MIB) == TG_GPU_CHARGE_LOCAL);
   after[0].usage = 10 * MIB; after[1].usage = 53 * MIB;
   CHECK(tg_gpu_classify_charge(before, after, 32 * MIB) == TG_GPU_CHARGE_NON_LOCAL);
   after[0].usage = 42 * MIB;
   CHECK(tg_gpu_classify_charge(before, after, 32 * MIB) == TG_GPU_CHARGE_BOTH);
   after[0].usage = 41 * MIB; after[1].usage = 20 * MIB; /* one MiB short */
   CHECK(tg_gpu_classify_charge(before, after, 32 * MIB) == TG_GPU_CHARGE_NONE);
   after[0].usage = 2 * MIB; /* shrinking usage is not a charge */
   CHECK(tg_gpu_classify_charge(before, after, 32 * MIB) == TG_GPU_CHARGE_NONE);
   /* A reservation change alone is not a charge. */
   after[0] = before[0]; after[0].reservation = 64 * MIB;
   CHECK(tg_gpu_classify_charge(before, after, 32 * MIB) == TG_GPU_CHARGE_NONE);
   CHECK(tg_gpu_classify_charge(before, before, 0) == TG_GPU_CHARGE_NONE);
   return 0;
}
