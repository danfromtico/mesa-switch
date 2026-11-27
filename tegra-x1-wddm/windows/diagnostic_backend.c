/* SPDX-License-Identifier: MIT */
#include "diagnostic_backend.h"
#include "worker.h"
#include "submit_ddi.h"
#include "reset_backend.h"
#include "paging_dispatch.h"
#include "../core/fault.h"
#include "../platform/t214_resources.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"

#define TAG 0x44674754u
#define USER_VA UINT64_C(0x02000000)
#define INTERNAL_VA UINT64_C(0x10000000)
#define REPORT_OFFSET 0x1000u
#define PIXEL_OFFSET 0x2000u
#define CODE_OFFSET 0x3000u
#define DRAW_DATA_BYTES 0x5000u
#define PAGING_SRC_OFFSET 0x5000u
#define PAGING_DST_OFFSET 0x6000u
#define DATA_BYTES 0x7000u
#define PAGING_FENCE 1u
#define PAGING_SRC_GUARD 32u
#define PAGING_DST_GUARD 48u
#define PAGING_TEST_BYTES (PAGE_SIZE-96u)
#define DECOY_OFFSET 0x800u
#define PREEMPT_FENCE 0xcafeu
#define REFERENCE_HEADER (NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE)|NVVAL(NVB06F,DMA_INCR,COUNT,1)|NVVAL(NVB06F,DMA_INCR,ADDRESS,NVB06F_SET_REFERENCE>>2))

struct _TG_GPU_DIAGNOSTIC_BACKEND {
    DXGKRNL_INTERFACE Dxgk;
    TG_GPU_TABLE_SEGMENT Tables;
    TG_GPU_CONTEXT_ENGINE Engine;
    TG_GPU_HARDWARE Hardware;
    TG_GPU_WORK_QUEUE Queue;
    TG_GPU_WORKER Worker;
    TG_GPU_PRIVATE_MEMORY Data;
    TG_GPU_PROCESS *Process;
    TG_GPU_DEVICE *Device;
    TG_GPU_CONTEXT *Context;
    TG_GPU_CONTEXT *PagingContext;
    TG_GPU_QUEUE_ENTRY PagingPrivate[4];
    UINT64 PagingBuffer[64];
    struct {struct tg_gpu_submit_request Header;UINT Words[PAGE_SIZE/4];} Private;
    KEVENT Completed;
    KSPIN_LOCK NotifyLock;
    struct tg_gpu_backend_status Progress;
    LONG Allocations;
    UINT Cycle,ExpectedSnapshotBytes;
    BOOLEAN EngineInitialized,QueueInitialized;
};
typedef struct _DIAGNOSTIC_MEMORY {
    TG_GPU_DIAGNOSTIC_BACKEND *Owner;
    PVOID Cpu;
    SIZE_T Bytes;
} DIAGNOSTIC_MEMORY;

/* The standalone lifetime fixture links this cleanup body without importing
 * the kernel-only execution path. Production builds include every path. */
#ifndef TG_GPU_DIAGNOSTIC_CLEANUP_FIXTURE
static NTSTATUS NTAPI allocate(HANDLE device,DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *a)
{
    TG_GPU_DIAGNOSTIC_BACKEND *b=device;DIAGNOSTIC_MEMORY *m;PHYSICAL_ADDRESS boundary={0};
    if(!b || !a || !a->NumberOfBytes || a->CacheType!=DXGK_MEMORY_CACHING_TYPE_NON_CACHED)
        return STATUS_INVALID_PARAMETER;
    m=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*m),TAG);
    if(!m)return STATUS_NO_MEMORY;
    m->Cpu=MmAllocateContiguousMemorySpecifyCache(a->NumberOfBytes,a->LowestAcceptableAddress,
        a->HighestAcceptableAddress,boundary,MmNonCached);
    if(!m->Cpu){ExFreePoolWithTag(m,TAG);return STATUS_INSUFFICIENT_RESOURCES;}
    m->Owner=b;m->Bytes=a->NumberOfBytes;a->hMemoryHandle=m;a->pMemory=m->Cpu;
    InterlockedIncrement(&b->Allocations);return STATUS_SUCCESS;
}
static NTSTATUS NTAPI release(HANDLE device,const DXGKARGCB_FREECONTIGUOUSMEMORY *a)
{
    TG_GPU_DIAGNOSTIC_BACKEND *b=device;DIAGNOSTIC_MEMORY *m=a?a->hMemoryHandle:NULL;
    if(!m || m->Owner!=b)return STATUS_INVALID_PARAMETER;
    MmFreeContiguousMemorySpecifyCache(m->Cpu,m->Bytes,MmNonCached);
    ExFreePoolWithTag(m,TAG);InterlockedDecrement(&b->Allocations);return STATUS_SUCCESS;
}
static NTSTATUS NTAPI synchronize(HANDLE device,PKSYNCHRONIZE_ROUTINE routine,
    PVOID context,ULONG message,PBOOLEAN result)
{
    TG_GPU_DIAGNOSTIC_BACKEND *b=device;KIRQL irql;
    if(!b || !routine || !result || message)return STATUS_INVALID_PARAMETER;
    /* This private lock is not a Dxgkrnl interrupt lock or a hardware ISR. */
    KeAcquireSpinLock(&b->NotifyLock,&irql);*result=routine(context);
    KeReleaseSpinLock(&b->NotifyLock,irql);return STATUS_SUCCESS;
}
static UINT draw_fence(UINT cycle,UINT index)
{
    return 3 + cycle * 4 + index;
}

static VOID NTAPI notification(HANDLE device,DXGKARGCB_NOTIFY_INTERRUPT_DATA *a)
{
    TG_GPU_DIAGNOSTIC_BACKEND *b=device;UINT n=b->Progress.notifications,p=b->Progress.preemption_notifications;
    if (a->InterruptType==DXGK_INTERRUPT_DMA_COMPLETED &&
        !a->DmaCompleted.NodeOrdinal && !a->DmaCompleted.EngineOrdinal &&
        a->DmaCompleted.SubmissionFenceId==PAGING_FENCE && !b->Progress.paging_fence)
        b->Progress.paging_fence=PAGING_FENCE;
    else if(a->InterruptType==DXGK_INTERRUPT_DMA_COMPLETED &&
       !a->DmaCompleted.NodeOrdinal && !a->DmaCompleted.EngineOrdinal &&
       n<4 && a->DmaCompleted.SubmissionFenceId==draw_fence(n/2,n%2?2u:0u))
        InterlockedIncrement((volatile LONG *)&b->Progress.notifications);
    else if(a->InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED &&
        !a->DmaPreempted.NodeOrdinal && !a->DmaPreempted.EngineOrdinal &&
        p<2 && a->DmaPreempted.PreemptionFenceId==PREEMPT_FENCE+p &&
        a->DmaPreempted.LastCompletedFenceId==draw_fence(p,0)) {
        b->Progress.preemption_completed=a->DmaPreempted.LastCompletedFenceId;
        InterlockedIncrement((volatile LONG *)&b->Progress.preemption_notifications);
    }
    else InterlockedIncrement((volatile LONG *)&b->Progress.recovery_requests);
    (VOID)KeSetEvent(&b->Completed,IO_NO_INCREMENT,FALSE);
}
static BOOLEAN NTAPI queue_dpc(HANDLE device)
{
    UNREFERENCED_PARAMETER(device);
    /* The diagnostic consumes notifications above, without an OS DPC claim. */
    return TRUE;
}
static VOID NTAPI notify_dpc(HANDLE device){UNREFERENCED_PARAMETER(device);}
static NTSTATUS NTAPI reserve(HANDLE device,DXGKARGCB_RESERVEGPUVIRTUALADDRESSRANGE *a)
{
    if(!device || !a || a->hDxgkProcess!=device || a->Flags || a->BaseAddress ||
       a->SizeInBytes!=TG_GPU_INTERNAL_COVERAGE || a->Alignment!=TG_GPU_INTERNAL_COVERAGE)
        return STATUS_INVALID_PARAMETER;
    /* No application runs in this address space. Real WDDM must reserve with
     * VidMm; the diagnostic alone chooses a fixed private 64 MiB window. */
    a->StartVirtualAddress=INTERNAL_VA;return STATUS_SUCCESS;
}
static NTSTATUS map_data(TG_GPU_DIAGNOSTIC_BACKEND *b,TG_GPU_CONTEXT *context)
{
    UINT64 *root=b->Tables.Memory.CpuAddress,*leaf=root+TG_GPU_TABLE_ENTRIES;
    UINT i;DXGKARG_SETROOTPAGETABLE set={0};
    if(!tg_gpu_pde_small((UINT64)b->Tables.Memory.SystemPhysicalAddress.QuadPart+TG_GPU_TABLE_BYTES,&root[0]))
        return STATUS_INVALID_ADDRESS;
    for(i=0;i<DRAW_DATA_BYTES/PAGE_SIZE;++i) {
        if(!tg_gpu_pte_4k((UINT64)b->Data.SystemPhysicalAddress.QuadPart+(UINT64)i*PAGE_SIZE,
            i==0 || i>=3,true,&leaf[(USER_VA>>TG_GPU_PAGE_SHIFT)+i]))return STATUS_INVALID_ADDRESS;
        /* USER mappings: never set PTE_PRIVILEGE for reports, code or target. */
    }
    KeMemoryBarrier();set.hContext=context;set.Address.SegmentId=b->Tables.SegmentId;
    set.NumEntries=TG_GPU_TABLE_ENTRIES;TgGpuDdiSetRootPageTable(&b->Engine,&set);
    return context->RootStatus;
}
static NTSTATUS enqueue(TG_GPU_DIAGNOSTIC_BACKEND *b,UINT index,UINT offset,UINT words)
{
    DXGKARG_SUBMITCOMMANDVIRTUAL a={0};NTSTATUS status;
    a.hContext=b->Context;a.DmaBufferVirtualAddress=USER_VA+offset;a.DmaBufferSize=words*4;
    if(words>PAGE_SIZE/4)return STATUS_INVALID_PARAMETER;
    b->Private.Header=(struct tg_gpu_submit_request){TG_GPU_SUBMIT_ABI,sizeof(b->Private.Header)+words*4,words*4,0};
    RtlCopyMemory(b->Private.Words,(UCHAR *)b->Data.CpuAddress+offset,words*4);
    a.SubmissionFenceId=draw_fence(b->Cycle,index);a.pDmaBufferPrivateData=&b->Private;
    a.DmaBufferPrivateDataSize=sizeof(b->Private);a.DmaBufferUmdPrivateDataSize=b->Private.Header.size;
    KeMemoryBarrier();status=TgGpuDdiSubmitCommandVirtual(&b->Engine,&a);
    if(status!=STATUS_SUCCESS)return status;
    /* Corrupt both originating copies after admission. Neither this reused
     * private payload nor the original GPU VA may become the execution PB. */
    for(UINT i=0;i<words;++i) {
        b->Private.Words[i]=0xc0000000; /* reserved DMA opcode, never runnable */
        ((UINT32 *)((UCHAR *)b->Data.CpuAddress+offset))[i]=0xc0000000;
    }
    KeMemoryBarrier();++b->Progress.source_mutations;
    if(index!=1)b->ExpectedSnapshotBytes+=words*4;
    ++b->Progress.submissions;return STATUS_SUCCESS;
}
static NTSTATUS reject_reference(TG_GPU_DIAGNOSTIC_BACKEND *b)
{
    DXGKARG_SUBMITCOMMANDVIRTUAL a={.hContext=b->Context,.DmaBufferVirtualAddress=USER_VA,
        .DmaBufferSize=8,.SubmissionFenceId=draw_fence(b->Cycle,0)-1};
    b->Private.Header=(struct tg_gpu_submit_request){TG_GPU_SUBMIT_ABI,sizeof(b->Private.Header)+8,8,0};
    b->Private.Words[0]=REFERENCE_HEADER;b->Private.Words[1]=b->Engine.Marker+1;
    a.pDmaBufferPrivateData=&b->Private;a.DmaBufferPrivateDataSize=sizeof(b->Private);
    a.DmaBufferUmdPrivateDataSize=b->Private.Header.size;
    if(TgGpuDdiSubmitCommandVirtual(&b->Engine,&a)!=STATUS_INVALID_PARAMETER ||
       b->Context->References || b->Engine.RecoveryPending)return STATUS_DEVICE_DATA_ERROR;
    ++b->Progress.snapshot_rejections;return STATUS_SUCCESS;
}
static NTSTATUS wait_completed(TG_GPU_DIAGNOSTIC_BACKEND *b,UINT completed)
{
    LARGE_INTEGER timeout;NTSTATUS status;
    ULONGLONG deadline=KeQueryInterruptTime()+30000000;
    for(;;) {
        ULONGLONG now;
        KeClearEvent(&b->Completed);KeMemoryBarrier();
        if(b->Progress.recovery_requests || !NT_SUCCESS(b->Worker.Failure))
            return NT_SUCCESS(b->Worker.Failure)?STATUS_DEVICE_DATA_ERROR:b->Worker.Failure;
        if(b->Progress.notifications==completed && b->Progress.paging_fence==PAGING_FENCE)break;
        now=KeQueryInterruptTime();if(now>=deadline)return STATUS_IO_TIMEOUT;
        timeout.QuadPart=-(LONGLONG)(deadline-now);
        status=KeWaitForSingleObject(&b->Completed,Executive,KernelMode,FALSE,&timeout);
        if(status!=STATUS_SUCCESS && status!=STATUS_TIMEOUT)return status;
    }
    /* Notification can wake us before the worker finishes its bookkeeping.
     * Wait for its dispatch gate before reading saved GR state or reusing PB. */
    status=KeWaitForSingleObject(&b->Queue.DispatchGate,Executive,KernelMode,FALSE,NULL);
    if(status!=STATUS_SUCCESS)return status;
    if(b->Engine.Active || b->Engine.Notification || b->Engine.Channel.referenced)
        status=STATUS_DEVICE_BUSY;
    else if(tg_gpu_frontend_sync_memory(&b->Hardware.Io,&b->Hardware.Power,
        &b->Engine.Channel.frontend)!=TG_GPU_FE_OK)status=STATUS_DEVICE_DATA_ERROR;
    KeMemoryBarrier();(VOID)KeReleaseMutex(&b->Queue.DispatchGate,FALSE);return status;
}
static NTSTATUS preempt_then_draw(TG_GPU_DIAGNOSTIC_BACKEND *b,UINT seed)
{
    DXGKARG_PREEMPTCOMMAND a={.PreemptionFenceId=PREEMPT_FENCE+b->Cycle};KIRQL irql;
    UINT draw_words;NTSTATUS status;
    UINT32 *decoy=(UINT32 *)((UCHAR *)b->Data.CpuAddress+DECOY_OFFSET);
    if(!tg_gpu_build_gr_engine_packet(decoy,(PAGE_SIZE-DECOY_OFFSET)/4,
        USER_VA+REPORT_OFFSET+8,seed^0x2468ace0u,false) ||
       !tg_gpu_build_draw_packet(b->Data.CpuAddress,DECOY_OFFSET/4,
        USER_VA+CODE_OFFSET,USER_VA+REPORT_OFFSET+4,~seed,&draw_words))return STATUS_INVALID_PARAMETER;
    /* Hold the real dispatcher mutex to make the admission cut reproducible:
     * canceled report packet -> DISPATCH preempt marker -> surviving draw.
     * The kernel worker cannot consume this prefix until it is complete. */
    status=KeWaitForSingleObject(&b->Queue.DispatchGate,Executive,KernelMode,FALSE,NULL);
    if(status!=STATUS_SUCCESS)return status;
    status=enqueue(b,1,DECOY_OFFSET,TG_GPU_GR_ENGINE_NEXT_WORDS);
    if(NT_SUCCESS(status)) {
        irql=KeRaiseIrqlToDpcLevel();
        status=TgGpuDdiPreemptCommand(&b->Engine,&a);
        KeLowerIrql(irql);++b->Progress.preemptions;
        if(NT_SUCCESS(status))status=enqueue(b,2,0,draw_words);
    }
    (VOID)KeReleaseMutex(&b->Queue.DispatchGate,FALSE);
    if(NT_SUCCESS(status))status=wait_completed(b,(b->Cycle+1)*2);
    return status;
}
static NTSTATUS test_virtual_paging(TG_GPU_DIAGNOSTIC_BACKEND *b,UINT pattern)
{
    DXGKARG_CREATECONTEXT create={.EngineAffinity=1,.Flags={.VirtualAddressing=1,.SystemContext=1}};
    NTSTATUS status=TgGpuDdiCreateContext(b->Device,&create);
    if (status!=STATUS_SUCCESS) return status;
    b->PagingContext=create.hContext;
    status=map_data(b,b->PagingContext);
    if (status!=STATUS_SUCCESS) return status;

    UINT64 physical=(UINT64)b->Data.SystemPhysicalAddress.QuadPart;
    UINT32 *src=(UINT32 *)((UCHAR *)b->Data.CpuAddress+PAGING_SRC_OFFSET);
    UINT32 *dst=(UINT32 *)((UCHAR *)b->Data.CpuAddress+PAGING_DST_OFFSET);
    for (UINT i=0;i<PAGE_SIZE/4;++i) src[i]=dst[i]=TG_GPU_CLEAR_SENTINEL;
    if (pattern==TG_GPU_CLEAR_SENTINEL) pattern^=1;

    DXGK_PTE entries[2]={0};
    entries[0].Valid=entries[1].Valid=1;
    entries[0].PageAddress=(physical+PAGING_SRC_OFFSET)>>PAGE_SHIFT;
    entries[1].PageAddress=(physical+PAGING_DST_OFFSET)>>PAGE_SHIFT;
    DXGKARG_BUILDPAGINGBUFFER build={.pDmaBuffer=b->PagingBuffer,.DmaSize=sizeof(b->PagingBuffer),
        .pDmaBufferPrivateData=b->PagingPrivate,.DmaBufferPrivateDataSize=sizeof(b->PagingPrivate),
        .hSystemContext=b->PagingContext,.DmaBufferGpuVirtualAddress=0x200000,
        .Operation=DXGK_OPERATION_UPDATE_PAGE_TABLE};
    build.UpdatePageTable=(DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE){
        .PageTableAddress.GpuPhysical={.SegmentId=b->Tables.SegmentId,.SegmentOffset=TG_GPU_TABLE_BYTES},
        .pPageTableEntries=entries,.StartIndex=(UINT)((USER_VA+PAGING_SRC_OFFSET)>>TG_GPU_PAGE_SHIFT),
        .NumPageTableEntries=2,.hProcess=b->Process,.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL};
    status=TgGpuDdiBuildPagingBuffer(&b->Engine,&build);
    if (status!=STATUS_SUCCESS) return status;

    build.DmaBufferWriteOffset=sizeof(b->PagingBuffer)-build.DmaSize;
    build.Operation=DXGK_OPERATION_FLUSH_TLB;
    build.FlushTlb=(DXGK_BUILDPAGINGBUFFER_FLUSHTLB){
        .RootPageTableAddress={.SegmentId=b->Tables.SegmentId},.hProcess=b->Process};
    status=TgGpuDdiBuildPagingBuffer(&b->Engine,&build);
    if (status!=STATUS_SUCCESS) return status;

    build.DmaBufferWriteOffset=sizeof(b->PagingBuffer)-build.DmaSize;
    build.Operation=DXGK_OPERATION_VIRTUAL_FILL;
    build.FillVirtual=(DXGK_BUILDPAGINGBUFFER_FILLVIRTUAL){.FillSizeInBytes=PAGING_TEST_BYTES,
        .FillPattern=pattern,.DestinationVirtualAddress=USER_VA+PAGING_SRC_OFFSET+PAGING_SRC_GUARD};
    status=TgGpuDdiBuildPagingBuffer(&b->Engine,&build);
    if (status!=STATUS_SUCCESS) return status;

    build.DmaBufferWriteOffset=sizeof(b->PagingBuffer)-build.DmaSize;
    build.Operation=DXGK_OPERATION_VIRTUAL_TRANSFER;
    build.TransferVirtual=(DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL){.TransferSizeInBytes=PAGING_TEST_BYTES,
        .SourceVirtualAddress=USER_VA+PAGING_SRC_OFFSET+PAGING_SRC_GUARD,
        .DestinationVirtualAddress=USER_VA+PAGING_DST_OFFSET+PAGING_DST_GUARD,
        .TransferDirection=DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL};
    status=TgGpuDdiBuildPagingBuffer(&b->Engine,&build);
    if (status!=STATUS_SUCCESS) return status;

    DXGKARG_SUBMITCOMMANDVIRTUAL submit={.hContext=b->PagingContext,.Flags.Paging=1,
        .DmaBufferVirtualAddress=build.DmaBufferGpuVirtualAddress,
        .DmaBufferSize=sizeof(b->PagingBuffer)-build.DmaSize,
        .pDmaBufferPrivateData=b->PagingPrivate,
        .DmaBufferPrivateDataSize=sizeof(b->PagingPrivate)-build.DmaBufferPrivateDataSize,
        .SubmissionFenceId=PAGING_FENCE};
    status=TgGpuDdiSubmitCommandVirtual(&b->Engine,&submit);
    if (status!=STATUS_SUCCESS) return status;
    status=wait_completed(b,0);
    if (status!=STATUS_SUCCESS) return status;
    if (b->Engine.Marker!=2 || b->Engine.LastCompletedFence!=PAGING_FENCE ||
        b->PagingContext->References) return STATUS_DEVICE_DATA_ERROR;

    for (UINT i=0;i<PAGE_SIZE;i+=4) {
        UINT32 expected_src=i>=PAGING_SRC_GUARD && i<PAGING_SRC_GUARD+PAGING_TEST_BYTES?
            pattern:TG_GPU_CLEAR_SENTINEL;
        UINT32 expected_dst=i>=PAGING_DST_GUARD && i<PAGING_DST_GUARD+PAGING_TEST_BYTES?
            pattern:TG_GPU_CLEAR_SENTINEL;
        b->Progress.paging_errors+=((volatile UINT32 *)src)[i/4]!=expected_src;
        b->Progress.paging_errors+=((volatile UINT32 *)dst)[i/4]!=expected_dst;
    }
    b->Progress.paging_bytes=PAGING_TEST_BYTES;
    if (b->Progress.paging_errors) return STATUS_DATA_ERROR;
    status=TgGpuDdiDestroyContext(b->PagingContext);
    if (status==STATUS_SUCCESS) b->PagingContext=NULL;
    return status;
}
static NTSTATUS draw_cycle(TG_GPU_DIAGNOSTIC_BACKEND *b,UINT seed,struct tg_gpu_status *s)
{
    UINT words,i;NTSTATUS status;
    const volatile UINT32 *reports=(const volatile UINT32 *)((UCHAR *)b->Data.CpuAddress+REPORT_OFFSET);
    status=reject_reference(b);if(!NT_SUCCESS(status))return status;
    s->expected[0]=seed;s->expected[1]=~seed;
    for(i=0;i<3;++i)((UINT32 *)((UCHAR *)b->Data.CpuAddress+REPORT_OFFSET))[i]=TG_GPU_CLEAR_SENTINEL;
    for(i=0;i<TG_GPU_CLEAR_BYTES/4;++i)((UINT32 *)((UCHAR *)b->Data.CpuAddress+PIXEL_OFFSET))[i]=TG_GPU_CLEAR_SENTINEL;
    if(!tg_gpu_prepare_draw_code((UCHAR *)b->Data.CpuAddress+CODE_OFFSET,TG_GPU_DRAW_BYTES,&s->draw) ||
       !tg_gpu_build_clear_packet(b->Data.CpuAddress,PAGE_SIZE/4,USER_VA+PIXEL_OFFSET,
        USER_VA+REPORT_OFFSET,seed,true,&words))return STATUS_INVALID_PARAMETER;
    b->Progress.stage=5;status=enqueue(b,0,0,words);
    if(NT_SUCCESS(status))status=wait_completed(b,b->Cycle*2+1);if(!NT_SUCCESS(status))return status;
    b->Progress.stage=6;status=preempt_then_draw(b,seed);if(!NT_SUCCESS(status))return status;
    b->Progress.stage=7;
    s->draw.checked=tg_gpu_check_draw_code((const volatile UINT32 *)((UCHAR *)b->Data.CpuAddress+CODE_OFFSET),TG_GPU_DRAW_BYTES);
    if(!tg_gpu_check_clear_pixels((const volatile UINT32 *)((UCHAR *)b->Data.CpuAddress+PIXEL_OFFSET),
        TG_GPU_CLEAR_BYTES,&s->clear) || !s->draw.checked || reports[0]!=seed || reports[1]!=~seed ||
        reports[2]!=TG_GPU_CLEAR_SENTINEL)return STATUS_DATA_ERROR;
    return STATUS_SUCCESS;
}
static VOID snapshot(TG_GPU_DIAGNOSTIC_BACKEND *b,struct tg_gpu_status *s)
{
    TG_GPU_CONTEXT_ENGINE *e=&b->Engine;
    if(e->Commands.CpuAddress) {
        const volatile UINT32 *userd=(const volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_USERD);
        const volatile UINT32 *ring=e->Commands.CpuAddress;
        b->Progress.userd_gp_get=userd[34];b->Progress.userd_gp_put=userd[35];
        b->Progress.user_priv=(ring[1]>>8)&1;b->Progress.kernel_priv=(ring[3]>>8)&1;
        b->Progress.fence_cpu=*(const volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_FENCE);
        s->userd_gp_get=userd[34];s->userd_gp_put=userd[35];
    }
    b->Progress.completion_fence=e->LastCompletedFence;
    b->Progress.snapshot_uploads=e->SnapshotUploads;b->Progress.snapshot_bytes=e->SnapshotBytes;
    if(b->Context) {
        const volatile UINT32 *image=(const volatile UINT32 *)((UCHAR *)b->Context->Instance.CpuAddress+TG_GPU_CONTEXT_GR_OFFSET);
        b->Progress.context_generation=b->Context->GraphicsGeneration;
        s->gr_channel.image_ready=b->Context->GraphicsGeneration!=0;
        s->gr_channel.class_id=TG_GPU_GR_ENGINE_CLASS;
        s->gr_channel.save_ops=image[0xf4/4];s->gr_channel.restore_ops=image[0xf8/4];
    }
    if(b->Data.CpuAddress) {
        const volatile UINT32 *reports=(const volatile UINT32 *)((UCHAR *)b->Data.CpuAddress+REPORT_OFFSET);
        s->observed[0]=reports[0];s->observed[1]=reports[1];
        b->Progress.cancelled_report=reports[2];
    }
    if(b->Hardware.Power.ready) {
        struct tg_gpu_fault_status faults;
        const struct tg_gpu_power_io *io=&b->Hardware.Io;
        (VOID)tg_gpu_fault_poll(io,&faults);
        s->fifo_interrupt=faults.fifo;s->mmu_fault=faults.mmu;
        s->pbdma_interrupt=faults.pbdma;s->pbdma_interrupt_1=faults.pbdma1;
        b->Progress.fault_id=UINT32_MAX;
        if(faults.mmu && faults.mmu!=UINT32_MAX) {
            UINT id=0,base;
            while(!(faults.mmu&(1u<<id)))++id;
            base=T214_GPU_REG_BASE+0x2800+id*16;
            b->Progress.fault_id=id;
            b->Progress.fault_instance=io->read32(io->context,base);
            b->Progress.fault_address_lo=io->read32(io->context,base+4);
            b->Progress.fault_address_hi=io->read32(io->context,base+8);
            b->Progress.fault_info=io->read32(io->context,base+12);
        }
        if(e->UserdBar1Ready)b->Progress.fence_observed=io->read32(io->context,
            T214_GPU_BAR1_BASE+e->UserdBar1Offset+TG_GPU_COMMAND_FENCE-TG_GPU_COMMAND_USERD);
        tg_gpu_gr_snapshot(io,&s->gr);s->gr_channel.current_context=s->gr.current_context;
#define REG(field,offset) s->method_error.field=io->read32(io->context,T214_GPU_REG_BASE+(offset))
        REG(status,0x400100);REG(trapped_address,0x400704);REG(data_lo,0x400708);REG(data_hi,0x40070c);
        REG(exception,0x400108);REG(fe_esr,0x404000);REG(mme_esr,0x404490);REG(scc_esr,0x408030);
#undef REG
    }
    b->Progress.fence_expected=e->Marker;b->Progress.channel_stage=e->Channel.stage;
    s->backend=b->Progress;s->backend.allocations_remaining=(UINT)b->Allocations;
}
#endif
NTSTATUS TgGpuStopDiagnosticBackend(TG_GPU_DIAGNOSTIC_BACKEND **state,struct tg_gpu_status *s)
{
    TG_GPU_DIAGNOSTIC_BACKEND *b;NTSTATUS status,hardware_status=STATUS_SUCCESS;
    PAGED_CODE();
    if(!state)return STATUS_INVALID_PARAMETER;
    b=*state;if(!b)return STATUS_SUCCESS;
    status=TgGpuStopWorker(&b->Worker);if(!NT_SUCCESS(status))goto done;
    if(b->QueueInitialized) {
        status=TgGpuResetWorkQueue(&b->Queue,&b->Hardware.Io,&b->Hardware.Power,&b->Hardware.Clock);
        if(!NT_SUCCESS(status))goto done;
        status=TgGpuStopWorkQueue(&b->Queue);if(!NT_SUCCESS(status))goto done;
        b->QueueInitialized=FALSE;
    }
    hardware_status=TgGpuStopHardware(&b->Hardware);
    if(b->Hardware.Engine){status=hardware_status;goto done;}
    if (b->PagingContext) {
        status=TgGpuDdiDestroyContext(b->PagingContext);
        if (status!=STATUS_SUCCESS) goto done;
        b->PagingContext=NULL;
    }
    if(b->Context) {
        status=TgGpuDdiDestroyContext(b->Context);if(!NT_SUCCESS(status))goto done;b->Context=NULL;
    }
    if(b->Device) {
        status=TgGpuDdiDestroyDevice(b->Device);if(!NT_SUCCESS(status))goto done;b->Device=NULL;
    }
    if(b->Process) {
        status=TgGpuDdiDestroyProcess(&b->Engine,b->Process);if(!NT_SUCCESS(status))goto done;b->Process=NULL;
    }
    if(b->EngineInitialized) {
        status=TgGpuStopContextEngine(&b->Engine);if(!NT_SUCCESS(status))goto done;b->EngineInitialized=FALSE;
    }
    status=TgGpuFreePrivateMemory(&b->Dxgk,&b->Data);if(!NT_SUCCESS(status))goto done;
    status=TgGpuDestroyTableSegment(&b->Dxgk,&b->Tables);if(!NT_SUCCESS(status))goto done;
    status=b->Allocations?STATUS_DEVICE_BUSY:hardware_status;
done:
    if(s) {
        s->restore_result=b->Hardware.Status.restore_result;
        s->resources_owned=b->Allocations || b->Worker.Thread || b->Hardware.Engine || b->QueueInitialized;
        s->backend.allocations_remaining=(UINT)b->Allocations;
    }
    if(!b->Allocations && !b->Worker.Thread && !b->Hardware.Engine && !b->QueueInitialized &&
       !b->Context && !b->PagingContext && !b->Device && !b->Process && !b->EngineInitialized) {
        ExFreePoolWithTag(b,TAG);*state=NULL;
    }
    return status;
}
#ifndef TG_GPU_DIAGNOSTIC_CLEANUP_FIXTURE
NTSTATUS TgGpuRunDiagnosticBackend(TG_GPU_DIAGNOSTIC_BACKEND **state,
    const struct tg_gpu_power_io *io,UCHAR address,const struct tg_gpu_gr_payload *firmware,
    UINT seed,struct tg_gpu_status *s)
{
    TG_GPU_DIAGNOSTIC_BACKEND *b;NTSTATUS status,cleanup;TG_GPU_CONTEXT_WORK lost={0};
    DXGKARG_CREATEPROCESS process={0};DXGKARG_CREATEDEVICE device={0};DXGKARG_CREATECONTEXT context={0};
    DXGKARG_SUBMITCOMMANDVIRTUAL old_submit={0};
    PAGED_CODE();
    if(!state || *state || !io || !firmware || !s)return STATUS_INVALID_PARAMETER;
    b=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*b),TAG);if(!b)return STATUS_NO_MEMORY;
    *state=b;b->Progress.requested=1;b->Progress.stage=1;
    KeInitializeSpinLock(&b->NotifyLock);KeInitializeEvent(&b->Completed,NotificationEvent,FALSE);
    b->Dxgk.Version=DXGKDDI_INTERFACE_VERSION_WDDM2_4;b->Dxgk.DeviceHandle=b;
    b->Dxgk.DxgkCbAllocateContiguousMemory=allocate;b->Dxgk.DxgkCbFreeContiguousMemory=release;
    b->Dxgk.DxgkCbSynchronizeExecution=synchronize;b->Dxgk.DxgkCbNotifyInterrupt=notification;
    b->Dxgk.DxgkCbQueueDpc=queue_dpc;b->Dxgk.DxgkCbNotifyDpc=notify_dpc;
    b->Dxgk.DxgkCbReserveGpuVirtualAddressRange=reserve;
    status=TgGpuCreateTableSegment(&b->Dxgk,1,2*TG_GPU_TABLE_BYTES,&b->Tables);
    if(!NT_SUCCESS(status))goto done;
    b->EngineInitialized=TRUE;status=TgGpuInitializeContextEngine(&b->Engine,&b->Dxgk,&b->Tables);
    if(!NT_SUCCESS(status))goto done;
    b->Progress.stage=2;status=TgGpuStartHardware(&b->Hardware,&b->Engine,io,address,firmware);
    *s=b->Hardware.Status;s->tested=1;s->expected[0]=seed;s->expected[1]=~seed;
    if(!NT_SUCCESS(status))goto done;
    b->Progress.stage=3;process.hDxgkProcess=b;process.Flags.SystemProcess=1;
    status=TgGpuDdiCreateProcess(&b->Engine,&process);if(!NT_SUCCESS(status))goto done;
    b->Process=process.hKmdProcess;device.hKmdProcess=b->Process;
    status=TgGpuDdiCreateDevice(&b->Engine,&device);if(!NT_SUCCESS(status))goto done;b->Device=device.hDevice;
    context.Flags.VirtualAddressing=1;context.EngineAffinity=1;
    status=TgGpuDdiCreateContext(b->Device,&context);if(!NT_SUCCESS(status))goto done;b->Context=context.hContext;
    status=TgGpuAllocatePrivateMemory(&b->Dxgk,DATA_BYTES,0x80000000,0xffffffff,&b->Data);
    if(!NT_SUCCESS(status))goto done;
    status=map_data(b,b->Context);if(!NT_SUCCESS(status))goto done;
    b->Progress.stage=4;status=TgGpuInitializeWorkQueue(&b->Queue,&b->Engine);
    if(!NT_SUCCESS(status))goto done;b->QueueInitialized=TRUE;
    status=TgGpuStartWorker(&b->Worker,&b->Queue,&b->Hardware);
    if(!NT_SUCCESS(status))goto done;b->Progress.worker_started=1;
    status=test_virtual_paging(b,seed);if(!NT_SUCCESS(status))goto done;
    status=draw_cycle(b,seed,s);if(!NT_SUCCESS(status))goto done;
    b->Progress.stage=8;status=TgGpuResetBackend(&b->Engine,&b->Queue,&b->Hardware,&b->Worker);
    b->QueueInitialized=b->Queue.Engine==&b->Engine;if(!NT_SUCCESS(status))goto done;
    ++b->Progress.resets;
    if(!b->Hardware.Isolated || b->Hardware.Power.owned || b->Worker.Thread || b->QueueInitialized ||
       b->Engine.UserdBar1Ready || !b->Hardware.Boot.Handle || !b->Hardware.Work.Handle ||
       !b->Engine.Graphics.Memory.Handle || !b->Engine.Commands.Handle) {status=STATUS_DEVICE_DATA_ERROR;goto done;}
    b->Progress.reset_isolated=1;
    b->Progress.stage=9;
    status=TgGpuRestartBackend(&b->Engine,&b->Queue,&b->Hardware,&b->Worker,io,address,firmware);
    b->QueueInitialized=b->Queue.Engine==&b->Engine;if(!NT_SUCCESS(status))goto done;
    ++b->Progress.restarts;
    old_submit.hContext=b->Context;old_submit.DmaBufferVirtualAddress=USER_VA;old_submit.DmaBufferSize=4;
    status=TgGpuCaptureContextWork(&b->Engine,&old_submit,NULL,&lost);
    if(status!=STATUS_DEVICE_NOT_READY) {
        if(status==STATUS_SUCCESS)(VOID)TgGpuCancelCapturedWork(&b->Engine,&lost);
        status=STATUS_DEVICE_DATA_ERROR;goto done;
    }
    b->Progress.lost_context_rejected=1;
    status=TgGpuDdiDestroyContext(b->Context);if(!NT_SUCCESS(status))goto done;b->Context=NULL;
    status=TgGpuDdiCreateContext(b->Device,&context);if(!NT_SUCCESS(status))goto done;b->Context=context.hContext;
    status=map_data(b,b->Context);if(!NT_SUCCESS(status))goto done;
    *s=b->Hardware.Status;s->tested=1;b->Cycle=1;
    seed^=0x70801021u;if(seed==TG_GPU_CLEAR_SENTINEL || ~seed==TG_GPU_CLEAR_SENTINEL)seed^=0x1000;
    status=draw_cycle(b,seed,s);
    if(NT_SUCCESS(status) && (b->Engine.SnapshotUploads!=4 ||
       b->Engine.SnapshotBytes!=b->ExpectedSnapshotBytes ||
       b->Progress.snapshot_rejections!=2 || b->Progress.source_mutations!=6))status=STATUS_DEVICE_DATA_ERROR;
done:
    /* Join before sampling/final cleanup: no concurrent work or callbacks. */
    cleanup=TgGpuStopWorker(&b->Worker);if(!NT_SUCCESS(cleanup) && NT_SUCCESS(status))status=cleanup;
    if(!b->Worker.Thread)snapshot(b,s);
    b->Progress.result=(UINT)status;s->backend=b->Progress;
    s->backend.allocations_remaining=(UINT)b->Allocations;
    cleanup=TgGpuStopDiagnosticBackend(state,s);
    if(!NT_SUCCESS(cleanup) && NT_SUCCESS(status))status=cleanup;
    s->backend.result=(UINT)status;
    if(NT_SUCCESS(status) && !*state)s->backend.stage=TG_GPU_BACKEND_COMPLETE_STAGE;
    s->stage=500+s->backend.stage;
    if(!NT_SUCCESS(status) && !s->execution_result)s->execution_result=TG_GPU_EXEC_GR_CONTEXT;
    return status;
}
#endif
