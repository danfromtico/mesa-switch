/* SPDX-License-Identifier: MIT */
/* Real KMT ABI with CPU substitutes: no device opened, no GPU commands run. */
#include "kmt_native.h"
#include "allocation_abi.h"
#include "submit_abi.h"
#include <d3dkmthk.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); ExitProcess(1); } } while (0)
static uint64_t paging;
static unsigned queries, opens, closes, residents, waits, submits;
static HANDLE exported;
static unsigned failure;
static bool fail_close, alias_live;
static uint32_t expected_count;
static LONG submit_status;
static unsigned teardown_failure, destroyed_queues, destroyed_devices, closed_adapters;

NTSTATUS APIENTRY D3DKMTDestroyPagingQueue(D3DDDI_DESTROYPAGINGQUEUE *q)
{
   CHECK(q->hPagingQueue == 7); ++destroyed_queues;
   return teardown_failure & 1 ? TG_KMT_DEVICE_REMOVED : 0;
}
NTSTATUS APIENTRY D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *d)
{
   CHECK(d->hDevice == 2); ++destroyed_devices;
   return teardown_failure & 2 ? TG_KMT_DEVICE_REMOVED : 0;
}
NTSTATUS APIENTRY D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *a)
{
   CHECK(a->hAdapter == 1); ++closed_adapters;
   return teardown_failure & 4 ? TG_KMT_DEVICE_REMOVED : 0;
}

NTSTATUS APIENTRY
D3DKMTShareObjects(UINT count, const D3DKMT_HANDLE *objects,
                   POBJECT_ATTRIBUTES attributes, DWORD access, HANDLE *handle)
{
   CHECK(count == 1 && *objects == 101 && !attributes->ObjectName && !attributes->Attributes);
   CHECK(access == (STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE));
   if (failure == 1) return TG_KMT_INVALID_PARAMETER;
   *handle = exported = CreateEventW(NULL, FALSE, FALSE, NULL);
   CHECK(exported); return 0;
}

NTSTATUS APIENTRY
D3DKMTQueryResourceInfoFromNtHandle(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *q)
{
   CHECK(q->hDevice == 2 && q->hNtHandle == exported); ++queries;
   q->NumAllocations = 1; q->PrivateRuntimeDataSize = 16;
   q->ResourcePrivateDriverDataSize = 72;
   q->TotalPrivateDriverDataSize = sizeof(struct tg_gpu_linear_allocation_request);
   if (failure == 2) q->PrivateRuntimeDataSize = 1024 * 1024 + 1;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTOpenResourceFromNtHandle(D3DKMT_OPENRESOURCEFROMNTHANDLE *o)
{
   CHECK(o->hDevice == 2 && o->hNtHandle == exported && o->NumAllocations == 1);
   CHECK(o->PrivateRuntimeDataSize == 16 && o->ResourcePrivateDriverDataSize == 72);
   CHECK(o->TotalPrivateDriverDataBufferSize == sizeof(struct tg_gpu_linear_allocation_request));
   CHECK(o->pPrivateRuntimeData && o->pResourcePrivateDriverData && o->pTotalPrivateDriverDataBuffer);
   ++opens;
   if (failure == 3) return TG_KMT_INVALID_PARAMETER;
   o->hResource = 202; o->pOpenAllocationInfo2[0].hAllocation = 22; alias_live = true;
   struct tg_gpu_linear_allocation_request storage = {
      {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_LINEAR_SURFACE, 8192},
      21, 32, 32, 128,
   };
   if (failure == 4) storage.Buffer.bytes = 4096;
   memcpy(o->pTotalPrivateDriverDataBuffer, &storage, sizeof(storage));
   o->pOpenAllocationInfo2[0].PrivateDriverDataSize = sizeof(storage);
   o->pOpenAllocationInfo2[0].pPrivateDriverData = failure == 5 ?
      (void *)(uintptr_t)1 : o->pTotalPrivateDriverDataBuffer;
   return failure == 10 ? TG_KMT_INVALID_PARAMETER : 0; /* partial failed open */
}

NTSTATUS APIENTRY
D3DKMTMakeResident(D3DDDI_MAKERESIDENT *m)
{
   CHECK(alias_live && m->hPagingQueue == 7 && m->NumAllocations == 1 && m->AllocationList[0] == 22);
   CHECK(!m->Flags.Value); ++residents;
   if (failure == 6) { m->NumBytesToTrim = 4096; return TG_KMT_NO_MEMORY; }
   m->PagingFenceValue = 7; return 0x103; /* STATUS_PENDING */
}

NTSTATUS APIENTRY
D3DKMTWaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w)
{
   CHECK(w->hDevice == 2 && w->ObjectCount == 1 && w->ObjectHandleArray[0] == 8 &&
         w->FenceValueArray[0] == 7 && !w->hAsyncEvent && !w->Flags.Value);
   ++waits; paging = failure == 8 ? UINT64_MAX : 7;
   return failure == 9 ? TG_KMT_DEVICE_REMOVED : 0;
}

NTSTATUS APIENTRY
D3DKMTDestroyAllocation2(const D3DKMT_DESTROYALLOCATION2 *d)
{
   CHECK(alias_live && d->hDevice == 2 && d->hResource == 202 && !d->AllocationCount && !d->phAllocationList);
   ++closes;
   if (fail_close) return TG_KMT_DEVICE_REMOVED;
   alias_live = false; return 0;
}

NTSTATUS APIENTRY
D3DKMTSubmitCommand(const D3DKMT_SUBMITCOMMAND *s)
{
   const D3DKMT_SUBMITCOMMANDFLAGS empty_flags = {0};
   CHECK(s->Commands == 0x20000 && s->CommandLength == 16 &&
         !memcmp(&s->Flags, &empty_flags, sizeof(empty_flags)));
   CHECK(s->BroadcastContextCount == 1 && s->BroadcastContext[0] == 9);
   CHECK(s->NumPrimaries == expected_count && !s->PresentHistoryToken && !s->NumHistoryBuffers);
   const struct tg_gpu_submit_request *r = s->pPrivateDriverData;
   CHECK(r->version == TG_GPU_SUBMIT_ABI && r->size == sizeof(*r) + 16 && r->command_bytes == 16);
   CHECK(s->PrivateDriverDataSize == r->size);
   for (UINT i = 0; i < s->NumPrimaries; ++i) CHECK(s->WrittenPrimaries[i] == 22 + i);
   ++submits; return submit_status;
}

static void reset(void)
{
   CHECK(!alias_live);
   paging = 0; queries = opens = closes = residents = waits = submits = 0;
   exported = NULL; fail_close = false;
}

int main(void)
{
   struct tg_kmt_device dev = {.handle = 2, .paging_queue = 7, .paging = {8, &paging},
                               .budget = 512u * 1024 * 1024, .allocated = 123456};
   InitializeSRWLock(&dev.memory_lock);
   for (failure = 0; failure <= 10; ++failure) {
      reset(); if (failure == 7) paging = UINT64_MAX;
      struct tg_kmt_runtime_alias alias = {0};
      LONG status = tg_kmt_open_runtime_alias(&dev, 101, 8192, &alias);
      CHECK(dev.allocated == 123456);
      if (!failure) {
         CHECK(status == 0 && alias.device == 2 && alias.resource == 202 && alias.allocation == 22);
         CHECK(queries == 1 && opens == 1 && residents == 1 && waits == 1);
         CHECK(tg_kmt_open_runtime_alias(&dev, 101, 8192, &alias) == TG_KMT_INVALID_PARAMETER);
         struct tg_kmt_device wrong = {.handle = 1};
         CHECK(tg_kmt_close_runtime_alias(&wrong, &alias) == TG_KMT_INVALID_PARAMETER && alias_live);
         fail_close = true;
         CHECK(tg_kmt_close_runtime_alias(&dev, &alias) == TG_KMT_DEVICE_REMOVED && alias.allocation == 22);
         fail_close = false;
         CHECK(tg_kmt_close_runtime_alias(&dev, &alias) == 0 && !alias.device && !alias_live);
      } else {
         CHECK(status < 0 && !alias.device && !alias_live);
         CHECK(opens == (failure >= 3 ? 1u : 0u));
         CHECK(closes == (failure >= 4 ? 1u : 0u));
         if (failure == 7 || failure == 8) CHECK(status == TG_KMT_DEVICE_REMOVED);
         if (failure == 7) CHECK(!waits);
      }
      DWORD flags;
      if (exported) CHECK(!GetHandleInformation(exported, &flags));
   }
   reset(); failure = 4; fail_close = true;
   struct tg_kmt_runtime_alias pending = {0};
   CHECK(tg_kmt_open_runtime_alias(&dev, 101, 8192, &pending) == TG_KMT_DEVICE_REMOVED);
   CHECK(pending.allocation == 22 && alias_live && dev.allocated == 123456);
   fail_close = false; CHECK(tg_kmt_close_runtime_alias(&dev, &pending) == 0);

   uint32_t words[4] = {1, 2, 3, 4}, primaries[17];
   for (UINT i = 0; i < 17; ++i) primaries[i] = 22 + i;
   for (expected_count = 0; expected_count <= 16; expected_count += 8)
      CHECK(tg_kmt_submit_primaries(9, 0x20000, words, sizeof(words), expected_count, primaries) == 0);
   const unsigned previous = submits;
   CHECK(tg_kmt_submit_primaries(9, 0x20000, words, sizeof(words), 17, primaries) == TG_KMT_INVALID_PARAMETER);
   CHECK(tg_kmt_submit_primaries(9, 0x20000, words, sizeof(words), 1, NULL) == TG_KMT_INVALID_PARAMETER);
   primaries[0] = 0;
   CHECK(tg_kmt_submit_primaries(9, 0x20000, words, sizeof(words), 1, primaries) == TG_KMT_INVALID_PARAMETER);
   primaries[0] = primaries[1];
   CHECK(tg_kmt_submit_primaries(9, 0x20000, words, sizeof(words), 2, primaries) == TG_KMT_INVALID_PARAMETER);
   CHECK(submits == previous);
   expected_count = 0; CHECK(tg_kmt_submit(9, 0x20000, words, sizeof(words)) == 0);
   primaries[0] = 22; expected_count = 1; submit_status = TG_KMT_DEVICE_REMOVED;
   CHECK(tg_kmt_submit_primaries(9, 0x20000, words, sizeof(words), 1, primaries) == submit_status);
   for (unsigned mask = 0; mask < 8; ++mask) {
      dev.handle = 2; dev.adapter = 1; dev.paging_queue = 7;
      dev.paging = (struct tg_kmt_fence){8, &paging}; dev.allocated = 123456;
      teardown_failure = mask; destroyed_queues = destroyed_devices = closed_adapters = 0;
      LONG status = tg_kmt_close_device(&dev);
      CHECK((status < 0) == (mask != 0));
      CHECK(destroyed_queues == 1 && destroyed_devices == 1);
      CHECK(closed_adapters == (mask & 2 ? 0u : 1u));
      CHECK(dev.handle == (mask & 2 ? 2u : 0u));
      CHECK(dev.paging_queue == ((mask & 3) == 3 ? 7u : 0u));
      CHECK(dev.paging.handle == ((mask & 3) == 3 ? 8u : 0u));
      CHECK(dev.adapter == (mask & 6 ? 1u : 0u));
      teardown_failure = 0;
      CHECK(tg_kmt_close_device(&dev) == 0);
      CHECK(!dev.handle && !dev.adapter && !dev.paging_queue && !dev.paging.handle && !dev.allocated);
   }
   puts("PASS: submitting-device aliases, bounded metadata, residency/removal, failed cleanup ownership and WrittenPrimaries (CPU control)");
   return 0;
}
