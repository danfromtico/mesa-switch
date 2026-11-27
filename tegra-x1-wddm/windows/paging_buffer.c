/* SPDX-License-Identifier: MIT */
#include "paging_buffer.h"

enum { PAGING_UPDATE = 1, PAGING_FLUSH, PAGING_FILL, PAGING_COPY };
typedef struct _PAGING_RECORD {
    UINT Kind, Bytes;
    union {
        struct {
            D3DGPU_PHYSICAL_ADDRESS Table;
            UINT TableBytes, Start, Count, Reserved;
            UINT64 VaLimit, FirstVa, LastVa;
        };
        struct tg_gpu_paging_dma Dma;
    };
    /* Encoded UINT64 entries immediately follow an UPDATE record. */
} PAGING_RECORD;
_Static_assert(sizeof(PAGING_RECORD) == 64, "paging record alignment");

static NTSTATUS RecordRoot(const TG_GPU_TABLE_SEGMENT *Tables,
    const PAGING_RECORD *Record, TG_GPU_ROOT_TABLE *Root)
{
    DXGKARG_SETROOTPAGETABLE set = {0};
    NTSTATUS status;
    set.Address = Record->Table;
    set.NumEntries = Record->Count;
    status = TgGpuResolveRootTable(Tables, &set, Root);
    if (!NT_SUCCESS(status)) return status;
    if (Root->Bytes != Record->TableBytes || Root->VaLimit != Record->VaLimit ||
        ((Record->FirstVa || Record->LastVa) &&
         (Record->FirstVa >= Root->VaLimit || Record->LastVa < Record->FirstVa ||
          Record->LastVa > Root->VaLimit)))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS ValidateRecord(const TG_GPU_TABLE_SEGMENT *Tables,
    const PAGING_RECORD *Record, SIZE_T Available)
{
    PVOID cpu;
    UINT64 address;
    TG_GPU_ROOT_TABLE root;
    if (Available < sizeof(*Record) || Record->Bytes < sizeof(*Record) ||
        Record->Bytes > Available || (Record->Bytes & 7))
        return STATUS_INVALID_PARAMETER;
    if (Record->Kind == PAGING_FILL || Record->Kind == PAGING_COPY)
        return Record->Bytes == sizeof(*Record) &&
            Record->Dma.fill == (Record->Kind == PAGING_FILL) &&
            tg_gpu_paging_dma_valid(&Record->Dma) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    if (Record->Reserved || Record->Table.Padding || Record->Table.SegmentId != Tables->SegmentId)
        return STATUS_INVALID_PARAMETER;
    if (Record->Kind == PAGING_FLUSH) {
        if (Record->Bytes != sizeof(*Record) || Record->Start)
            return STATUS_INVALID_PARAMETER;
        return RecordRoot(Tables, Record, &root);
    }
    if (Record->Kind != PAGING_UPDATE || Record->VaLimit || Record->FirstVa ||
        Record->LastVa || !Record->Count ||
        Record->Start > Record->TableBytes / sizeof(UINT64) ||
        Record->Count > Record->TableBytes / sizeof(UINT64) - Record->Start ||
        Record->Bytes != sizeof(*Record) + (SIZE_T)Record->Count * sizeof(UINT64))
        return STATUS_INVALID_PARAMETER;
    return TgGpuMapTableSegment(Tables, Record->Table.SegmentOffset,
        Record->TableBytes, &cpu, &address);
}

static NTSTATUS InitialUpdate(DXGKARG_BUILDPAGINGBUFFER *Args,
    const TG_GPU_PAGING_BUILD *Build, TG_GPU_PAGE_UPDATE_TARGET *Target)
{
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update = Args->UpdatePageTable;
    ULONG_PTR alias = (ULONG_PTR)update.PageTableAddress.CpuVirtual;
    UINT64 address, first;
    SIZE_T offset;
    PVOID owned;
    NTSTATUS status;
    if (Args->MultipassOffset || !alias || (alias & (PAGE_SIZE - 1)) ||
        alias > (ULONG_PTR)-1 - Target->Size)
        return STATUS_INVALID_PARAMETER;
    if (!Build->InitialMappingUncached)
        return TgGpuRejectPaging(Target->Rejection, TgGpuPagingRejectCpuAlias, 1);
    /* VidMm supplies a valid nonpaged alias; verify every page, not just the
     * first address. Never assume its virtual address equals our own mapping.
     */
    first = (UINT64)MmGetPhysicalAddress((PVOID)alias).QuadPart;
    address = (UINT64)Build->Tables->Memory.SystemPhysicalAddress.QuadPart;
    if (first < address) return STATUS_INVALID_ADDRESS;
    status = TgGpuMapTableSegment(Build->Tables, first - address, Target->Size,
        &owned, &address);
    if (!NT_SUCCESS(status)) return status;
    for (offset = 0; offset < Target->Size; offset += PAGE_SIZE)
        if ((UINT64)MmGetPhysicalAddress((PVOID)(alias + offset)).QuadPart != first + offset)
            return STATUS_INVALID_ADDRESS;
    Target->Entries = owned;
    update.PageTableAddress.CpuVirtual = owned;
    return TgGpuUpdatePageTable(&update, Target);
}

NTSTATUS TgGpuBuildPagingBuffer(DXGKARG_BUILDPAGINGBUFFER *Args,
                               const TG_GPU_PAGING_BUILD *Build)
{
    PAGING_RECORD record = {0};
    TG_GPU_PAGE_UPDATE_TARGET target = {0};
    UINT count = 0, done = 0, i;
    NTSTATUS status;
    PVOID cpu;
    UINT64 address;
    if (!Args || !Build || !Build->Tables)
        return STATUS_INVALID_PARAMETER;
    if (Args->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE) {
        const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *update = &Args->UpdatePageTable;
        target.Size = Build->TableBytes;
        target.Scratch = Build->Scratch;
        target.ScratchEntries = Build->ScratchEntries;
        target.CpuMappingUncached = TRUE; /* owned segment alias */
        target.GpuPagesUncached = Build->GpuPagesUncached;
        target.Resolve = TgGpuResolveVidMmPage;
        target.ResolveContext = (PVOID)Build->Tables;
        target.Rejection = Build->Rejection;
        if (!target.Size || target.Size > TG_GPU_TABLE_BYTES ||
            (target.Size & (PAGE_SIZE - 1))) return STATUS_INVALID_PARAMETER;
        if (update->UpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL) {
            /* VidMm can supply a DMA pointer during immediate system paging
             * initialization. Update only the validated table alias. */
            return InitialUpdate(Args, Build, &target);
        }
        if (update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL)
            return TgGpuRejectPaging(target.Rejection, TgGpuPagingRejectUpdateMode,
                                     update->UpdateMode);
        record.Kind = PAGING_UPDATE;
        record.Table = update->PageTableAddress.GpuPhysical;
        if (record.Table.Padding || record.Table.SegmentId != Build->Tables->SegmentId)
            return STATUS_INVALID_ADDRESS;
        status = TgGpuMapTableSegment(Build->Tables, record.Table.SegmentOffset,
            target.Size, &cpu, &address);
        if (!NT_SUCCESS(status)) return status;
        /* Validate all entries before emitting even the first partial buffer.
         * Scratch and resolved residency stay stable across multipass calls.
         */
        status = TgGpuEncodePageTable(update, &target);
        if (!NT_SUCCESS(status)) return status;
        done = Args->MultipassOffset;
        if (done > update->NumPageTableEntries) return STATUS_INVALID_PARAMETER;
        count = update->NumPageTableEntries - done;
        if (!count) { Args->MultipassOffset = 0; return STATUS_SUCCESS; }
        if (!Args->pDmaBuffer || Args->DmaSize < sizeof(record) + sizeof(UINT64))
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        if (count > (Args->DmaSize - sizeof(record)) / sizeof(UINT64))
            count = (Args->DmaSize - sizeof(record)) / sizeof(UINT64);
        record.TableBytes = (UINT)target.Size;
        record.Start = update->StartIndex + done;
        record.Count = count;
        record.Bytes = (UINT)sizeof(record) + count * (UINT)sizeof(UINT64);
    } else if (Args->Operation == DXGK_OPERATION_FLUSH_TLB) {
        const DXGK_BUILDPAGINGBUFFER_FLUSHTLB *flush = &Args->FlushTlb;
        if (!Build->Root || !Build->Root->Valid || Args->MultipassOffset ||
            flush->RootPageTableAddress.SegmentId != Build->Root->Address.SegmentId ||
            flush->RootPageTableAddress.SegmentOffset != Build->Root->Address.SegmentOffset)
            return STATUS_INVALID_PARAMETER;
        record.Kind = PAGING_FLUSH; record.Bytes = sizeof(record);
        record.Table = flush->RootPageTableAddress;
        record.TableBytes = (UINT)Build->Root->Bytes;
        record.Count = Build->Root->Entries;
        record.VaLimit = Build->Root->VaLimit;
        record.FirstVa = flush->StartVirtualAddress;
        record.LastVa = flush->EndVirtualAddress;
        status = ValidateRecord(Build->Tables, &record, sizeof(record));
        if (!NT_SUCCESS(status)) return status;
        if (!Args->pDmaBuffer || Args->DmaSize < sizeof(record))
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    } else if (Args->Operation == DXGK_OPERATION_VIRTUAL_FILL ||
               Args->Operation == DXGK_OPERATION_VIRTUAL_TRANSFER) {
        if (Args->MultipassOffset)
            return STATUS_INVALID_PARAMETER;
        if (Args->Operation == DXGK_OPERATION_VIRTUAL_FILL) {
            record.Kind = PAGING_FILL;
            record.Dma = (struct tg_gpu_paging_dma){
                .destination = Args->FillVirtual.DestinationVirtualAddress,
                .bytes = Args->FillVirtual.FillSizeInBytes,
                .pattern = Args->FillVirtual.FillPattern,
                .fill = true,
            };
        } else {
            const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL *transfer = &Args->TransferVirtual;
            if (transfer->Flags.Flags || (UINT)transfer->TransferDirection > DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL)
                return TgGpuRejectPaging(Build->Rejection, TgGpuPagingRejectVirtualTransfer,
                                         ((UINT64)(UINT)transfer->TransferDirection << 32) |
                                         transfer->Flags.Flags);
            record.Kind = PAGING_COPY;
            record.Dma = (struct tg_gpu_paging_dma){
                .source = transfer->SourceVirtualAddress,
                .destination = transfer->DestinationVirtualAddress,
                .bytes = transfer->TransferSizeInBytes,
            };
        }
        record.Bytes = sizeof(record);
        if (!tg_gpu_paging_dma_valid(&record.Dma))
            return STATUS_INVALID_PARAMETER;
        if (!Args->pDmaBuffer || Args->DmaSize < sizeof(record))
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    } else return TgGpuRejectPaging(Build->Rejection, TgGpuPagingRejectBufferOperation,
                                   Args->MultipassOffset);
    if ((ULONG_PTR)Args->pDmaBuffer & 7) return STATUS_INVALID_PARAMETER;
    *(PAGING_RECORD *)Args->pDmaBuffer = record;
    for (i = 0; i < count; ++i)
        ((UINT64 *)((UCHAR *)Args->pDmaBuffer + sizeof(record)))[i] = Build->Scratch[done+i];
    Args->pDmaBuffer = (UCHAR *)Args->pDmaBuffer + record.Bytes;
    if (record.Kind == PAGING_UPDATE && done + count < Args->UpdatePageTable.NumPageTableEntries) {
        Args->MultipassOffset = done + count;
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    Args->MultipassOffset = 0;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuValidatePagingBuffer(const TG_GPU_TABLE_SEGMENT *Tables,const void *Buffer,SIZE_T Bytes,
                                  BOOLEAN *RequiresSystemContext)
{
    BOOLEAN requires = FALSE;
    if (!Tables || (Bytes && !Buffer) || ((ULONG_PTR)Buffer & 7) || (Bytes & 7))
        return STATUS_INVALID_PARAMETER;
    for (SIZE_T offset = 0; offset < Bytes; ) {
        const PAGING_RECORD *record = (const void *)((const UCHAR *)Buffer + offset);
        NTSTATUS status = ValidateRecord(Tables, record, Bytes - offset);
        if (!NT_SUCCESS(status)) return status;
        if (record->Kind == PAGING_FILL || record->Kind == PAGING_COPY)
            requires = TRUE;
        offset += record->Bytes;
    }
    if (RequiresSystemContext) *RequiresSystemContext = requires;
    return STATUS_SUCCESS;
}
NTSTATUS TgGpuExecutePagingBufferChecked(const TG_GPU_TABLE_SEGMENT *Tables,
    const void *Buffer, SIZE_T Bytes, BOOLEAN AddressSpaceIdle,
    const struct tg_gpu_power_io *Io, const struct tg_gpu_power *Power,
    SIZE_T *CompletedBytes, TG_GPU_PAGING_DMA_STEP *NextDma,
    const struct tg_gpu_commit_guard *Guard,
    volatile LONG *DiagLastKind)
{
    SIZE_T offset;NTSTATUS status;BOOLEAN requires;
    if (!CompletedBytes) return STATUS_INVALID_PARAMETER;
    *CompletedBytes = 0;
    if (NextDma) NextDma->RecordBytes = 0;
    /* Reject a malformed trailing record before publishing any earlier one. */
    status=TgGpuValidatePagingBuffer(Tables,Buffer,Bytes,&requires);
    if(!NT_SUCCESS(status) || !Bytes)return status;
    if (!AddressSpaceIdle) return STATUS_GRAPHICS_ALLOCATION_BUSY;
    if (requires && !NextDma) return STATUS_NOT_SUPPORTED;
    if (!Io || !Io->read32 || !Io->write32 || !Io->delay_us ||
        !Power || !Power->owned || !Power->ready) return STATUS_DEVICE_NOT_READY;
    for (offset = 0; offset < Bytes; ) {
        const PAGING_RECORD *record = (const void *)((const UCHAR *)Buffer + offset);
        /* Per-record fault/epoch gate (B11) and per-record kind breadcrumb (N3). */
        if (Guard && Guard->abort && Guard->abort(Guard->context))
            return STATUS_DEVICE_DATA_ERROR;
        if (DiagLastKind)
            InterlockedExchange(DiagLastKind, (LONG)record->Kind);
        if (record->Kind == PAGING_UPDATE) {
            PVOID cpu;
            UINT64 address;
            UINT i;
            const UINT64 *values = (const void *)(record + 1);
            status = TgGpuMapTableSegment(Tables, record->Table.SegmentOffset,
                record->TableBytes, &cpu, &address);
            if (!NT_SUCCESS(status)) return status;
            TgGpuDataSyncBarrier();
            for (i = 0; i < record->Count; ++i)
                ((volatile UINT64 *)cpu)[record->Start+i] = values[i];
            TgGpuDataSyncBarrier();
        } else if (record->Kind == PAGING_FLUSH) {
            TG_GPU_ROOT_TABLE root;
            DXGK_BUILDPAGINGBUFFER_FLUSHTLB flush = {0};
            status = RecordRoot(Tables, record, &root);
            if (!NT_SUCCESS(status)) return status;
            flush.RootPageTableAddress = record->Table;
            flush.StartVirtualAddress = record->FirstVa;
            flush.EndVirtualAddress = record->LastVa;
            status = TgGpuFlushRootTableChecked(&root, &flush, Io, Power, Guard);
            if (!NT_SUCCESS(status)) return status;
            if (Guard && Guard->abort && Guard->abort(Guard->context))
                return STATUS_DEVICE_DATA_ERROR;
        } else {
            NextDma->Operation = record->Dma;
            NextDma->RecordBytes = record->Bytes;
            return STATUS_SUCCESS;
        }
        offset += record->Bytes;
        *CompletedBytes = offset;
    }
    return STATUS_SUCCESS;
}
NTSTATUS TgGpuExecutePagingBuffer(const TG_GPU_TABLE_SEGMENT *Tables,
    const void *Buffer, SIZE_T Bytes, BOOLEAN AddressSpaceIdle,
    const struct tg_gpu_power_io *Io, const struct tg_gpu_power *Power,
    SIZE_T *CompletedBytes, TG_GPU_PAGING_DMA_STEP *NextDma)
{
    return TgGpuExecutePagingBufferChecked(Tables, Buffer, Bytes, AddressSpaceIdle,
        Io, Power, CompletedBytes, NextDma, NULL, NULL);
}
