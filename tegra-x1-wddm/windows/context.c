/* SPDX-License-Identifier: MIT */
#include "context.h"
#include "work_queue.h"
#include "allocation.h"
#include "query.h"
#include "paging_dispatch.h"
#include "../core/gpfifo.h"

#define TAG 0x43674754u

static PVOID object(SIZE_T bytes)
{ return ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, TAG); }
static TG_GPU_PROCESS *find_process(TG_GPU_CONTEXT_ENGINE *e, HANDLE h)
{
    LIST_ENTRY *l;
    for (l=e->Processes.Flink;l!=&e->Processes;l=l->Flink) {
        TG_GPU_PROCESS *p=CONTAINING_RECORD(l,TG_GPU_PROCESS,Link);
        if (p==h) return p;
    }
    return NULL;
}
static TG_GPU_CONTEXT *find_context(TG_GPU_CONTEXT_ENGINE *e, HANDLE h)
{
    LIST_ENTRY *l;
    for (l=e->Contexts.Flink;l!=&e->Contexts;l=l->Flink) {
        TG_GPU_CONTEXT *c=CONTAINING_RECORD(l,TG_GPU_CONTEXT,Link);
        if (c==h) return c;
    }
    return NULL;
}
BOOLEAN TgGpuIsGdiContext(TG_GPU_CONTEXT_ENGINE *e,HANDLE handle)
{
    KIRQL irql;
    if(!e)return FALSE;
    KeAcquireSpinLock(&e->Lock,&irql);
    const TG_GPU_CONTEXT *c=find_context(e,handle);
    const BOOLEAN gdi=c && !c->Closing && c->Flags.GdiContext && c->Flags.VirtualAddressing;
    KeReleaseSpinLock(&e->Lock,irql);
    return gdi;
}

NTSTATUS TgGpuInitializeContextEngine(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKRNL_INTERFACE *dxgk, TG_GPU_TABLE_SEGMENT *tables)
{
    if (!e || !dxgk || !tables || !tables->Memory.Handle || !tables->SegmentId ||
        dxgk->Version<DXGKDDI_INTERFACE_VERSION_WDDM2_4 ||
        !dxgk->DxgkCbAllocateContiguousMemory || !dxgk->DxgkCbFreeContiguousMemory ||
        !dxgk->DxgkCbSynchronizeExecution || !dxgk->DxgkCbNotifyInterrupt ||
        !dxgk->DxgkCbQueueDpc || !dxgk->DxgkCbNotifyDpc ||
        !dxgk->DxgkCbReserveGpuVirtualAddressRange)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(e,sizeof(*e));
    e->Dxgk=*dxgk; e->Tables=tables;
    KeInitializeSpinLock(&e->Lock);
    InitializeListHead(&e->Processes); InitializeListHead(&e->Devices); InitializeListHead(&e->Contexts);
    return TgGpuAllocatePrivateMemory(dxgk,TG_GPU_COMMAND_BYTES,0x80000000,0xffffffff,&e->Commands);
}
NTSTATUS TgGpuInitializeGraphicsTemplate(TG_GPU_CONTEXT_ENGINE *e,
    const struct tg_gpu_gr_work *work,const struct tg_gpu_gr_context_status *golden)
{
    TG_GPU_GRAPHICS_TEMPLATE next={0};NTSTATUS status;KIRQL irql;
    PAGED_CODE();
    if(!e || !e->Commands.Handle)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->Graphics.Memory.Handle || e->Creating || e->Active || e->Notification ||
       e->Channel.referenced) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    ++e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuCreateGraphicsTemplate(&e->Dxgk,work,golden,&next);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;
    if(NT_SUCCESS(status)) {
        e->Graphics=next;if(!e->GraphicsGeneration)e->GraphicsGeneration=1;
        /* Stop may have closed admission while allocation slept. Retain the
         * successful allocation in the engine so its stop retry can free it.
         */
        if(e->Stopping)status=STATUS_DEVICE_NOT_READY;
    }
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
NTSTATUS TgGpuStopContextEngine(TG_GPU_CONTEXT_ENGINE *e)
{
    KIRQL irql; NTSTATUS status;
    if (!e) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    e->Stopping=TRUE;
    status=e->Active || e->Notification || e->QueueOwner || e->HardwareOwner || e->RecoverySending || e->Channel.referenced || e->Creating || e->AllocationCount || e->ResourceCount || !IsListEmpty(&e->Contexts) || !IsListEmpty(&e->Devices) ||
        !IsListEmpty(&e->Processes) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    KeReleaseSpinLock(&e->Lock,irql);
    if (!NT_SUCCESS(status)) return status;
    status=TgGpuFreePrivateMemory(&e->Dxgk,&e->Commands);
    if(!NT_SUCCESS(status))return status;
    return TgGpuFreePrivateMemory(&e->Dxgk,&e->Graphics.Memory);
}
NTSTATUS APIENTRY TgGpuDdiCreateProcess(HANDLE adapter, DXGKARG_CREATEPROCESS *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_PROCESS *p; KIRQL irql; NTSTATUS status;
    if (!e || !a) return STATUS_INVALID_PARAMETER;
    p=object(sizeof(*p)); if (!p) return STATUS_NO_MEMORY;
    p->Engine=e; p->Runtime=a->hDxgkProcess; p->Flags=a->Flags;
    KeAcquireSpinLock(&e->Lock,&irql);
    if (e->Stopping) { KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(p,TAG); return STATUS_DEVICE_NOT_READY; }
    ++e->Creating; KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuCreateInternalVa(&e->Dxgk,p->Runtime,&e->Commands,&p->Internal);
    KeAcquireSpinLock(&e->Lock,&irql); --e->Creating;
    if (!NT_SUCCESS(status)) {
        KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(p,TAG); return status;
    }
    InsertTailList(&e->Processes,&p->Link);
    KeReleaseSpinLock(&e->Lock,irql);
    a->hKmdProcess=p;
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiDestroyProcess(HANDLE adapter, HANDLE process)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_PROCESS *p; KIRQL irql; NTSTATUS status;
    if (!e) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql); p=find_process(e,process);
    if (!p || p->Devices || p->PagingReferences || p->Closing) { KeReleaseSpinLock(&e->Lock,irql); return p ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER; }
    p->Closing=TRUE; KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuFreePrivateMemory(&e->Dxgk,&p->Internal.Leaf);
    KeAcquireSpinLock(&e->Lock,&irql);
    if (!NT_SUCCESS(status)) { p->Closing=FALSE; KeReleaseSpinLock(&e->Lock,irql); return status; }
    RemoveEntryList(&p->Link); KeReleaseSpinLock(&e->Lock,irql);
    ExFreePoolWithTag(p,TAG); return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiCreateDevice(HANDLE adapter, DXGKARG_CREATEDEVICE *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_DEVICE *d; TG_GPU_PROCESS *p; KIRQL irql;
    if (!e || !a) return STATUS_INVALID_PARAMETER;
    d=object(sizeof(*d)); if (!d) return STATUS_NO_MEMORY;
    KeAcquireSpinLock(&e->Lock,&irql); p=find_process(e,a->hKmdProcess);
    if (e->Stopping || (p && p->Closing) || (!p && (!a->Flags.SystemDevice || a->hKmdProcess))) {
        KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(d,TAG); return STATUS_INVALID_PARAMETER;
    }
    d->Engine=e; d->Process=p; d->Runtime=a->hDevice;
    if (p) ++p->Devices;
    InsertTailList(&e->Devices,&d->Link); KeReleaseSpinLock(&e->Lock,irql);
    a->hDevice=d; return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiCreateContext(HANDLE device, DXGKARG_CREATECONTEXT *a)
{
    TG_GPU_DEVICE *d=device; TG_GPU_CONTEXT_ENGINE *e; TG_GPU_CONTEXT *c;
    KIRQL irql; NTSTATUS status;
    if (!d || !a) return STATUS_INVALID_PARAMETER;
    /* One node/physical engine; no protected or hardware-queue capability is
     * advertised. A software context does not consume a hardware FIFO slot.
     */
    if (a->NodeOrdinal || (a->EngineAffinity & ~1u) || a->Flags.SystemProtectedContext ||
        a->Flags.HwQueueSupported || a->PrivateDriverDataSize)
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    e=d->Engine; c=object(sizeof(*c)); if (!c) return STATUS_NO_MEMORY;
    c->Device=d; c->Runtime=a->hContext; c->Flags=a->Flags; c->RootStatus=STATUS_DEVICE_NOT_READY;
    KeAcquireSpinLock(&e->Lock,&irql);
    if (e->Stopping || d->Closing) { KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(c,TAG); return STATUS_DEVICE_NOT_READY; }
    ++d->Contexts;++e->Creating; /* reserve the parent while allocation may sleep */
    if(d->Process) {
        if(!++e->ContextSerial)++e->ContextSerial;
        c->ContextId=e->ContextSerial;
    }
    KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuAllocatePrivateMemory(&e->Dxgk,d->Process?TG_GPU_CONTEXT_PRIVATE_BYTES:PAGE_SIZE,
                                     0x80000000,0xffffffff,&c->Instance);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;
    if (!NT_SUCCESS(status)) {
        --d->Contexts; KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(c,TAG); return status;
    }
    InsertTailList(&e->Contexts,&c->Link); KeReleaseSpinLock(&e->Lock,irql);
    RtlZeroMemory(&a->ContextInfo,sizeof(a->ContextInfo));
    a->ContextInfo.DmaBufferSize=TG_GPU_DMA_BUFFER_BYTES;
    /* The system paging context must match QUERYSEGMENT4's private data size.
     * Application contexts also reserve room for UMD submission data. */
    if (a->Flags.SystemContext) {
        a->ContextInfo.DmaBufferPrivateDataSize=TG_GPU_PAGING_PRIVATE_BYTES;
        a->ContextInfo.DmaBufferSegmentSet=1u<<(TG_GPU_PAGING_APERTURE_SEGMENT-1);
    } else if (a->Flags.GdiContext) {
        /* VidMm creates a privileged DMA pool for the virtual GDI context.
         * Its buffers need allocation objects for GPU VA mapping; segment
         * set zero takes the physical contiguous-buffer path instead. */
        a->ContextInfo.DmaBufferSegmentSet=1u<<(TG_GPU_PAGING_APERTURE_SEGMENT-1);
        /* GDI reserves only the kernel queue slot. */
        a->ContextInfo.DmaBufferPrivateDataSize=sizeof(TG_GPU_QUEUE_ENTRY);
    } else
        a->ContextInfo.DmaBufferPrivateDataSize=sizeof(TG_GPU_QUEUE_ENTRY)+
            sizeof(struct tg_gpu_submit_request)+sizeof(struct tg_gpu_submit_indirect)+
            TG_GPU_SUBMIT_MAX_BYTES;
    /* WDDM requires a 256-entry allocation list for GDI contexts. */
    if (a->Flags.GdiContext) a->ContextInfo.AllocationListSize=256;
    a->ContextInfo.Caps.NoPatchingRequired=a->Flags.VirtualAddressing;
    /* VidMm manages residency. No system-IOMMU capability or GDI acceleration
     * is implied by creating the object. Submission/paging dispatch is separate.
     */
    a->hContext=c; return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiDestroyContext(HANDLE context)
{
    TG_GPU_CONTEXT *c=context; TG_GPU_CONTEXT_ENGINE *e; KIRQL irql; NTSTATUS status;
    if (!c) return STATUS_INVALID_PARAMETER;
    e=c->Device->Engine; KeAcquireSpinLock(&e->Lock,&irql);
    if (c->References || c->Closing) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    c->Closing=TRUE;
    if(c->Device->Process)TgGpuUnmapContextGraphics(&c->Device->Process->Internal,&c->Instance);
    KeReleaseSpinLock(&e->Lock,irql);
    status=TgGpuFreePrivateMemory(&e->Dxgk,&c->Instance);
    KeAcquireSpinLock(&e->Lock,&irql);
    if (!NT_SUCCESS(status)) {
        c->Closing=FALSE; KeReleaseSpinLock(&e->Lock,irql); return status;
    }
    RemoveEntryList(&c->Link); --c->Device->Contexts;
    KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(c,TAG); return STATUS_SUCCESS;
}
SIZE_T APIENTRY TgGpuDdiGetRootPageTableSize(HANDLE adapter, DXGKARG_GETROOTPAGETABLESIZE *a)
{
    if(!adapter || !a || a->PhysicalAdapterIndex || a->NumberOfPte>TG_GPU_TABLE_ENTRIES)return 0;
    /* A complete native GM20B root is 128 KiB. Report its actual capacity to
     * VidMm, including at first allocation, so later paging records never
     * presume a 128 KiB alias for a smaller OS root allocation. The generic
     * root helper still supports resized roots for other explicit callers. */
    a->NumberOfPte=TG_GPU_TABLE_ENTRIES;return TG_GPU_TABLE_BYTES;
}
VOID APIENTRY TgGpuDdiSetRootPageTable(HANDLE adapter, const DXGKARG_SETROOTPAGETABLE *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter; TG_GPU_CONTEXT *c; KIRQL irql;
    if (!e || !a) return;
    KeAcquireSpinLock(&e->Lock,&irql); c=find_context(e,a->hContext);
    if (c) {
        if (c->References || c->Closing) c->RootStatus=STATUS_GRAPHICS_ALLOCATION_BUSY;
        else {
            c->RootStatus=TgGpuResolveRootTable(e->Tables,a,&c->Root);
            if (NT_SUCCESS(c->RootStatus)) c->RootStatus=c->Device->Process ?
                TgGpuInstallInternalRoot(e->Tables,&c->Root,&c->Device->Process->Internal) : STATUS_INVALID_DEVICE_STATE;
            if (NT_SUCCESS(c->RootStatus)) c->RootStatus=TgGpuWriteRootInstance(&c->Root,&c->Instance,TRUE);
        }
        /* This DDI has no return status. Never schedule a stale root after a
         * failed change; existing work retains its own validated snapshot.
         */
        if (!NT_SUCCESS(c->RootStatus)) c->Root.Valid=FALSE;
    }
    KeReleaseSpinLock(&e->Lock,irql);
}
void TgGpuRegisterContextDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiCreateProcess=TgGpuDdiCreateProcess;
    d->DxgkDdiDestroyProcess=TgGpuDdiDestroyProcess;
    d->DxgkDdiCreateDevice=TgGpuDdiCreateDevice;
    d->DxgkDdiDestroyDevice=TgGpuDdiDestroyDevice;
    d->DxgkDdiCreateContext=TgGpuDdiCreateContext;
    d->DxgkDdiDestroyContext=TgGpuDdiDestroyContext;
    d->DxgkDdiGetRootPageTableSize=TgGpuDdiGetRootPageTableSize;
    d->DxgkDdiSetRootPageTable=TgGpuDdiSetRootPageTable;
    d->DxgkDdiQueryCurrentFence=TgGpuDdiQueryCurrentFence;
    d->DxgkDdiDpcRoutine=TgGpuDdiDpcRoutine;
    TgGpuRegisterAllocationDdis(d);
    TgGpuRegisterQueryDdi(d);
    TgGpuRegisterPagingDdi(d);
}
NTSTATUS APIENTRY TgGpuDdiQueryCurrentFence(HANDLE adapter, DXGKARG_QUERYCURRENTFENCE *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;
    if (!e || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    KeMemoryBarrier(); a->CurrentFence=e->LastCompletedFence; return STATUS_SUCCESS;
}
VOID TgGpuDdiDpcRoutine(PVOID adapter)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;
    e->Dxgk.DxgkCbNotifyDpc(e->Dxgk.DeviceHandle);
}

static NTSTATUS command_context(TG_GPU_CONTEXT_ENGINE *e,HANDLE handle,TG_GPU_CONTEXT **out)
{
    TG_GPU_CONTEXT *c=find_context(e,handle);
    if (!c || c->Closing || !c->Device->Process || !c->Flags.VirtualAddressing)
        return STATUS_INVALID_PARAMETER;
    if (!c->Root.Valid || !NT_SUCCESS(c->RootStatus))
        return STATUS_DEVICE_NOT_READY;
    *out=c;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuGetCommandVa(TG_GPU_CONTEXT_ENGINE *e,HANDLE handle,struct tg_gpu_command_va *va)
{
    TG_GPU_CONTEXT *c;KIRQL irql;NTSTATUS status;
    if (!e || !va) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    status=(e->Stopping || e->Resetting || e->RecoveryPending)?STATUS_DEVICE_NOT_READY:
        command_context(e,handle,&c);
    if (status==STATUS_SUCCESS) {
        const UINT64 reserved=c->Device->Process->Internal.Base;
        *va=(struct tg_gpu_command_va){c->Root.VaLimit,reserved,reserved+TG_GPU_INTERNAL_COVERAGE};
    }
    KeReleaseSpinLock(&e->Lock,irql);
    return status;
}

NTSTATUS TgGpuCaptureContextWork(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *a, const struct tg_gpu_command_info *info,
    TG_GPU_CONTEXT_WORK *w)
{
    TG_GPU_CONTEXT *c;KIRQL irql;NTSTATUS status;struct tg_gpu_gpfifo_entry packet;
    if (!e || !a || !w || w->Context || a->NodeOrdinal || a->EngineOrdinal ||
        !a->DmaBufferSize || (a->DmaBufferSize & 3) ||
        !tg_gpu_build_gpfifo(a->DmaBufferVirtualAddress,a->DmaBufferSize/4,false,&packet))
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if (e->Stopping || e->Resetting || e->RecoveryPending) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_NOT_READY; }
    if(e->Active==w || e->Notification==w) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY; }
    status=command_context(e,a->hContext,&c);
    if (status!=STATUS_SUCCESS) {
        KeReleaseSpinLock(&e->Lock,irql);return status;
    }
    if(a->DmaBufferVirtualAddress>=c->Root.VaLimit || a->DmaBufferSize>c->Root.VaLimit-a->DmaBufferVirtualAddress) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    if((e->HardwareOwner && !e->UserdBar1Ready) || !e->Graphics.Memory.Handle ||
       (c->GraphicsGeneration && c->GraphicsGeneration!=e->GraphicsGeneration)) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    if (a->DmaBufferVirtualAddress<c->Device->Process->Internal.Base+TG_GPU_INTERNAL_COVERAGE &&
        a->DmaBufferVirtualAddress+a->DmaBufferSize>c->Device->Process->Internal.Base) {
        KeReleaseSpinLock(&e->Lock,irql); return STATUS_INVALID_PARAMETER;
    }
    /* VidMm can replace the root while the immutable snapshot is decoded. */
    if (info && info->va_end && (info->va_end>c->Root.VaLimit ||
        info->reserved_base!=c->Device->Process->Internal.Base)) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    RtlZeroMemory(w,sizeof(*w));
    w->Engine=e;w->Context=c; w->Root=c->Root; w->DmaAddress=a->DmaBufferVirtualAddress;
    w->DmaBytes=a->DmaBufferSize; w->SubmissionFence=w->CompletionFence=a->SubmissionFenceId;
    w->InternalVa=c->Device->Process->Internal.Base;
    w->FencePhysicalAddress=(UINT64)e->Commands.SystemPhysicalAddress.QuadPart+TG_GPU_COMMAND_FENCE;
    w->UploadsMme=info && info->uploads_mme;
    ++c->References;
    KeReleaseSpinLock(&e->Lock,irql); return STATUS_SUCCESS;
}
NTSTATUS TgGpuCapturePagingContext(TG_GPU_CONTEXT_ENGINE *e,HANDLE handle,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&e->Lock,&irql);
    TG_GPU_CONTEXT *c = find_context(e,handle);
    if (e->Stopping || e->Resetting || e->RecoveryPending)
        status = STATUS_DEVICE_NOT_READY;
    else if (c && !c->Closing && c->Flags.SystemContext && c->Flags.VirtualAddressing &&
             c->Device->Process && c->Device->Process->Flags.SystemProcess) {
        status = STATUS_DEVICE_NOT_READY;
        if (c->Root.Valid && NT_SUCCESS(c->RootStatus) && e->Graphics.Memory.Handle &&
            (!c->GraphicsGeneration || c->GraphicsGeneration == e->GraphicsGeneration)) {
            w->Context = c;
            w->Root = c->Root;
            w->InternalVa = c->Device->Process->Internal.Base;
            w->FencePhysicalAddress = (UINT64)e->Commands.SystemPhysicalAddress.QuadPart+TG_GPU_COMMAND_FENCE;
            ++c->References;
            status = STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&e->Lock,irql);
    return status;
}

NTSTATUS TgGpuActivateContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
                                  const struct tg_gpu_command_info *info)
{
    KIRQL irql;UINT marker;NTSTATUS status;
    if(!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(!w->Context || w->Context->Device->Engine!=e || !w->Context->References ||
        w->Expected || !w->Root.Valid) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    if((e->Active && !(w->Paging && e->Active==w)) || e->Notification || e->Channel.referenced) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    if(!e->Graphics.Memory.Handle || (w->Context->GraphicsGeneration &&
       w->Context->GraphicsGeneration!=e->GraphicsGeneration)) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    /* Admission can run ahead of a preceding state-binding packet. Validate
     * inherited state only when this work reaches the serialized execution
     * slot, after that packet has retired and published its context state.
     * This still precedes root writes, fence arming and hardware execution. */
    if (info && ((info->requires_mme && !w->Context->MmeInitialized) ||
                 (info->requires_cb0 && !w->Context->Cb0End))) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    if (info && info->requires_cb0 && w->Context->Cb0End>w->Root.VaLimit) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    if (info) {
        for (UINT engine=0;engine<TG_GPU_PROGRAM_ENGINES;engine++) {
            if (!(info->requires_program & (1u << engine)))
                continue;
            const UINT64 end=w->Context->Programs.end[engine];
            if (!end || end>w->Root.VaLimit) {
                status=end ? STATUS_DEVICE_NOT_READY : STATUS_INVALID_PARAMETER;
                KeReleaseSpinLock(&e->Lock,irql);return status;
            }
        }
    }
    if (info && info->required_compute_slm_per_tpc &&
        (info->required_compute_slm_per_tpc>w->Context->ComputeSlm.bytes_per_tpc ||
         w->Context->ComputeSlm.va_end>w->Root.VaLimit)) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    if (info) {
        for (UINT engine=0;engine<TG_GPU_PROGRAM_ENGINES;engine++) {
            for (UINT pool=0;pool<TG_GPU_TEXTURE_POOLS;pool++) {
                const UINT64 end=w->Context->TexturePool[engine][pool].end;
                if (info->requires_texture_pool[engine][pool] && (!end || end>w->Root.VaLimit)) {
                    KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
                }
            }
        }
    }
    status=TgGpuWriteRootInstance(&w->Root,&w->Context->Instance,TRUE);
    if(!NT_SUCCESS(status)) { KeReleaseSpinLock(&e->Lock,irql);return status; }
    marker=++e->Marker;
    if(!marker || marker==UINT32_MAX) marker=e->Marker=1;
    w->Expected=marker;
    /* CPU initializes only the complement, never the expected completion. */
    *(volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_FENCE)=~marker;
    KeMemoryBarrier();e->Active=w;
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}
NTSTATUS TgGpuCancelCapturedWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;
    if(!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(!w->Context || w->Context->Device->Engine!=e || !w->Context->References || w->Expected || e->Active==w) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    --w->Context->References;w->Context=NULL;
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}
NTSTATUS TgGpuAcquireContextWork(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *a,TG_GPU_CONTEXT_WORK *w)
{
    NTSTATUS status;KIRQL irql;
    if(!e) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    status=e->QueueOwner ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    KeReleaseSpinLock(&e->Lock,irql);
    if(!NT_SUCCESS(status)) return status;
    status=TgGpuCaptureContextWork(e,a,NULL,w);
    if(!NT_SUCCESS(status)) return status;
    status=TgGpuActivateContextWork(e,w,NULL);
    if(!NT_SUCCESS(status)) (VOID)TgGpuCancelCapturedWork(e,w);
    return status;
}
NTSTATUS TgGpuPrepareContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;NTSTATUS status=STATUS_SUCCESS;TG_GPU_CONTEXT *c;
    BOOLEAN build;UINT generation;
    if (!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);c=w->Context;
    if (e->Active!=w || !c || w->Prepared || w->Signaling || w->Notified || w->HardwareBusy || w->Attempted) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    if(!e->Graphics.Memory.Handle || (c->GraphicsGeneration && c->GraphicsGeneration!=e->GraphicsGeneration)) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    build=!c->GraphicsGeneration;generation=e->GraphicsGeneration;
    w->HardwareBusy=TRUE;KeReleaseSpinLock(&e->Lock,irql);
    /* The first immutable clone/whitelist is large. Keep its uncached CPU
     * initialization outside the spin lock, with work/context ownership held.
     * Later submissions preserve the GPU-saved image and global buffers.
     */
    if(build)status=TgGpuBuildContextGraphics(&e->Graphics,&c->Instance,w->InternalVa,c->ContextId);
    KeAcquireSpinLock(&e->Lock,&irql);
    if(NT_SUCCESS(status)) {
        if(build)c->GraphicsGeneration=generation;
        status=TgGpuInstallInternalRoot(e->Tables,&w->Root,&c->Device->Process->Internal);
    }
    if(NT_SUCCESS(status))status=TgGpuMapContextGraphics(&c->Device->Process->Internal,&c->Instance);
    uint32_t gp_put=0;
    if (NT_SUCCESS(status) && !tg_gpu_prepare_indirect_submission(e->Commands.CpuAddress,e->Commands.Bytes,
        (UINT64)e->Commands.SystemPhysicalAddress.QuadPart,w->InternalVa,
        c->Instance.CpuAddress,c->Instance.Bytes,(UINT64)c->Instance.SystemPhysicalAddress.QuadPart,
        w->DmaAddress,w->DmaBytes,w->IndirectAddress,w->IndirectBytes,
        w->IndirectSuffixOffset,w->Expected,&gp_put)) status=STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status)) { w->GpPut=gp_put;KeMemoryBarrier();w->Prepared=TRUE; }
    w->HardwareBusy=FALSE;
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
static BOOLEAN NotifyFence(PVOID context)
{
    TG_GPU_CONTEXT_WORK *w=context; TG_GPU_CONTEXT_ENGINE *e=w->Engine;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data={0};
    if(w->Preemption) {
        data.InterruptType=DXGK_INTERRUPT_DMA_PREEMPTED;
        data.DmaPreempted.PreemptionFenceId=w->SubmissionFence;
        data.DmaPreempted.LastCompletedFenceId=w->CompletionFence;
        data.DmaPreempted.NodeOrdinal=data.DmaPreempted.EngineOrdinal=0;
    } else {
        data.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;
        data.DmaCompleted.SubmissionFenceId=w->SubmissionFence;
        data.DmaCompleted.NodeOrdinal=0; data.DmaCompleted.EngineOrdinal=0;
        e->LastCompletedFence=w->CompletionFence;
    }
    KeMemoryBarrier(); w->Notified=TRUE;
    /* Dxgkrnl already accounts for INVALID_PARAMETER submissions. Their
     * ordered marker updates our fence notion without inventing a DMA event. */
    if(!w->Rejected) {
        e->Dxgk.DxgkCbNotifyInterrupt(e->Dxgk.DeviceHandle,&data);
        (VOID)e->Dxgk.DxgkCbQueueDpc(e->Dxgk.DeviceHandle);
    }
    return TRUE; /* FALSE from QueueDpc may just mean one is already queued */
}
NTSTATUS TgGpuContextWorkFinished(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;NTSTATUS status;
    if(!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Active!=w || !w->Context) status=STATUS_INVALID_PARAMETER;
    else if(w->HardwareBusy) status=STATUS_DEVICE_BUSY;
    else if(!w->Submitted) status=STATUS_DEVICE_NOT_READY;
    else status=*(volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_FENCE)==w->Expected ?
        STATUS_SUCCESS : STATUS_PENDING;
    KeMemoryBarrier();KeReleaseSpinLock(&e->Lock,irql);return status;
}
NTSTATUS TgGpuSignalContextWork(TG_GPU_CONTEXT_ENGINE *e, TG_GPU_CONTEXT_WORK *w,
                               const struct tg_gpu_command_info *info)
{
    KIRQL irql; NTSTATUS status; BOOLEAN invoked=FALSE;
    if (!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if (w->Engine!=e) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER; }
    if (w->HardwareBusy) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    if (w->Signaling) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    if (w->Notified) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_SUCCESS; }
    if(!w->CompletionReady) {
        if(e->Active!=w || !w->Context || e->Notification) {
            KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
        }
        if(!w->Submitted || !w->Retired || !w->GpuCompleted || !w->CpuCompleted) {
            KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;
        }
        if(*(volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_FENCE)!=w->Expected) {
            KeReleaseSpinLock(&e->Lock,irql);return STATUS_PENDING;
        }
        if (w->UploadsMme)
            w->Context->MmeInitialized=TRUE;
        if (info) {
            w->Context->Programs=info->programs;
            if (info->cb0_end)
                w->Context->Cb0End=info->cb0_end;
            if (info->compute_slm.va_end)
                w->Context->ComputeSlm=info->compute_slm;
            for (UINT engine=0;engine<TG_GPU_PROGRAM_ENGINES;engine++) {
                for (UINT pool=0;pool<TG_GPU_TEXTURE_POOLS;pool++) {
                    if (info->texture_pool[engine][pool].end)
                        w->Context->TexturePool[engine][pool]=info->texture_pool[engine][pool];
                }
            }
        }
        /* Retirement has already removed hardware references. Do not make
         * Dxgkrnl retry DestroyContext because our CPU reference lingers after
         * completion. Notification thereafter uses only this engine snapshot.
         */
        --w->Context->References;w->Context=NULL;e->Active=NULL;
        w->CompletionReady=TRUE;e->Notification=w;
    }
    if(e->Notification!=w) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER; }
    if(w->Rejected) {
        /* Pure bookkeeping for an OS-rejected packet needs no DMA event.
         * The queue has already proved all earlier hardware work retired. */
        e->LastCompletedFence=w->CompletionFence;KeMemoryBarrier();w->Notified=TRUE;
        e->Notification=NULL;KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
    }
    KeMemoryBarrier(); w->Signaling=TRUE; KeReleaseSpinLock(&e->Lock,irql);
    status=e->Dxgk.DxgkCbSynchronizeExecution(e->Dxgk.DeviceHandle,NotifyFence,w,0,&invoked);
    KeAcquireSpinLock(&e->Lock,&irql); w->Signaling=FALSE;
    if (w->Notified) { status=STATUS_SUCCESS;e->Notification=NULL; }
    else if (NT_SUCCESS(status)) status=STATUS_UNSUCCESSFUL;
    KeReleaseSpinLock(&e->Lock,irql); return status;
}
NTSTATUS TgGpuCancelContextNotification(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;
    if(!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(w->Engine!=e || e->Notification!=w || !w->CompletionReady || w->Context || w->Notified) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    if(w->Signaling) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY; }
    e->Notification=NULL;w->CompletionReady=FALSE;
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}
NTSTATUS TgGpuCaptureSoftwareWork(TG_GPU_CONTEXT_ENGINE *e,const DXGKARG_SUBMITCOMMANDVIRTUAL *a,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;TG_GPU_CONTEXT *c;
    if(!e || !a || !w || a->NodeOrdinal || a->EngineOrdinal)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);c=find_context(e,a->hContext);
    if(e->Stopping){KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;}
    if((!c && !(a->Flags.ContextSwitch && !a->hContext)) || (c && c->Closing)) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(w,sizeof(*w));w->Engine=e;w->Software=TRUE;
    w->SubmissionFence=w->CompletionFence=a->SubmissionFenceId;
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}
NTSTATUS TgGpuArmSoftwareCompletion(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w)
{
    KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!e || !w || w->Engine!=e || (!w->Software && !w->Preemption) || w->Context)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(w->CompletionReady)status=e->Notification==w?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
    else if(e->Active || e->Notification || e->Channel.referenced)status=STATUS_PENDING;
    else {
        if(w->Preemption)w->CompletionFence=e->LastCompletedFence;
        e->Notification=w;w->CompletionReady=TRUE;
    }
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
