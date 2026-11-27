/* SPDX-License-Identifier: MIT */
#include "table_segment.h"

#define T214_TABLE_LOW  UINT64_C(0x80000000)
#define T214_TABLE_HIGH UINT64_C(0xffffffff)

static BOOLEAN ValidContiguousMemory(const TG_GPU_PRIVATE_MEMORY *Memory)
{
    UINT64 base;
    if (!Memory->Handle || !Memory->CpuAddress ||
        ((ULONG_PTR)Memory->CpuAddress & (PAGE_SIZE - 1)) ||
        !Memory->Bytes || (Memory->Bytes & (PAGE_SIZE - 1)))
        return FALSE;
    base = (UINT64)Memory->SystemPhysicalAddress.QuadPart;
    return base >= T214_TABLE_LOW && base <= T214_TABLE_HIGH &&
        !(base & (PAGE_SIZE - 1)) &&
        Memory->Bytes - 1 <= T214_TABLE_HIGH - base;
}

static BOOLEAN ValidSegment(const TG_GPU_TABLE_SEGMENT *Segment)
{
    return Segment && Segment->SegmentId && Segment->SegmentId <= 31 &&
        !(Segment->Memory.Bytes % TG_GPU_TABLE_BYTES) &&
        ValidContiguousMemory(&Segment->Memory);
}

NTSTATUS TgGpuCreateTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    UINT SegmentId, SIZE_T Bytes, TG_GPU_TABLE_SEGMENT *Segment)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!Segment || !SegmentId || SegmentId > 31 || !Bytes ||
        Bytes % TG_GPU_TABLE_BYTES)
        return STATUS_INVALID_PARAMETER;
    if (Segment->Memory.Handle || Segment->SegmentId)
        return STATUS_INVALID_DEVICE_STATE;
    status = TgGpuAllocatePrivateMemory(Dxgk, Bytes, T214_TABLE_LOW,
        T214_TABLE_HIGH, &Segment->Memory);
    if (!NT_SUCCESS(status)) return status;
    Segment->SegmentId = SegmentId;
    RtlZeroMemory((PVOID)Segment->AperturePfns, sizeof(Segment->AperturePfns));
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuDestroyTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    TG_GPU_TABLE_SEGMENT *Segment)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!Segment) return STATUS_INVALID_PARAMETER;
    status = TgGpuFreePrivateMemory(Dxgk, &Segment->Scanout);
    if (!NT_SUCCESS(status)) return status;
    status = TgGpuFreePrivateMemory(Dxgk, &Segment->Memory);
    if (NT_SUCCESS(status)) RtlZeroMemory(Segment, sizeof(*Segment));
    return status;
}

NTSTATUS TgGpuDescribeTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    DXGK_SEGMENTDESCRIPTOR3 *Description)
{
    if (!Description || !ValidSegment(Segment)) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Description, sizeof(*Description));
    Description->BaseAddress = Segment->Memory.SystemPhysicalAddress;
    Description->CpuTranslatedAddress = Segment->Memory.SystemPhysicalAddress;
    Description->Size = Description->CommitLimit = Segment->Memory.Bytes;
    Description->Flags.CpuVisible = 1;
    Description->Flags.PopulatedFromSystemMemory = 1;
    Description->Flags.LocalBudgetGroup = 1;
    /* A real memory segment, not an aperture. CPU mapping is uncached;
     * neither cache coherence, direct scanout nor sleep retention is claimed.
     */
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuCreateScanoutSegment(const DXGKRNL_INTERFACE *Dxgk,
    TG_GPU_TABLE_SEGMENT *Segment)
{
    PAGED_CODE();
    if (!ValidSegment(Segment) || Segment->SegmentId == TG_GPU_SCANOUT_SEGMENT)
        return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = TgGpuAllocatePrivateMemory(Dxgk, TG_GPU_SCANOUT_SEGMENT_BYTES,
        T214_TABLE_LOW, T214_TABLE_HIGH, &Segment->Scanout);
    if (!NT_SUCCESS(status))
        return status;
    /* On failure StopDevice owns cleanup of the retained handle. */
    return ValidContiguousMemory(&Segment->Scanout) ? STATUS_SUCCESS : STATUS_INVALID_ADDRESS;
}

NTSTATUS TgGpuDescribeScanoutSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    DXGK_SEGMENTDESCRIPTOR4 *Description)
{
    if (!Segment || !Description || !ValidContiguousMemory(&Segment->Scanout))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Description, sizeof(*Description));
    Description->BaseAddress = Segment->Scanout.SystemPhysicalAddress;
    Description->CpuTranslatedAddress = Segment->Scanout.SystemPhysicalAddress;
    Description->Size = Segment->Scanout.Bytes;
    Description->Flags.CpuVisible = 1;
    Description->Flags.PopulatedFromSystemMemory = 1;
    Description->Flags.LocalBudgetGroup = 1;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuResolveScanoutSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    UINT64 ByteOffset, SIZE_T Bytes, UINT64 *DeviceAddress)
{
    if (!Segment || !DeviceAddress || !ValidContiguousMemory(&Segment->Scanout) ||
        (ByteOffset & (PAGE_SIZE - 1)) || !Bytes || ByteOffset >= Segment->Scanout.Bytes ||
        Bytes > Segment->Scanout.Bytes - ByteOffset)
        return STATUS_INVALID_ADDRESS;
    *DeviceAddress = (UINT64)Segment->Scanout.SystemPhysicalAddress.QuadPart + ByteOffset;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuResolveTableSegment(PVOID Context, UINT SegmentId,
    UINT64 ByteOffset, SIZE_T Bytes, UINT64 *DeviceAddress)
{
    const TG_GPU_TABLE_SEGMENT *segment = Context;
    if (!DeviceAddress || !ValidSegment(segment) || SegmentId != segment->SegmentId ||
        !Bytes || (Bytes & (PAGE_SIZE - 1)) || (ByteOffset & (PAGE_SIZE - 1)) ||
        ByteOffset > segment->Memory.Bytes || Bytes > segment->Memory.Bytes - ByteOffset)
        return STATUS_INVALID_ADDRESS;
    *DeviceAddress = (UINT64)segment->Memory.SystemPhysicalAddress.QuadPart + ByteOffset;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuResolveScanoutAddress(const TG_GPU_TABLE_SEGMENT *segment,
    UINT64 address,SIZE_T bytes,UINT64 *device_address)
{
    if(!segment || address<(UINT64)segment->Scanout.SystemPhysicalAddress.QuadPart)
        return STATUS_INVALID_ADDRESS;
    return TgGpuResolveScanoutSegment(segment,
        address-(UINT64)segment->Scanout.SystemPhysicalAddress.QuadPart,bytes,device_address);
}

NTSTATUS TgGpuUpdatePagingAperture(TG_GPU_TABLE_SEGMENT *Segment,
    const DXGKARG_BUILDPAGINGBUFFER *Args)
{
    const SIZE_T capacity = TG_GPU_PAGING_APERTURE_BYTES / PAGE_SIZE;
    const PFN_NUMBER *pages = NULL;
    SIZE_T offset, count;
    UINT id;

    if (!ValidSegment(Segment) || !Args || Args->MultipassOffset)
        return STATUS_INVALID_PARAMETER;
    if (Args->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT) {
        PMDL mdl = Args->MapApertureSegment.pMdl;
        SIZE_T start = Args->MapApertureSegment.MdlOffset;
        SIZE_T available;

        id = Args->MapApertureSegment.SegmentId;
        offset = Args->MapApertureSegment.OffsetInPages;
        count = Args->MapApertureSegment.NumberOfPages;
        if (Args->MapApertureSegment.Flags.Value)
            return STATUS_NOT_SUPPORTED;
        if (!mdl || !MmGetMdlByteCount(mdl))
            return STATUS_INVALID_PARAMETER;
        available = ((SIZE_T)MmGetMdlByteOffset(mdl) + MmGetMdlByteCount(mdl) +
            PAGE_SIZE - 1) >> PAGE_SHIFT;
        if (start > available || count > available - start)
            return STATUS_INVALID_PARAMETER;
        pages = MmGetMdlPfnArray(mdl) + start;
    } else if (Args->Operation == DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        id = Args->UnmapApertureSegment.SegmentId;
        offset = Args->UnmapApertureSegment.OffsetInPages;
        count = Args->UnmapApertureSegment.NumberOfPages;
    } else return STATUS_NOT_SUPPORTED;

    if (id != TG_GPU_PAGING_APERTURE_SEGMENT || id == Segment->SegmentId ||
        !count || offset >= capacity || count > capacity - offset)
        return STATUS_INVALID_PARAMETER;
    if (pages) {
        for (SIZE_T i = 0; i < count; ++i)
            if (pages[i] < (T214_TABLE_LOW >> PAGE_SHIFT) ||
                pages[i] >= (T214_SYSTEM_LIMIT >> PAGE_SHIFT))
                return STATUS_INVALID_ADDRESS;
    }
    for (SIZE_T i = 0; i < count; ++i) {
        LONG pfn = pages ? (LONG)pages[i] : 0;
        WriteRelease(&Segment->AperturePfns[offset + i], pfn);
    }
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuResolveVidMmPage(PVOID Context, UINT SegmentId,
    UINT64 ByteAddress, SIZE_T Bytes, UINT64 *DeviceAddress)
{
    const TG_GPU_TABLE_SEGMENT *segment = Context;
    /* On T214, GPU address bit 34 selects Tegra SMMU translation. This
     * physical path must leave it clear and must not truncate >4 GiB pages.
     * VidMm, not this resolver, establishes that the address is resident RAM.
     */
    if (SegmentId && segment && SegmentId == segment->SegmentId)
        return TgGpuResolveTableSegment(Context, SegmentId, ByteAddress, Bytes, DeviceAddress);
    if (SegmentId == TG_GPU_SCANOUT_SEGMENT && Bytes == PAGE_SIZE)
        return TgGpuResolveScanoutSegment(segment, ByteAddress, Bytes, DeviceAddress);
    if (!DeviceAddress || !ValidSegment(segment) || Bytes != PAGE_SIZE ||
        (ByteAddress & (PAGE_SIZE - 1)) ||
        (SegmentId && SegmentId != TG_GPU_PAGING_APERTURE_SEGMENT))
        return STATUS_INVALID_ADDRESS;
    if (SegmentId) {
        if (ByteAddress >= TG_GPU_PAGING_APERTURE_BYTES)
            return STATUS_INVALID_ADDRESS;
        ByteAddress = (UINT64)(ULONG)ReadAcquire(&segment->AperturePfns[ByteAddress >> PAGE_SHIFT]) << PAGE_SHIFT;
    }
    if (ByteAddress < T214_TABLE_LOW || ByteAddress >= T214_SYSTEM_LIMIT)
        return STATUS_INVALID_ADDRESS;
    *DeviceAddress = ByteAddress;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuMapTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    UINT64 ByteOffset, SIZE_T Bytes, PVOID *CpuAddress, UINT64 *DeviceAddress)
{
    NTSTATUS status;
    if (!CpuAddress || !Segment || Bytes > TG_GPU_TABLE_BYTES)
        return STATUS_INVALID_PARAMETER;
    status = TgGpuResolveTableSegment((PVOID)Segment, Segment->SegmentId,
        ByteOffset, Bytes, DeviceAddress);
    if (!NT_SUCCESS(status)) return status;
    *CpuAddress = (UCHAR *)Segment->Memory.CpuAddress + (SIZE_T)ByteOffset;
    return STATUS_SUCCESS;
}
