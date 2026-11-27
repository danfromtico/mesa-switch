/* SPDX-License-Identifier: MIT */
#include "reset_backend.h"

#define TG_GPU_SHUTDOWN_WAIT_100NS (-10000000LL)

/* tg_gpu_power_down proves PMC clamp and CAR reset before releasing the rail.
 * If only that final release failed because its provider is already in D3, a
 * second local readback can finish the one-way isolation without claiming that
 * the rail was released. Power.owned and Status.restore_result remain as the
 * unresolved-rail record; every other failure retains the original status. */
static NTSTATUS shutdown_hardware(TG_GPU_HARDWARE *h)
{
    NTSTATUS status=TgGpuIsolateHardware(h);
    TG_GPU_CONTEXT_ENGINE *e;KIRQL irql;UINT32 clamp,reset;BOOLEAN advance;
    if(NT_SUCCESS(status) || !h || status!=STATUS_DEVICE_DATA_ERROR ||
       (h->Status.restore_result&0xffffu)!=TG_GPU_POWER_PMIC_ERROR ||
       !h->Power.owned || h->Power.ready || !h->GpuOwned || !h->Engine ||
       !h->Io.read32)return status;
    e=h->Engine;
    clamp=h->Io.read32(h->Io.context,0x7000e6d4u);
    reset=h->Io.read32(h->Io.context,0x6000628cu);
    if(clamp==UINT32_MAX || reset==UINT32_MAX || !(clamp&1u) || !(reset&0x01000000u))
        return status;
    advance=!h->Isolated;
    h->Ready=FALSE;h->Isolated=TRUE;
    h->Clock.owned=h->Clock.running=false;h->Execution.owned=false;
    if(advance) {
        KeAcquireSpinLock(&e->Lock,&irql);
        e->UserdBar1Ready=FALSE;
        if(!++e->GraphicsGeneration)++e->GraphicsGeneration;
        KeReleaseSpinLock(&e->Lock,irql);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS isolate_backend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w,BOOLEAN reset,BOOLEAN shutdown)
{
    KIRQL irql,engine_irql;NTSTATUS status;LIST_ENTRY *link;
    PAGED_CODE();
    if(!e || !q || !h || !w || !e->Commands.Handle ||
       (h->Engine && h->Engine!=e) || (q->Engine && q->Engine!=e) ||
       (w->Thread && (w->Queue!=q || w->Hardware!=h)))return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(!reset && !shutdown && q->Engine && (q->Current.Engine || !IsListEmpty(&q->Pending))) {
        KeReleaseSpinLock(&q->Lock,irql);
        return STATUS_DEVICE_BUSY;
    }
    q->Paused=TRUE;
    KeAcquireSpinLock(&e->Lock,&engine_irql);e->Resetting=TRUE;
    if(shutdown)e->ShutdownFinal=TRUE;
    if(reset && !e->RecoveryPending){e->RecoveryPending=TRUE;e->RecoveryStatus=STATUS_IO_TIMEOUT;}
    KeReleaseSpinLock(&e->Lock,engine_irql);KeReleaseSpinLock(&q->Lock,irql);
    /* Never join while holding DispatchGate: a waking worker may be waiting
     * for it. Resetting also suppresses a late worker recovery notification.
     * Terminal teardown never waits forever for a wedged worker or gate. */
    if(shutdown) {
        LARGE_INTEGER timeout={.QuadPart=TG_GPU_SHUTDOWN_WAIT_100NS};
        status=TgGpuStopWorkerEx(w,&timeout);
    } else status=TgGpuStopWorker(w);
    if(!NT_SUCCESS(status))return status;
    if(q->Engine) {
        LARGE_INTEGER timeout={.QuadPart=TG_GPU_SHUTDOWN_WAIT_100NS};
        status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,
            shutdown?&timeout:NULL);
        if(status!=STATUS_SUCCESS)
            return shutdown && NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status;
        if(reset && TgGpuIsFaultLatched(&e->FaultIsr)) {
            TG_GPU_FAULT_ISR_EVENT details={0};
            if(TgGpuCaptureFaultDetails(&e->FaultIsr,&h->Io,&h->Power,&details)) {
                KeAcquireSpinLock(&e->Lock,&engine_irql);
                details.AdapterLuid=e->AdapterLuid;
                e->Channel.fifo_interrupt=details.FifoIntr;
                e->Channel.mmu_fault=details.MmuFaultMask;
                e->Channel.pbdma_interrupt=details.Pbdma0;
                e->Channel.pbdma_interrupt_1=details.Pbdma1;
                KeReleaseSpinLock(&e->Lock,engine_irql);
                TgGpuTraceFaultIsr(&details);
                /* Emit Event 10 v2 for every additional set MMU fault ID (B08). */
                const ULONG mmu_count=TgGpuGetCapturedMmuEntryCount(&e->FaultIsr,(LONG)details.Epoch);
                for(ULONG i=1;i<mmu_count;++i) {
                    TG_GPU_MMU_FAULT_ENTRY mmu={0};
                    if(TgGpuGetCapturedMmuEntry(&e->FaultIsr,(LONG)details.Epoch,i,&mmu)) {
                        TG_GPU_FAULT_ISR_EVENT extra=details;
                        extra.FaultId=mmu.Id;
                        extra.FaultInst=mmu.Inst;
                        extra.FaultAddrLo=mmu.AddrLo;
                        extra.FaultAddrHi=mmu.AddrHi;
                        extra.FaultInfo=mmu.Info;
                        TgGpuTraceFaultIsr(&extra);
                    }
                }
            }
        }
        if(reset)TgGpuTraceResetWork(q,&h->Io,&h->Power);
        status=TgGpuResetWorkQueue(q,&h->Io,&h->Power,&h->Clock);
        if(NT_SUCCESS(status))status=TgGpuStopWorkQueue(q);
        (VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
        if(!NT_SUCCESS(status))return status;
    }
    if(!h->Engine)return h->Isolated?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
    status=shutdown?shutdown_hardware(h):TgGpuIsolateHardware(h);
    if(!NT_SUCCESS(status))return status;
    KeAcquireSpinLock(&e->Lock,&irql);
    for(link=e->Contexts.Flink;link!=&e->Contexts;link=link->Flink) {
        TG_GPU_CONTEXT *c=CONTAINING_RECORD(link,TG_GPU_CONTEXT,Link);
        const BOOLEAN system_paging=c->Flags.SystemContext && c->Flags.VirtualAddressing &&
            !c->Flags.GdiContext && !c->Closing && !c->References &&
            c->Device && !c->Device->Closing && c->Device->Process &&
            !c->Device->Process->Closing && c->Device->Process->Flags.SystemProcess;
        if(shutdown || (reset && !system_paging)) {
            /* TDR may evict roots; terminal shutdown retires every root. */
            c->Root.Valid=FALSE;c->RootStatus=STATUS_DEVICE_NOT_READY;
        } else {
            /* The system paging device survives adapter recovery. Its root
             * is the authoritative OS binding, updated by SetRootPageTable
             * when moved/resized; never revive a failed binding from a saved
             * descriptor. Resetting/RecoveryPending still block execution
             * through OS cleanup and cold startup. Only its private image
             * must be rebuilt from the new hardware generation's template.
             * Suspend retains its existing paging-only behavior. */
            c->GraphicsGeneration=0;
        }
    }
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}

NTSTATUS TgGpuResetBackend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w)
{
    TgGpuTraceExecutionState(e,TgGpuExecutionResetBegin,STATUS_PENDING,0,NULL);
    NTSTATUS status=isolate_backend(e,q,h,w,TRUE,FALSE);
    TgGpuTraceExecutionState(e,TgGpuExecutionResetEnd,status,0,NULL);
    return status;
}

NTSTATUS TgGpuSuspendBackend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w)
{
    if (!e || !e->PagingOnly) return STATUS_NOT_SUPPORTED;
    return isolate_backend(e,q,h,w,FALSE,FALSE);
}

NTSTATUS TgGpuShutdownBackend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w)
{
    return isolate_backend(e,q,h,w,FALSE,TRUE);
}
NTSTATUS TgGpuRestartBackendEx(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w,const struct tg_gpu_power_io *io,
    UCHAR address,const struct tg_gpu_gr_payload *firmware,
    const NTSTATUS *io_status,PVOID gpu_regs,PVOID lic_regs,BOOLEAN preserve_roots)
{
    struct tg_gpu_power_io next_io;KIRQL irql,engine_irql;NTSTATUS status,cleanup;
    BOOLEAN armed=FALSE;LONG armed_epoch=0;
    PAGED_CODE();
    if(!e || !q || !h || !w || !io || !firmware || !e->Resetting || e->ShutdownFinal ||
       !h->Isolated || h->Power.owned || w->Thread || q->Engine ||
       (h->Engine && h->Engine!=e))return STATUS_INVALID_DEVICE_STATE;
    if(h->Status.restore_result)return STATUS_DEVICE_DATA_ERROR;
    next_io=*io; /* io may point into h, which cold StartHardware clears. */
    status=TgGpuStopHardware(h);if(!NT_SUCCESS(status))return status;
    status=TgGpuFreePrivateMemory(&e->Dxgk,&e->Graphics.Memory);
    if(!NT_SUCCESS(status))return status;
    e->Graphics.ImageBytes=0;
    /* Isolation and detached queue/worker are prerequisites for reusing the
     * private command backing. This never initializes an expected fence. */
    RtlZeroMemory(e->Commands.CpuAddress,e->Commands.Bytes);KeMemoryBarrier();
    RtlZeroMemory(&e->Channel,sizeof(e->Channel));
    status=TgGpuStartHardware(h,e,&next_io,address,firmware);
    if(!NT_SUCCESS(status))goto fail;
    if(io_status && !NT_SUCCESS(*io_status)) {
        status=*io_status;
        goto fail;
    }
    status=TgGpuInitializeWorkQueue(q,e);if(!NT_SUCCESS(status))goto fail;
    status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
    if(status!=STATUS_SUCCESS)goto fail;
    /* Validate IoStatus and arm the stall ISR BEFORE starting the worker or
     * releasing RecoveryPending/Resetting, so no submit can race an unarmed
     * or failed restart epoch. */
    if(gpu_regs && lic_regs) {
        status=TgGpuArmFaultIsr(&e->FaultIsr,&e->Dxgk,gpu_regs,lic_regs);
        if(NT_SUCCESS(status)) {
            armed=TRUE;
            armed_epoch=TgGpuCurrentFaultEpoch(&e->FaultIsr);
        }
    }
    if(NT_SUCCESS(status))
        status=TgGpuStartWorker(w,q,h);
    if(NT_SUCCESS(status)) {
        KeAcquireSpinLock(&q->Lock,&irql);KeAcquireSpinLock(&e->Lock,&engine_irql);
        if(armed && !TgGpuFaultEpochHealthy(&e->FaultIsr,armed_epoch)) {
            /* A new stall fault latched between ArmFaultIsr and releasing
             * Resetting (AB06). Keep Resetting/RecoveryPending asserted and
             * fail this restart so the fault cannot be swallowed. */
            status=STATUS_DEVICE_DATA_ERROR;
        } else {
            e->RecoveryPending=e->RecoveryNotified=e->RecoverySending=e->Resetting=FALSE;
            e->RecoveryActiveToken=0;
            e->RecoveryStatus=STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&e->Lock,engine_irql);KeReleaseSpinLock(&q->Lock,irql);
    }
    (VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
    if(NT_SUCCESS(status))return status;
fail:
    if(armed) {
        NTSTATUS disarm=TgGpuDisarmFaultIsr(&e->FaultIsr,&e->Dxgk,
            h->Power.owned && h->Power.ready);
        if(!NT_SUCCESS(disarm))return disarm;
    }
    cleanup=(preserve_roots && e->PagingOnly)?
        TgGpuSuspendBackend(e,q,h,w):
        TgGpuResetBackend(e,q,h,w);
    if(cleanup==STATUS_DEVICE_BUSY)
        cleanup=TgGpuResetBackend(e,q,h,w);
    return NT_SUCCESS(cleanup)?status:cleanup;
}
NTSTATUS TgGpuRestartBackend(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_WORK_QUEUE *q,
    TG_GPU_HARDWARE *h,TG_GPU_WORKER *w,const struct tg_gpu_power_io *io,
    UCHAR address,const struct tg_gpu_gr_payload *firmware)
{
    return TgGpuRestartBackendEx(e,q,h,w,io,address,firmware,NULL,NULL,NULL,FALSE);
}
