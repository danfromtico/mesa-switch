/* SPDX-License-Identifier: MIT */
#include "private_memory.h"
#include "../core/gm20b_mmu.h"

NTSTATUS
TgGpuAllocatePrivateMemory(const DXGKRNL_INTERFACE *Dxgk,
    SIZE_T Bytes, UINT64 LowestAddress, UINT64 HighestAddress,
    TG_GPU_PRIVATE_MEMORY *Memory)
{
    DXGKARGCB_ALLOCATECONTIGUOUSMEMORY args = {0};
    NTSTATUS status;
    PAGED_CODE();
    if (!Dxgk || !Bytes || (Bytes & (PAGE_SIZE - 1)) || !Memory ||
        LowestAddress > HighestAddress || HighestAddress >= TG_GPU_VA_LIMIT ||
        Bytes - 1 > HighestAddress - LowestAddress)
        return STATUS_INVALID_PARAMETER;
    if (Memory->Handle)
        return STATUS_INVALID_DEVICE_STATE;
    if (Dxgk->Version < DXGKDDI_INTERFACE_VERSION_WDDM2_4 ||
        !Dxgk->DxgkCbAllocateContiguousMemory || !Dxgk->DxgkCbFreeContiguousMemory)
        return STATUS_NOT_SUPPORTED;

    args.NumberOfBytes = Bytes;
    args.LowestAcceptableAddress.QuadPart = (LONGLONG)LowestAddress;
    args.HighestAcceptableAddress.QuadPart = (LONGLONG)HighestAddress;
    args.CacheType = DXGK_MEMORY_CACHING_TYPE_NON_CACHED;
    status = Dxgk->DxgkCbAllocateContiguousMemory(Dxgk->DeviceHandle, &args);
    if (!NT_SUCCESS(status))
        return status;
    Memory->Handle = args.hMemoryHandle;
    Memory->CpuAddress = args.pMemory;
    Memory->SystemPhysicalAddress = MmGetPhysicalAddress(args.pMemory);
    Memory->Bytes = Bytes;
    RtlZeroMemory(Memory->CpuAddress, Bytes);
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

NTSTATUS
TgGpuFreePrivateMemory(const DXGKRNL_INTERFACE *Dxgk, TG_GPU_PRIVATE_MEMORY *Memory)
{
    DXGKARGCB_FREECONTIGUOUSMEMORY args;
    NTSTATUS status;
    PAGED_CODE();
    if (!Dxgk || !Memory) return STATUS_INVALID_PARAMETER;
    if (!Memory->Handle)
        return STATUS_SUCCESS;
    if (!Dxgk->DxgkCbFreeContiguousMemory) return STATUS_NOT_SUPPORTED;
    args.hMemoryHandle = Memory->Handle;
    status = Dxgk->DxgkCbFreeContiguousMemory(Dxgk->DeviceHandle, &args);
    if (NT_SUCCESS(status))
        RtlZeroMemory(Memory, sizeof(*Memory));
    return status;
}
