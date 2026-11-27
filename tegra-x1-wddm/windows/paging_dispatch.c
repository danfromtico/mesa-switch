/* SPDX-License-Identifier: MIT */
#include "paging_dispatch.h"
#include "allocation.h"
#include "work_queue.h"
#define TAG 0x50674754u

static TG_GPU_PROCESS *process(TG_GPU_CONTEXT_ENGINE *e,HANDLE h)
{
    for(LIST_ENTRY *l=e->Processes.Flink;l!=&e->Processes;l=l->Flink) {
        TG_GPU_PROCESS *p=CONTAINING_RECORD(l,TG_GPU_PROCESS,Link);
        if(p==h && !p->Closing)return p;
    }
    return NULL;
}
static BOOLEAN system_context(TG_GPU_CONTEXT_ENGINE *e,HANDLE h)
{
    if(!h)return TRUE; /* OS paging submission may have no context handle. */
    for(LIST_ENTRY *l=e->Contexts.Flink;l!=&e->Contexts;l=l->Flink) {
        TG_GPU_CONTEXT *c=CONTAINING_RECORD(l,TG_GPU_CONTEXT,Link);
        if(c==h)return !c->Closing && c->Flags.SystemContext;
    }
    return FALSE;
}
NTSTATUS TgGpuBuildPagingRequest(HANDLE adapter, DXGKARG_BUILDPAGINGBUFFER *a,
                                TG_GPU_PAGING_REJECTION *rejection)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_PROCESS *p;TG_GPU_ROOT_TABLE root={0};
    TG_GPU_PAGING_BUILD build={0};TG_GPU_QUEUE_ENTRY *entry;KIRQL irql;
    HANDLE owner=NULL;NTSTATUS status;UINT count=0,available,offset;PVOID before;
    UINT64 *scratch=NULL;BOOLEAN immediate=FALSE,virtual_dma=FALSE;
    PAGED_CODE();
    if(!e || !a || !e->Tables)return STATUS_INVALID_PARAMETER;
    if (a->Operation == DXGK_OPERATION_DISCARD_CONTENT) {
        /* Uncompressed allocations have no content metadata to invalidate.
         * VidMm owns residency and the contents become undefined. */
        return TgGpuValidatePagingAllocation(e, a->DiscardContent.hAllocation, 0, 0);
    }
    if(a->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
       a->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        /* The aperture is an offset space resolved into GM20B PTEs. Its
         * CPU-side translation does not consume a paging DMA record. */
        KeAcquireSpinLock(&e->Lock,&irql);
        status=e->Stopping?STATUS_DEVICE_NOT_READY:TgGpuUpdatePagingAperture(e->Tables,a);
        KeReleaseSpinLock(&e->Lock,irql);
        if (status == STATUS_NOT_SUPPORTED && a->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT)
            return TgGpuRejectPaging(rejection, TgGpuPagingRejectApertureFlags,
                                     a->MapApertureSegment.Flags.Value);
        return status;
    }
    if(a->Operation==DXGK_OPERATION_UPDATE_PAGE_TABLE) {
        const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *u=&a->UpdatePageTable;
        if(u->PageTableLevel>1 || u->NumPageTableEntries>TG_GPU_TABLE_ENTRIES)return STATUS_INVALID_PARAMETER;
        owner=u->hProcess;count=u->NumPageTableEntries;
        immediate=u->UpdateMode==DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    } else if(a->Operation==DXGK_OPERATION_FLUSH_TLB)owner=a->FlushTlb.hProcess;
    else if (a->Operation==DXGK_OPERATION_VIRTUAL_FILL || a->Operation==DXGK_OPERATION_VIRTUAL_TRANSFER) {
        if (!a->hSystemContext) return STATUS_INVALID_PARAMETER;
        virtual_dma=TRUE;
    } else if (a->Operation==DXGK_OPERATION_TRANSFER && !a->Transfer.TransferSize && !a->MultipassOffset) {
        /* Global TDR eviction can have no contents to move. This path is
         * called while the GPU is isolated and must perform no MMIO. */
        return a->Transfer.hAllocation ? TgGpuValidatePagingAllocation(e,a->Transfer.hAllocation,
            a->Transfer.TransferOffset,0) : STATUS_SUCCESS;
    } else return TgGpuRejectPaging(rejection, TgGpuPagingRejectOperation,
                                   a->MultipassOffset);
    KeAcquireSpinLock(&e->Lock,&irql);p=virtual_dma?NULL:process(e,owner);
    /* VidMm selects CPU_VIRTUAL while setting up the system address space,
     * both for clearing tables and for populating its initial mappings. */
    if(e->Stopping || (!virtual_dma && !p) || !system_context(e,a->hSystemContext) ||
       (immediate && (!p->Flags.SystemProcess || e->Active || e->Notification ||
                       e->Channel.referenced))) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    if (p) ++p->PagingReferences;
    ++e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    build.Tables=e->Tables;build.TableBytes=TG_GPU_TABLE_BYTES;
    build.GpuPagesUncached=build.InitialMappingUncached=TRUE;
    build.Rejection=rejection;
    if(a->Operation==DXGK_OPERATION_UPDATE_PAGE_TABLE) {
        const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *u=&a->UpdatePageTable;
        if(u->hAllocation) {
            status = u->PageTableLevel
                ? TgGpuRejectPaging(rejection, TgGpuPagingRejectDirectoryAllocation,
                                   u->PageTableLevel)
                : TgGpuValidatePagingAllocation(e, u->hAllocation,
                    u->AllocationOffsetInBytes, (UINT64)count * PAGE_SIZE);
            if(!NT_SUCCESS(status))goto done;
        }
        scratch=ExAllocatePool2(POOL_FLAG_NON_PAGED,(SIZE_T)(count?count:1)*sizeof(*scratch),TAG);
        if(!scratch){status=STATUS_NO_MEMORY;goto done;}
        build.Scratch=scratch;build.ScratchEntries=count;
    } else if (!virtual_dma) {
        /* This DDI reports a full 128 KiB root and its full entry capacity in
         * GetRootPageTableSize. No root pointer from an older context is used
         * while VidMm may be moving that context's root allocation. */
        DXGKARG_SETROOTPAGETABLE set={.Address=a->FlushTlb.RootPageTableAddress,.NumEntries=TG_GPU_TABLE_ENTRIES};
        status=TgGpuResolveRootTable(e->Tables,&set,&root);
        if(!NT_SUCCESS(status))goto done;
        build.Root=&root;
    } else {
        BOOLEAN fill=a->Operation==DXGK_OPERATION_VIRTUAL_FILL;
        HANDLE allocation=fill?a->FillVirtual.hAllocation:a->TransferVirtual.hAllocation;
        if (allocation) {
            status=TgGpuValidatePagingAllocation(e,allocation,
                fill?a->FillVirtual.AllocationOffsetInBytes:a->TransferVirtual.AllocationOffsetInBytes,
                fill?a->FillVirtual.FillSizeInBytes:a->TransferVirtual.TransferSizeInBytes);
            if (!NT_SUCCESS(status)) goto done;
        }
    }
    if(immediate) {status=TgGpuBuildPagingBuffer(a,&build);goto done;}
    offset=a->DmaBufferWriteOffset;available=a->DmaSize;before=a->pDmaBuffer;
    entry=a->pDmaBufferPrivateData;
    if(!before || ((ULONG_PTR)before&7) ||
       (offset&7) || offset>TG_GPU_DMA_BUFFER_BYTES || available>TG_GPU_DMA_BUFFER_BYTES-offset ||
       (ULONG_PTR)before<offset || a->DmaBufferGpuVirtualAddress>TG_GPU_VA_LIMIT-TG_GPU_DMA_BUFFER_BYTES) {
        status=STATUS_INVALID_PARAMETER;goto done;
    }
    /* VidMm can submit several packets from the same DMA buffer. Reserve a
     * private entry for each build that adds commands to a packet. */
    if(a->DmaBufferPrivateDataSize<sizeof(*entry)) {
        status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto done;
    }
    if(!entry || ((ULONG_PTR)entry&(TYPE_ALIGNMENT(TG_GPU_QUEUE_ENTRY)-1)) ||
       a->DmaBufferPrivateDataSize>TG_GPU_PAGING_PRIVATE_BYTES ||
       a->DmaBufferPrivateDataSize%sizeof(*entry)) {
        status=STATUS_INVALID_PARAMETER;goto done;
    }
    status=TgGpuBuildPagingBuffer(a,&build);
    if(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) {
        UINT written=(UINT)((UCHAR *)a->pDmaBuffer-(UCHAR *)before);
        if(written) {
            RtlZeroMemory(entry,sizeof(*entry));
            entry->Paging=(TG_GPU_PAGING_BUFFER){.Engine=e,.CpuAddress=(UCHAR *)before-offset,
                .GpuAddress=a->DmaBufferGpuVirtualAddress,.WriteOffset=offset,.BuiltBytes=offset+written,
                .Capacity=offset+available,.Magic=TG_GPU_PAGING_BUFFER_MAGIC,
                .SystemContext=virtual_dma?a->hSystemContext:NULL};
            a->pDmaBufferPrivateData=entry+1;
            a->DmaBufferPrivateDataSize-=sizeof(*entry);
            a->DmaSize=available-written;
        }
    }
done:
    if(scratch)ExFreePoolWithTag(scratch,TAG);
    KeAcquireSpinLock(&e->Lock,&irql);if (p) --p->PagingReferences;--e->Creating;
    KeReleaseSpinLock(&e->Lock,irql);return status;
}

NTSTATUS APIENTRY TgGpuDdiBuildPagingBuffer(HANDLE adapter, DXGKARG_BUILDPAGINGBUFFER *args)
{
    return TgGpuBuildPagingRequest(adapter, args, NULL);
}

void TgGpuRegisterPagingDdi(DRIVER_INITIALIZATION_DATA *d)
{d->DxgkDdiBuildPagingBuffer=TgGpuDdiBuildPagingBuffer;}
