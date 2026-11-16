/* SPDX-License-Identifier: MIT */
#include "nvdec_memory.h"
#define TAG 'mvdN'

typedef struct _TG_NVDEC_MEMORY_RECORD {
    LIST_ENTRY Link;
    TG_GPU_PRIVATE_MEMORY Memory;
    SIZE_T RequestedBytes;
    BOOLEAN Retired;
} TG_NVDEC_MEMORY_RECORD;

NTSTATUS TgNvdecMemoryInit(TG_NVDEC_MEMORY *pool, const DXGKRNL_INTERFACE *dxgk)
{
    PAGED_CODE();
    if (!pool || pool->Dxgk || !dxgk || !dxgk->DeviceHandle)
        return STATUS_INVALID_PARAMETER;
    if (dxgk->Version < DXGKDDI_INTERFACE_VERSION_WDDM2_4 ||
        !dxgk->DxgkCbAllocateContiguousMemory || !dxgk->DxgkCbFreeContiguousMemory)
        return STATUS_NOT_SUPPORTED;
    InitializeListHead(&pool->Allocations);
    pool->Dxgk = dxgk;
    pool->LastStatus = STATUS_SUCCESS;
    pool->Corrupt = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS free_record(TG_NVDEC_MEMORY *pool, TG_NVDEC_MEMORY_RECORD *record)
{
    NTSTATUS status = TgGpuFreePrivateMemory(pool->Dxgk, &record->Memory);
    if (!NT_SUCCESS(status)) {
        pool->LastStatus = status;
        return status;
    }
    RemoveEntryList(&record->Link);
    ExFreePoolWithTag(record, TAG);
    return STATUS_SUCCESS;
}

NTSTATUS TgNvdecMemoryRetryRetired(TG_NVDEC_MEMORY *pool)
{
    NTSTATUS result = STATUS_SUCCESS;
    PLIST_ENTRY link, next;
    PAGED_CODE();
    if (!pool || !pool->Dxgk)
        return STATUS_INVALID_PARAMETER;
    for (link = pool->Allocations.Flink; link != &pool->Allocations; link = next) {
        TG_NVDEC_MEMORY_RECORD *r = CONTAINING_RECORD(link, TG_NVDEC_MEMORY_RECORD, Link);
        next = link->Flink;
        if (r->Retired) {
            NTSTATUS status = free_record(pool, r);
            if (!NT_SUCCESS(status) && NT_SUCCESS(result)) result = status;
        }
    }
    if (pool->Corrupt) result = STATUS_INVALID_DEVICE_STATE;
    pool->LastStatus = result;
    return result;
}

BOOLEAN TgNvdecMemoryAllocate(TG_NVDEC_MEMORY *pool, SIZE_T bytes, struct tg_nvdec_dma *out)
{
    TG_NVDEC_MEMORY_RECORD *r;
    SIZE_T aligned;
    UINT64 physical;
    NTSTATUS status;
    PAGED_CODE();
    if (!pool || !pool->Dxgk || !out || out->cpu || out->bus || out->size ||
        !bytes || bytes > (SIZE_T)UINT32_MAX - (PAGE_SIZE - 1))
        return FALSE;
    if (!NT_SUCCESS(TgNvdecMemoryRetryRetired(pool))) return FALSE;
    aligned = (bytes + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    r = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*r), TAG);
    if (!r) { pool->LastStatus = STATUS_NO_MEMORY; return FALSE; }
    RtlZeroMemory(r, sizeof(*r));
    status = TgGpuAllocatePrivateMemory(pool->Dxgk, aligned, 0, UINT32_MAX, &r->Memory);
    if (!NT_SUCCESS(status)) {
        pool->LastStatus = status;
        ExFreePoolWithTag(r, TAG);
        return FALSE;
    }
    r->RequestedBytes = bytes;
    InsertTailList(&pool->Allocations, &r->Link);
    physical = (UINT64)r->Memory.SystemPhysicalAddress.QuadPart;
    /* Only the already-validated direct client route may consume this
     * address. The current Host1x encoder admits low32 buffers only. */
    if (!physical || (physical & 255u) || physical > UINT32_MAX ||
        aligned > UINT64_C(0x100000000) - physical) {
        r->Retired = TRUE;
        pool->LastStatus = STATUS_INVALID_ADDRESS;
        (VOID)free_record(pool, r);
        return FALSE;
    }
    out->cpu = r->Memory.CpuAddress;
    out->bus = physical;
    out->size = bytes;
    return TRUE;
}

VOID TgNvdecMemoryRetire(TG_NVDEC_MEMORY *pool, struct tg_nvdec_dma *dma)
{
    PAGED_CODE();
    if (!pool || !pool->Dxgk || !dma) return;
    if (!dma->cpu && !dma->bus && !dma->size) return;
    for (PLIST_ENTRY link = pool->Allocations.Flink; link != &pool->Allocations; link = link->Flink) {
        TG_NVDEC_MEMORY_RECORD *r = CONTAINING_RECORD(link, TG_NVDEC_MEMORY_RECORD, Link);
        if (r->Memory.CpuAddress != dma->cpu || r->Retired) continue;
        if (dma->bus != (UINT64)r->Memory.SystemPhysicalAddress.QuadPart ||
            dma->size != r->RequestedBytes) break;
        r->Retired = TRUE;
        /* Ownership moves to the pool's retired list before the codec may
         * erase the descriptor. No successful OS free is invented. */
        RtlZeroMemory(dma, sizeof(*dma));
        (VOID)free_record(pool, r);
        return;
    }
    pool->Corrupt = TRUE;
    pool->LastStatus = STATUS_INVALID_DEVICE_STATE;
}

NTSTATUS TgNvdecMemoryFinish(TG_NVDEC_MEMORY *pool)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!pool) return STATUS_INVALID_PARAMETER;
    if (!pool->Dxgk) return STATUS_SUCCESS;
    status = TgNvdecMemoryRetryRetired(pool);
    if (!IsListEmpty(&pool->Allocations))
        return NT_SUCCESS(status) ? STATUS_DEVICE_BUSY : status;
    /* A rejected foreign descriptor must not pin an empty owner forever.
     * All allocations actually belonging to this pool have been freed. */
    RtlZeroMemory(pool, sizeof(*pool));
    return STATUS_SUCCESS;
}
