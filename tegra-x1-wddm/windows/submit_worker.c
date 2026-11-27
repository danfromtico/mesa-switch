/* SPDX-License-Identifier: MIT */
#include "context.h"
#include "../core/fault.h"

struct fault_guard {
    TG_GPU_CONTEXT_ENGINE *engine;
    LONG epoch;
};

static bool fault_abort_cb(void *ctx)
{
    const struct fault_guard *g=ctx;
    return !TgGpuFaultEpochHealthy(&g->engine->FaultIsr,g->epoch);
}

static bool fault_acquire_cb(void *ctx)
{
    struct fault_guard *g=ctx;
    TG_GPU_CONTEXT_ENGINE *e=g->engine;
    return TgGpuFaultIsrAcquireCommit(&e->FaultIsr,&e->Dxgk,g->epoch,
        &e->DiagCommitSeq,&e->DiagCommitPermitCount,&e->DiagCommitStep,&e->DiagLastMmioReg)!=FALSE;
}

static void fault_write32_cb(void *ctx,const struct tg_gpu_power_io *io,
    uint32_t reg,uint32_t val,bool final_step)
{
    struct fault_guard *g=ctx;
    TG_GPU_CONTEXT_ENGINE *e=g->engine;
    TgGpuFaultIsrTrackedWrite32(io,reg,val,final_step?TRUE:FALSE,
        &e->DiagCommitSeq,&e->DiagLastMmioReg,&e->DiagMmioEnterCount,
        &e->DiagMmioDoneCount,&e->DiagCommitStep);
}

static BOOLEAN fault_latched_or_stale(const TG_GPU_FAULT_ISR *isr,LONG epoch,
    struct tg_gpu_fault_status *faults)
{
    if(TgGpuFaultEpochHealthy(isr,epoch))return FALSE;
    (VOID)TgGpuReadFaultIsr(isr,faults);
    return TRUE;
}

static NTSTATUS channel_status(enum tg_gpu_channel_result result)
{
    switch(result) {
    case TG_GPU_CHANNEL_OK:return STATUS_SUCCESS;
    case TG_GPU_CHANNEL_ARGUMENT:return STATUS_INVALID_PARAMETER;
    case TG_GPU_CHANNEL_NOT_READY:return STATUS_DEVICE_NOT_READY;
    case TG_GPU_CHANNEL_BUSY:return STATUS_DEVICE_BUSY;
    case TG_GPU_CHANNEL_TLB:
    case TG_GPU_CHANNEL_TIMEOUT:
    case TG_GPU_CHANNEL_FLUSH:return STATUS_IO_TIMEOUT;
    default:return STATUS_DEVICE_DATA_ERROR;
    }
}
static void drop_work(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w)
{
    if(w->Context)--w->Context->References;
    e->Active=NULL;w->Context=NULL;w->Submitted=FALSE;
}
NTSTATUS TgGpuObserveContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    KIRQL irql;NTSTATUS status;UINT expected,offset,observed=0,mc=0;
    BOOLEAN retired,sampled=FALSE,cpu_sample=FALSE,mc_sample=FALSE,gr_sample=FALSE;
    struct tg_gpu_fault_status faults;
    struct tg_gpu_gr_status gr={0};
    if(!e || !w || !io || !io->read32 || !io->write32 || !io->delay_us ||
       !power || !power->owned || !power->ready)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Active!=w || !w->Context || !w->Submitted || w->HardwareBusy || !e->UserdBar1Ready) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    offset=e->UserdBar1Offset;
    if((offset&0xfff) || offset>=0x01000000) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_ADDRESS;
    }
    retired=w->Retired;
    if(retired && !w->GpuCompleted) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    expected=w->Expected;const LONG fault_epoch=w->FaultEpoch;
    w->HardwareBusy=TRUE;KeReleaseSpinLock(&e->Lock,irql);
    status=STATUS_PENDING;
    faults=(struct tg_gpu_fault_status){0};
    struct tg_gpu_fault_status isr_faults={0};
    struct fault_guard guard_ctx={.engine=e,.epoch=fault_epoch};
    const struct tg_gpu_commit_guard guard={
        .abort=fault_abort_cb,.acquire=fault_acquire_cb,.write32=fault_write32_cb,.context=&guard_ctx
    };
    if(fault_latched_or_stale(&e->FaultIsr,fault_epoch,&faults) ||
       tg_gpu_fault_poll_checked(io,&faults,&guard) ||
       fault_latched_or_stale(&e->FaultIsr,fault_epoch,&isr_faults)) {
        if(isr_faults.fifo)faults.fifo=isr_faults.fifo;
        if(isr_faults.mmu)faults.mmu=isr_faults.mmu;
        if(isr_faults.pbdma)faults.pbdma=isr_faults.pbdma;
        if(isr_faults.pbdma1)faults.pbdma1=isr_faults.pbdma1;
        status=STATUS_DEVICE_DATA_ERROR;
    } else {
        mc=io->read32(io->context,0x57000200);
        mc_sample=TRUE;
        if(mc==UINT32_MAX || fault_latched_or_stale(&e->FaultIsr,fault_epoch,&faults))
            status=STATUS_DEVICE_DATA_ERROR;
        else if(mc&0x1000) {
            if(!tg_gpu_gr_snapshot_checked(io,&gr,&guard)) {
                (VOID)fault_latched_or_stale(&e->FaultIsr,fault_epoch,&faults);
                status=STATUS_DEVICE_DATA_ERROR;
            } else {
                gr_sample=TRUE;
                if(gr.gr_interrupt || gr.fecs_error_mailbox || gr.fecs_host_interrupt)
                    status=STATUS_DEVICE_DATA_ERROR;
            }
        }
        if(status==STATUS_PENDING && fault_latched_or_stale(&e->FaultIsr,fault_epoch,&faults))
            status=STATUS_DEVICE_DATA_ERROR;
        if(status==STATUS_PENDING) {
            observed=retired?expected:
                io->read32(io->context,0x58000000u+offset+TG_GPU_COMMAND_FENCE-TG_GPU_COMMAND_USERD);
            sampled=!retired;
            if(fault_latched_or_stale(&e->FaultIsr,fault_epoch,&faults) || observed==UINT32_MAX)
                status=STATUS_DEVICE_DATA_ERROR;
            else if(observed==expected) {
                if(fault_latched_or_stale(&e->FaultIsr,fault_epoch,&faults) ||
                   tg_gpu_frontend_sync_memory_checked(io,power,&e->Channel.frontend,&guard)!=TG_GPU_FE_OK)
                    status=STATUS_DEVICE_DATA_ERROR;
                else if(fault_latched_or_stale(&e->FaultIsr,fault_epoch,&faults))
                    status=STATUS_DEVICE_DATA_ERROR;
                else {
                    TgGpuDataSyncBarrier();
                    if(retired){
                        observed=*(volatile UINT32 *)((UCHAR *)e->Commands.CpuAddress+TG_GPU_COMMAND_FENCE);
                        sampled=cpu_sample=TRUE;
                    }
                    status=observed==expected?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR;
                }
            }
        }
    }
    KeAcquireSpinLock(&e->Lock,&irql);w->HardwareBusy=FALSE;
    ++w->TracePolls;
    if(sampled){w->TraceObserved=observed;w->TraceObservedValid=TRUE;w->TraceObservedCpu=cpu_sample;}
    w->TraceFifo=faults.fifo;w->TraceMmu=faults.mmu;
    w->TracePbdma=faults.pbdma;w->TracePbdma1=faults.pbdma1;
    w->TraceMc=mc;w->TraceGr=gr.gr_interrupt;
    w->TraceFecsError=gr.fecs_error_mailbox;w->TraceFecsHost=gr.fecs_host_interrupt;
    w->TraceMcValid=mc_sample;w->TraceGrValid=gr_sample;
    if(status==STATUS_SUCCESS) {
        if(retired)w->CpuCompleted=TRUE;else w->GpuCompleted=TRUE;
    }
    e->Channel.fifo_interrupt=faults.fifo;e->Channel.mmu_fault=faults.mmu;
    e->Channel.pbdma_interrupt=faults.pbdma;e->Channel.pbdma_interrupt_1=faults.pbdma1;
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
NTSTATUS TgGpuSubmitContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    KIRQL irql;enum tg_gpu_channel_result result;UINT64 instance,runlist;
    if(!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Active!=w || !w->Context || !w->Prepared || w->Attempted || w->Signaling || w->HardwareBusy) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    if(!e->UserdBar1Ready) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    w->HardwareBusy=TRUE;w->Attempted=TRUE;
    const LONG fault_epoch=TgGpuCurrentFaultEpoch(&e->FaultIsr);
    w->FaultEpoch=fault_epoch;
    instance=(UINT64)w->Context->Instance.SystemPhysicalAddress.QuadPart;
    runlist=(UINT64)e->Commands.SystemPhysicalAddress.QuadPart+TG_GPU_COMMAND_RUNLIST;
    const UINT gp_put=w->GpPut?w->GpPut:2u;
    KeReleaseSpinLock(&e->Lock,irql);
    /* All waits/MMIO run outside the object spin lock. References remain held
     * even when a partial bind fails or a teardown request races this worker.
     */
    struct tg_gpu_fault_status isr_faults={0};
    struct fault_guard guard_ctx={.engine=e,.epoch=fault_epoch};
    const struct tg_gpu_commit_guard guard={
        .abort=fault_abort_cb,.acquire=fault_acquire_cb,.write32=fault_write32_cb,.context=&guard_ctx
    };
    BOOLEAN faulted=fault_latched_or_stale(&e->FaultIsr,fault_epoch,&isr_faults);
    if(faulted) {
        result=TG_GPU_CHANNEL_FAULT;
    } else {
        result=tg_gpu_channel_submit_entries_checked(io,power,w->Root.DeviceAddress,instance,runlist,
                                                     e->UserdBar1Offset,gp_put,w,&e->Channel,&guard);
        faulted=fault_latched_or_stale(&e->FaultIsr,fault_epoch,&isr_faults);
    }
    KeAcquireSpinLock(&e->Lock,&irql);
    if(faulted) {
        if(isr_faults.fifo)e->Channel.fifo_interrupt=isr_faults.fifo;
        if(isr_faults.mmu)e->Channel.mmu_fault=isr_faults.mmu;
        if(isr_faults.pbdma)e->Channel.pbdma_interrupt=isr_faults.pbdma;
        if(isr_faults.pbdma1)e->Channel.pbdma_interrupt_1=isr_faults.pbdma1;
        result=TG_GPU_CHANNEL_FAULT;
    }
    w->Submitted=result==TG_GPU_CHANNEL_OK;w->HardwareBusy=FALSE;
    KeReleaseSpinLock(&e->Lock,irql);return channel_status(result);
}
static NTSTATUS retire(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const struct tg_gpu_power_io *io,const struct tg_gpu_power *power,BOOLEAN release)
{
    KIRQL irql;enum tg_gpu_channel_result result;
    if(!e || !w) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(release && w->Engine==e && w->Notified && !w->Context) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
    }
    if(e->Active!=w || !w->Context) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER; }
    if(w->HardwareBusy || w->Signaling) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY; }
    const LONG fault_epoch=w->FaultEpoch;
    w->HardwareBusy=TRUE;KeReleaseSpinLock(&e->Lock,irql);
    struct fault_guard guard_ctx={.engine=e,.epoch=fault_epoch};
    const struct tg_gpu_commit_guard guard={
        .abort=fault_abort_cb,.acquire=fault_acquire_cb,.write32=fault_write32_cb,.context=&guard_ctx
    };
    result=tg_gpu_channel_retire_checked(io,power,w,&e->Channel,
        (io && power) ? &guard : NULL);
    KeAcquireSpinLock(&e->Lock,&irql);w->HardwareBusy=FALSE;
    if(result==TG_GPU_CHANNEL_OK) {
        w->Retired=TRUE;
        if(release) drop_work(e,w);
    }
    KeReleaseSpinLock(&e->Lock,irql);return channel_status(result);
}
NTSTATUS TgGpuQuiesceContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{ return retire(e,w,io,power,FALSE); }
NTSTATUS TgGpuRetireContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{ return retire(e,w,io,power,TRUE); }
NTSTATUS TgGpuResetContextWork(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_CONTEXT_WORK *w,
    const struct tg_gpu_power_io *io,struct tg_gpu_power *power,struct tg_gpu_clock *clock)
{
    KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    enum tg_gpu_power_result pr;enum tg_gpu_clock_result cr;
    UINT32 clamp,reset;
    if(!e || !w || !power || !clock || !io || !io->read32 || !io->write32 ||
        !io->delay_us || !io->read_pmic || !io->write_pmic) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Active!=w || (!w->Context && !w->Paging) || (e->Channel.referenced &&
        (e->Channel.owner!=w || e->Channel.power!=power))) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER;
    }
    if(w->HardwareBusy || w->Signaling) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY; }
    w->HardwareBusy=TRUE;w->Submitted=FALSE;KeReleaseSpinLock(&e->Lock,irql);
    tg_gpu_channel_disable(io,power);
    if(power->owned && power->ready && tg_gpu_power_drain(io,power)!=TG_GPU_POWER_OK) {
        status=STATUS_IO_TIMEOUT;goto done;
    }
    cr=tg_gpu_clock_stop(io,clock);
    pr=tg_gpu_power_down(io,power);
    if(pr!=TG_GPU_POWER_OK || power->owned) { status=STATUS_DEVICE_DATA_ERROR;goto done; }
    /* A successful software return alone is not the isolation proof. These
     * two hardware states were also required when acquiring the cold GPU.
     */
    clamp=io->read32(io->context,0x7000e6d4);
    reset=io->read32(io->context,0x6000628c);
    if(clamp==UINT32_MAX || reset==UINT32_MAX || !(clamp&1) || !(reset&0x01000000)) {
        power->owned=true;power->ready=false;
        status=STATUS_DEVICE_DATA_ERROR;goto done;
    }
    /* Isolation retires DMA even if clock restoration failed. Keep that
     * failure's saved clock ownership so a new start cannot silently reuse it.
     */
    clock->running=false;
    KeAcquireSpinLock(&e->Lock,&irql);
    e->UserdBar1Ready=FALSE;
    /* Existing graphics images belong to the lost hardware epoch. Vulkan
     * must recreate those contexts; never silently replay a possibly damaged
     * saved image after full reset. The immutable cold template is retained.
     */
    if(!++e->GraphicsGeneration)++e->GraphicsGeneration;
    RtlZeroMemory(&e->Channel,sizeof(e->Channel));drop_work(e,w);
    KeReleaseSpinLock(&e->Lock,irql);
    if(cr!=TG_GPU_CLOCK_OK) status=STATUS_DEVICE_DATA_ERROR;
done:
    KeAcquireSpinLock(&e->Lock,&irql);w->HardwareBusy=FALSE;KeReleaseSpinLock(&e->Lock,irql);
    return status;
}
