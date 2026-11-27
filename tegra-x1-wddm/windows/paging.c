/* SPDX-License-Identifier: MIT */
#include "paging.h"

_Static_assert(sizeof(DXGK_PTE) == 16, "Windows PTE layout changed");
_Static_assert(sizeof(DXGK_UPDATEPAGETABLEFLAGS) == sizeof(ULONG), "Unexpected page table flags");

static NTSTATUS EncodePage(UINT Level, UINT8 Kind, const DXGK_PTE *Pte,
                          const TG_GPU_PAGE_UPDATE_TARGET *Target, UINT64 *Encoded)
{
    UINT64 address;
    NTSTATUS status;
    /* Invalid entries may carry stale addresses; do not resolve them. */
    if (!Pte->Valid) { *Encoded = 0; return STATUS_SUCCESS; }
    if (Pte->Zero || Pte->CacheCoherent || Pte->NoExecute || Pte->LargePage ||
        Pte->PhysicalAdapterIndex || Pte->Reserved ||
        (Level == 1 && (Pte->ReadOnly || Pte->PageTablePageSize != DXGK_PTE_PAGE_TABLE_PAGE_4KB)))
        return TgGpuRejectPaging(Target->Rejection, TgGpuPagingRejectPteFlags, Pte->Flags);
    /* VidMm supplies the upper address bits as a 4 KiB page number.
     * Both segment offsets and system addresses reach the resolver in bytes. */
    if (Pte->PageAddress > (UINT64_MAX >> PAGE_SHIFT))
        return STATUS_INVALID_PARAMETER;
    /* A level-1 entry points to a contiguous table in the dedicated table
     * segment. Segment zero cannot promise this for a 128 KiB allocation.
     */
    if (Level == 1 && !Pte->Segment)
        return TgGpuRejectPaging(Target->Rejection, TgGpuPagingRejectDirectorySegment, Pte->Flags);
    status = Target->Resolve(Target->ResolveContext, (UINT)Pte->Segment,
        Pte->PageAddress << PAGE_SHIFT,
        Level ? TG_GPU_TABLE_BYTES : PAGE_SIZE, &address);
    if (!NT_SUCCESS(status)) return status;
    if (address >= TG_GPU_VA_LIMIT ||
        (Level ? TG_GPU_TABLE_BYTES : PAGE_SIZE) > TG_GPU_VA_LIMIT - address)
        return STATUS_INVALID_PARAMETER;
    if (Level == 0) {
        if (!tg_gpu_pte_4k_kind(address, Pte->ReadOnly != 0, Target->GpuPagesUncached != FALSE,
                               Kind, Encoded))
            return STATUS_INVALID_PARAMETER;
    } else if (!tg_gpu_pde_small(address, Encoded)) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuEncodePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *Update,
                             const TG_GPU_PAGE_UPDATE_TARGET *Target)
{
    UINT i, count;
    SIZE_T entries;
    NTSTATUS status;
    if (!Update || !Target || !Target->Resolve ||
        !Target->Scratch || !Target->Size || Target->Size > TG_GPU_TABLE_BYTES ||
        (Target->Size & (PAGE_SIZE - 1)) || ((ULONG_PTR)Target->Scratch & 7) ||
        Update->PageTableLevel > 1 || Update->Reserved0)
        return STATUS_INVALID_PARAMETER;
    if (Update->Flags.Use64KBPages ||
        Update->Flags.Reserved || Update->pPageTableEntries64KB) {
        ULONG flags;
        RtlCopyMemory(&flags, &Update->Flags, sizeof(flags));
        UINT64 has_64kb_entries = Update->pPageTableEntries64KB != NULL;
        return TgGpuRejectPaging(Target->Rejection, TgGpuPagingRejectUpdateFlags,
                                 (has_64kb_entries << 32) | flags);
    }
    /* The PAL4 capture has a GPU_PHYSICAL level-1 update with a NULL
     * allocation and the known leaf kind 0xfe. Accept a supported kind on
     * this directory path, but EncodePage must still encode only the table
     * pointer there. Kind bits belong exclusively to leaf PTEs. Preserve
     * the existing pitch-only CPU initialization contract. */
    if (!tg_gpu_pte_kind_supported(Update->DriverProtection) ||
        (Update->DriverProtection &&
         Update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL))
        return TgGpuRejectPaging(Target->Rejection, TgGpuPagingRejectDriverProtection,
                                 Update->DriverProtection);
    if (!Update->PageTableLevel && Target->Size != TG_GPU_TABLE_BYTES)
        return STATUS_INVALID_PARAMETER;
    entries = Target->Size / sizeof(UINT64);
    count = Update->NumPageTableEntries;
    if (Update->StartIndex > entries || count > entries - Update->StartIndex ||
        count > Target->ScratchEntries || (count && !Update->pPageTableEntries))
        return STATUS_INVALID_PARAMETER;
    /* VidMm owns input entries and the caller pins both target and resolved
     * addresses for the full operation. Scratch avoids partial publication
     * if a later segment translation or attribute is rejected.
     */
    for (i = 0; i < count; ++i) {
        status = EncodePage(Update->PageTableLevel, (UINT8)Update->DriverProtection,
            &Update->pPageTableEntries[Update->Flags.Repeat ? 0 : i], Target, &Target->Scratch[i]);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuUpdatePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *Update,
                              const TG_GPU_PAGE_UPDATE_TARGET *Target)
{
    UINT i;
    NTSTATUS status;
    if (!Update || !Target || !Target->Entries || ((ULONG_PTR)Target->Entries & 7) ||
        Update->PageTableAddress.CpuVirtual != (PVOID)Target->Entries ||
        Update->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL)
        return STATUS_INVALID_PARAMETER;
    if (!Target->CpuMappingUncached)
        return TgGpuRejectPaging(Target->Rejection, TgGpuPagingRejectCpuAlias, 2);
    status = TgGpuEncodePageTable(Update, Target);
    if (!NT_SUCCESS(status)) return status;
    TgGpuDataSyncBarrier();
    for (i = 0; i < Update->NumPageTableEntries; ++i)
        Target->Entries[Update->StartIndex + i] = Target->Scratch[i];
    TgGpuDataSyncBarrier();
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuDescribePageTable(UINT Level, UINT TableSegment,
                               DXGK_PAGE_TABLE_LEVEL_DESC *Description)
{
    if (!Description || Level > 1 || !TableSegment || TableSegment > 31)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Description, sizeof(*Description));
    Description->PageTableIndexBitCount = TG_GPU_TABLE_BITS;
    Description->PageTableSegmentId = TableSegment;
    Description->PagingProcessPageTableSegmentId = TableSegment;
    Description->PageTableSizeInBytes = TG_GPU_TABLE_BYTES;
    Description->PageTableAlignmentInBytes = PAGE_SIZE;
    return STATUS_SUCCESS;
}
