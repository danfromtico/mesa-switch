/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "reset_backend.h"
#include "gr_firmware_io.h"

/* Caller holds Engine.Lock; event publication always follows lock release. */
static VOID tdr_snapshot_locked(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_TDR_EVENT *record)
{
    record->Flags|=TgGpuTdrEngineSnapshot;
    record->LastSubmittedFence=e->LastSubmittedFence;
    record->LastCompletedFence=e->LastCompletedFence;
    if(e->SubmittedFenceValid)record->Flags|=TgGpuTdrSubmittedValid;
    if(e->AdapterIdentityReady){record->AdapterLuid=e->AdapterLuid;record->Flags|=TgGpuTdrIdentityValid;}
    if(e->RecoveryPending)record->Flags|=TgGpuTdrRecoveryPending;
    if(e->Resetting)record->Flags|=TgGpuTdrResetting;
}
static VOID trace_adapter_tdr(TG_GPU_CONTEXT_ENGINE *e,ULONG operation,ULONG phase,NTSTATUS status)
{
    KIRQL irql;
    if(!TgGpuTraceTdrEnabled())return;
    TG_GPU_TDR_EVENT record={.Version=1,.Operation=operation,.Phase=phase,
        .Status=status,.Engine=(ULONGLONG)(ULONG_PTR)e};
    KeAcquireSpinLock(&e->Lock,&irql);tdr_snapshot_locked(e,&record);KeReleaseSpinLock(&e->Lock,irql);
    TgGpuTraceTdr(&record);
}

NTSTATUS APIENTRY TgGpuDdiResetFromTimeout(HANDLE adapter)
{
    TG_GPU_ADAPTER *a=adapter;NTSTATUS status;
    PAGED_CODE();
    if(!a || a->ShutdownFinal || !a->EngineReady || (a->State!=TgGpuAdapterReady && a->State!=TgGpuAdapterResetting))
        return STATUS_INVALID_DEVICE_STATE;
    trace_adapter_tdr(&a->Engine,TgGpuTdrResetAdapter,0,0);
    a->State=TgGpuAdapterResetting;
    status=TgGpuDisarmFaultIsr(&a->Engine.FaultIsr,&a->Engine.Dxgk,
        a->Hardware.Power.owned && a->Hardware.Power.ready);
    if(!NT_SUCCESS(status)) {
        trace_adapter_tdr(&a->Engine,TgGpuTdrResetAdapter,1,status);return status;
    }
    status=TgNvdecAdapterStop(a);
    if(status!=STATUS_SUCCESS) {
        trace_adapter_tdr(&a->Engine,TgGpuTdrResetAdapter,1,status);return status;
    }
    status=TgGpuResetBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    if(status==STATUS_SUCCESS) {
        KIRQL irql;
        /* This is the scheduler's adapter-wide reset boundary. Suspend and
         * generic cleanup share ResetBackend but must not retire its fences.
         * Use the most recent scheduler submission, including ordered
         * format rejection markers and wraparound;
         * preemption identifiers belong to a separate sequence. */
        KeAcquireSpinLock(&a->Engine.Lock,&irql);
        if(a->Engine.SubmittedFenceValid)
            a->Engine.LastCompletedFence=a->Engine.LastSubmittedFence;
        a->Engine.EngineQueryFence=a->Engine.LastCompletedFence;
        KeReleaseSpinLock(&a->Engine.Lock,irql);
    }
    a->QueueReady=a->Queue.Engine==&a->Engine;
    trace_adapter_tdr(&a->Engine,TgGpuTdrResetAdapter,1,status);return status;
}
NTSTATUS TgGpuRestartAdapterHardware(TG_GPU_ADAPTER *a)
{
    TG_GPU_FIRMWARE firmware={0};
    struct tg_gpu_power_io io;UCHAR address=0;NTSTATUS status;
    PVOID gpu_regs=NULL,lic_regs=NULL;
    PAGED_CODE();
    if(!a || a->ShutdownFinal || !a->EngineReady || !a->Engine.Resetting ||
       !a->Platform.Resources || !a->Hardware.Isolated)return STATUS_INVALID_DEVICE_STATE;
    if(a->Resources.Mapping[TgGpuRegisters] && a->Resources.LicMapping &&
       a->Resources.Memory[TgGpuRegisters].u.Memory.Length>=T214_GPU_REG_SIZE &&
       a->Resources.Lic.u.Memory.Length>=T214_GPU_LIC_SIZE) {
        gpu_regs=a->Resources.Mapping[TgGpuRegisters];
        lic_regs=a->Resources.LicMapping;
    }
    /* The OS has completed its cleanup interval. Clear any sticky I/O status
     * from the prior epoch before preflight and cold hardware initialization. */
    a->Platform.IoStatus=STATUS_SUCCESS;
    status=TgGpuPlatformPreflight(&a->Platform,&address);
    if(NT_SUCCESS(status))status=TgGpuFirmwareLoad(&firmware);
    if(NT_SUCCESS(status)) {
        io=TgGpuPlatformIo(&a->Platform);
        status=TgGpuRestartBackendEx(&a->Engine,&a->Queue,&a->Hardware,&a->Worker,
            &io,address,&firmware.payload,&a->Platform.IoStatus,gpu_regs,lic_regs,
            a->State==TgGpuAdapterSuspended && a->Engine.PagingOnly);
    }
    TgGpuFirmwareFree(&firmware);a->QueueReady=a->Queue.Engine==&a->Engine;
    if(NT_SUCCESS(status)) {
        TgGpuDvfsAttach(&a->Hardware,&a->Platform,&a->Queue);
        a->State=TgGpuAdapterReady;
    } else if(!a->Hardware.Power.owned) {
        /* LastAbandonStatus and AbandonFailureCount are recorded on Platform
         * even when an earlier restart error already latched in status (A05). */
        NTSTATUS abandon=TgGpuPlatformAbandonRail(&a->Platform,address);
        if(!NT_SUCCESS(abandon) && NT_SUCCESS(status))status=abandon;
    }
    return status;
}
static BOOLEAN terminal_power_action(POWER_ACTION action)
{
    return action==PowerActionShutdown || action==PowerActionShutdownReset ||
        action==PowerActionShutdownOff;
}
static NTSTATUS terminal_shutdown(TG_GPU_ADAPTER *a)
{
    KIRQL q_irql,e_irql;NTSTATUS status;
    /* Publish the one-way state before any operation that can fail. Resetting
     * closes native admission; no branch below requests recovery or rearms ISR. */
    a->ShutdownFinal=TRUE;a->State=TgGpuAdapterSuspended;
    if(a->QueueReady && a->Queue.Engine==&a->Engine) {
        KeAcquireSpinLock(&a->Queue.Lock,&q_irql);
        a->Queue.Paused=TRUE;
        KeAcquireSpinLock(&a->Engine.Lock,&e_irql);
        a->Engine.Resetting=TRUE;a->Engine.ShutdownFinal=TRUE;
        KeReleaseSpinLock(&a->Engine.Lock,e_irql);
        KeReleaseSpinLock(&a->Queue.Lock,q_irql);
    } else {
        KeAcquireSpinLock(&a->Engine.Lock,&e_irql);
        a->Engine.Resetting=TRUE;a->Engine.ShutdownFinal=TRUE;
        KeReleaseSpinLock(&a->Engine.Lock,e_irql);
    }
    status=TgGpuDisarmFaultIsrEx(&a->Engine.FaultIsr,&a->Engine.Dxgk,
        a->Hardware.Power.owned && a->Hardware.Power.ready,NULL,NULL);
    if(!NT_SUCCESS(status))return status;
    status=TgNvdecAdapterShutdown(a);
    if(status!=STATUS_SUCCESS)return status;
    status=TgGpuShutdownBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    if(a->Engine.Resetting) {
        a->State=TgGpuAdapterSuspended;
        a->QueueReady=a->Queue.Engine==&a->Engine;
    }
    if(NT_SUCCESS(status) && a->Hardware.Isolated && a->Hardware.Power.owned) {
        a->Platform.PreservedGpuIsolated=TRUE;
        TgGpuHandOffUnresolvedRail(&a->Platform,TRUE);
    }
    return status;
}
NTSTATUS TgGpuSetAdapterPowerState(TG_GPU_ADAPTER *a,ULONG uid,
    DEVICE_POWER_STATE state,POWER_ACTION action)
{
    PAGED_CODE();
    if(!a)return STATUS_INVALID_PARAMETER;
    if(uid!=DISPLAY_ADAPTER_HW_ID || !a->EngineReady)return STATUS_SUCCESS;
    if(state==PowerDeviceD0) {
        if(a->ShutdownFinal)return STATUS_INVALID_DEVICE_STATE;
        return a->State==TgGpuAdapterSuspended ? TgGpuRestartAdapterHardware(a) : STATUS_SUCCESS;
    }
    if(state>PowerDeviceD0 && terminal_power_action(action) &&
       (a->State==TgGpuAdapterReady || a->State==TgGpuAdapterSuspended ||
        a->State==TgGpuAdapterResetting))return terminal_shutdown(a);
    if(a->ShutdownFinal)
        return a->Hardware.Isolated?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
    if(a->State!=TgGpuAdapterReady && a->State!=TgGpuAdapterSuspended)return STATUS_SUCCESS;
    if(a->State==TgGpuAdapterSuspended && a->Hardware.Isolated)return STATUS_SUCCESS;
    if(!a->Engine.PagingOnly)return STATUS_NOT_SUPPORTED;
    NTSTATUS status=TgNvdecAdapterStop(a);
    if(status!=STATUS_SUCCESS)return status;

    /* AB06 (1) & v6/v7 P2: Pre-check Queue/Engine spinlocks and ALWAYS probe
     * DispatchGate with ZERO timeout when not busy (never inferring mutex
     * ownership from Queue.Servicing!). Both hardware faults AND prior software
     * RecoveryPending / Queue.Failure hand off recovery without ever entering
     * TgGpuSuspendBackend's infinite KeWaitForSingleObject(..., NULL). */
    BOOLEAN prior_fault_or_failed=FALSE;
    BOOLEAN gate_held=FALSE;
    NTSTATUS recovery_status=STATUS_DEVICE_DATA_ERROR;
    if(a->QueueReady && a->Queue.Engine==&a->Engine) {
        KIRQL q_irql,e_irql;
        KeAcquireSpinLock(&a->Queue.Lock,&q_irql);
        KeAcquireSpinLock(&a->Engine.Lock,&e_irql);
        const BOOLEAN busy=a->Queue.Servicing || a->Queue.Current.Engine!=NULL ||
            !IsListEmpty(&a->Queue.Pending) || a->Engine.Active!=NULL ||
            a->Engine.Notification!=NULL || a->Engine.Channel.referenced;
        const BOOLEAN fault_or_failed=TgGpuIsFaultLatched(&a->Engine.FaultIsr) ||
            a->Engine.RecoveryPending || !NT_SUCCESS(a->Queue.Failure);
        if(busy && !fault_or_failed) {
            KeReleaseSpinLock(&a->Engine.Lock,e_irql);
            KeReleaseSpinLock(&a->Queue.Lock,q_irql);
            return STATUS_DEVICE_BUSY;
        }
        if(fault_or_failed) {
            a->Queue.Paused=TRUE;
            prior_fault_or_failed=TRUE;
            if(!NT_SUCCESS(a->Queue.Failure))
                recovery_status=a->Queue.Failure;
            else if(a->Engine.RecoveryPending && !NT_SUCCESS(a->Engine.RecoveryStatus))
                recovery_status=a->Engine.RecoveryStatus;
        }
        KeReleaseSpinLock(&a->Engine.Lock,e_irql);
        KeReleaseSpinLock(&a->Queue.Lock,q_irql);

        LARGE_INTEGER zero_timeout={.QuadPart=0};
        status=KeWaitForSingleObject(&a->Queue.DispatchGate,Executive,KernelMode,FALSE,&zero_timeout);
        if(status!=STATUS_SUCCESS) {
            gate_held=TRUE;
            KeAcquireSpinLock(&a->Queue.Lock,&q_irql);
            KeAcquireSpinLock(&a->Engine.Lock,&e_irql);
            const BOOLEAN raced_fault=TgGpuIsFaultLatched(&a->Engine.FaultIsr) ||
                a->Engine.RecoveryPending || !NT_SUCCESS(a->Queue.Failure);
            if(raced_fault) {
                a->Queue.Paused=TRUE;
                prior_fault_or_failed=TRUE;
                if(!NT_SUCCESS(a->Queue.Failure))
                    recovery_status=a->Queue.Failure;
                else if(a->Engine.RecoveryPending && !NT_SUCCESS(a->Engine.RecoveryStatus))
                    recovery_status=a->Engine.RecoveryStatus;
            }
            KeReleaseSpinLock(&a->Engine.Lock,e_irql);
            KeReleaseSpinLock(&a->Queue.Lock,q_irql);
            if(!prior_fault_or_failed)return STATUS_DEVICE_BUSY;
        } else {
            KeAcquireSpinLock(&a->Queue.Lock,&q_irql);
            KeAcquireSpinLock(&a->Engine.Lock,&e_irql);
            const BOOLEAN re_busy=a->Queue.Servicing || a->Queue.Current.Engine!=NULL ||
                !IsListEmpty(&a->Queue.Pending) || a->Engine.Active!=NULL ||
                a->Engine.Notification!=NULL || a->Engine.Channel.referenced;
            const BOOLEAN re_fault=TgGpuIsFaultLatched(&a->Engine.FaultIsr) ||
                a->Engine.RecoveryPending || !NT_SUCCESS(a->Queue.Failure);
            if(re_busy && !re_fault && !prior_fault_or_failed) {
                KeReleaseSpinLock(&a->Engine.Lock,e_irql);
                KeReleaseSpinLock(&a->Queue.Lock,q_irql);
                (VOID)KeReleaseMutex(&a->Queue.DispatchGate,FALSE);
                return STATUS_DEVICE_BUSY;
            }
            a->Queue.Paused=TRUE;
            if(re_fault) {
                prior_fault_or_failed=TRUE;
                if(!NT_SUCCESS(a->Queue.Failure))
                    recovery_status=a->Queue.Failure;
                else if(a->Engine.RecoveryPending && !NT_SUCCESS(a->Engine.RecoveryStatus))
                    recovery_status=a->Engine.RecoveryStatus;
            }
            KeReleaseSpinLock(&a->Engine.Lock,e_irql);
            KeReleaseSpinLock(&a->Queue.Lock,q_irql);
            (VOID)KeReleaseMutex(&a->Queue.DispatchGate,FALSE);
        }
    }

    BOOLEAN was_armed=FALSE,had_fault=FALSE;
    NTSTATUS disarm=TgGpuDisarmFaultIsrEx(&a->Engine.FaultIsr,&a->Engine.Dxgk,
        a->Hardware.Power.owned && a->Hardware.Power.ready,&was_armed,&had_fault);
    if(!NT_SUCCESS(disarm)) {
        /* AB06 (2): disarm_sync cleared DpcPending and advanced Epoch while
         * retaining mappings/PendingRestore for retry. Hand off recovery now so
         * a queued fault DPC is never swallowed without a recovery owner. */
        (VOID)TgGpuRequestEngineRecoveryAt(&a->Engine,
            had_fault?STATUS_DEVICE_DATA_ERROR:disarm,
            had_fault?TgGpuRecoveryIsr:TgGpuRecoveryUnspecified,NULL);
        return disarm;
    }
    if(had_fault || prior_fault_or_failed || gate_held) {
        (VOID)TgGpuRequestEngineRecoveryAt(&a->Engine,
            had_fault?STATUS_DEVICE_DATA_ERROR:recovery_status,
            had_fault?TgGpuRecoveryIsr:TgGpuRecoveryUnspecified,NULL);
        return STATUS_DEVICE_BUSY;
    }

    status=TgGpuSuspendBackend(&a->Engine,&a->Queue,&a->Hardware,&a->Worker);
    if(a->Engine.Resetting) {
        a->State=TgGpuAdapterSuspended;
        a->QueueReady=a->Queue.Engine==&a->Engine;
    } else if(!NT_SUCCESS(status)) {
        if(a->QueueReady && a->Queue.Engine==&a->Engine &&
           !TgGpuIsFaultLatched(&a->Engine.FaultIsr) && !a->Engine.RecoveryPending) {
            KIRQL q_irql;
            KeAcquireSpinLock(&a->Queue.Lock,&q_irql);
            if(NT_SUCCESS(a->Queue.Failure))a->Queue.Paused=FALSE;
            KeReleaseSpinLock(&a->Queue.Lock,q_irql);
        }
        if(was_armed && a->Resources.Mapping[TgGpuRegisters] && a->Resources.LicMapping &&
           a->Resources.Memory[TgGpuRegisters].u.Memory.Length>=T214_GPU_REG_SIZE &&
           a->Resources.Lic.u.Memory.Length>=T214_GPU_LIC_SIZE) {
            NTSTATUS rearm=TgGpuArmFaultIsr(&a->Engine.FaultIsr,&a->Engine.Dxgk,
                a->Resources.Mapping[TgGpuRegisters],a->Resources.LicMapping);
            if(!NT_SUCCESS(rearm)) {
                a->State=TgGpuAdapterResetting;
                (VOID)TgGpuRequestEngineRecovery(&a->Engine,rearm);
                return rearm;
            }
        }
    }
    return status;
}
NTSTATUS APIENTRY TgGpuDdiRestartFromTimeout(HANDLE adapter)
{
    TG_GPU_ADAPTER *a=adapter;
    if (!a || a->State!=TgGpuAdapterResetting) return STATUS_INVALID_DEVICE_STATE;
    trace_adapter_tdr(&a->Engine,TgGpuTdrRestartAdapter,0,0);
    NTSTATUS status=TgGpuRestartAdapterHardware(a);
    trace_adapter_tdr(&a->Engine,TgGpuTdrRestartAdapter,1,status);return status;
}
static BOOLEAN engine_idle(TG_GPU_ADAPTER *a,UINT *fence,BOOLEAN *responsive,TG_GPU_TDR_EVENT *record)
{
    KIRQL irql,engine_irql;
    TG_GPU_WORK_QUEUE *q=&a->Queue;
    TG_GPU_CONTEXT_ENGINE *e=&a->Engine;
    if(responsive)*responsive=FALSE;
    if (a->State!=TgGpuAdapterReady || !a->QueueReady) return FALSE;
    KeAcquireSpinLock(&q->Lock,&irql);
    KeAcquireSpinLock(&e->Lock,&engine_irql);
    BOOLEAN healthy=!q->Paused && NT_SUCCESS(q->Failure) &&
        !e->RecoveryPending && !e->Resetting &&
        !TgGpuIsFaultLatched(&e->FaultIsr);
    BOOLEAN idle=!q->Servicing &&
        !q->Current.Engine && IsListEmpty(&q->Pending);
    *fence=e->LastCompletedFence;
    if(record) {
        tdr_snapshot_locked(e,record);
        if(healthy)record->Flags|=TgGpuTdrHealthy;
        if(idle)record->Flags|=TgGpuTdrIdle;
        if(q->Paused)record->Flags|=TgGpuTdrQueuePaused;
        if(!NT_SUCCESS(q->Failure))record->Flags|=TgGpuTdrQueueFailure;
        record->QueueFailure=q->Failure;
        if(q->Servicing)record->Flags|=TgGpuTdrQueueServicing;
        if(!IsListEmpty(&q->Pending))record->Flags|=TgGpuTdrQueuePending;
        if(responsive){record->QuerySampleFence=e->EngineQueryFence;record->Flags|=TgGpuTdrQuerySampleValid;}
    }
    if(responsive) {
        /* Busy is not the same as unresponsive. Report an actual completed
         * packet since the previous sample, or a healthy empty queue. The
         * first admission after idle starts a fresh observation interval.
         * No observed completion is not a claim that the GPU is dead. */
        *responsive=healthy && (idle || *fence!=e->EngineQueryFence);
        e->EngineQueryFence=*fence;
    }
    KeReleaseSpinLock(&e->Lock,engine_irql);
    KeReleaseSpinLock(&q->Lock,irql);
    return healthy && idle;
}
static NTSTATUS APIENTRY query_group(HANDLE adapter,DXGKARG_QUERYDEPENDENTENGINEGROUP *a)
{
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    a->DependentNodeOrdinalMask=1;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY query_engine(HANDLE adapter,DXGKARG_QUERYENGINESTATUS *a)
{
    UINT fence;BOOLEAN responsive;
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    TG_GPU_ADAPTER *d=adapter;BOOLEAN trace=TgGpuTraceTdrEnabled();
    TG_GPU_TDR_EVENT record={.Version=1,.Operation=TgGpuTdrQueryEngine,.Phase=1,
        .Engine=(ULONGLONG)(ULONG_PTR)&d->Engine,.NodeOrdinal=a->NodeOrdinal,.EngineOrdinal=a->EngineOrdinal};
    (VOID)engine_idle(d,&fence,&responsive,trace?&record:NULL);
    a->EngineStatus.Value=responsive?1:0;
    if(trace){record.Result=a->EngineStatus.Value;record.Flags|=TgGpuTdrResultValid;TgGpuTraceTdr(&record);}
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY reset_engine(HANDLE adapter,DXGKARG_RESETENGINE *a)
{
    UINT fence;
    if (!adapter || !a || a->NodeOrdinal || a->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    /* The queue may have drained before the timeout callback arrived.
     * Otherwise GR and CE share the runlist and need the full adapter reset. */
    TG_GPU_ADAPTER *d=adapter;BOOLEAN trace=TgGpuTraceTdrEnabled();
    TG_GPU_TDR_EVENT record={.Version=1,.Operation=TgGpuTdrResetEngine,.Phase=1,
        .Engine=(ULONGLONG)(ULONG_PTR)&d->Engine,.NodeOrdinal=a->NodeOrdinal,.EngineOrdinal=a->EngineOrdinal};
    NTSTATUS status=engine_idle(d,&fence,NULL,trace?&record:NULL)?STATUS_SUCCESS:STATUS_UNSUCCESSFUL;
    if(status==STATUS_SUCCESS){a->LastAbortedFenceId=fence;record.Result=fence;record.Flags|=TgGpuTdrResultValid;}
    if(trace){record.Status=status;TgGpuTraceTdr(&record);}
    return status;
}
static NTSTATUS APIENTRY collect_debug_info(HANDLE adapter,const DXGKARG_COLLECTDBGINFO *a)
{
    TG_GPU_ADAPTER *d=adapter;
    if(!d || !a)return STATUS_UNSUCCESSFUL;
    /* May run during a bugcheck at undefined IRQL. All data is resident:
     * no queue/context dereference, lock wait, allocation, ETW or MMIO here.
     * LastCompletedFence is an independent aligned observation; cached
     * boundaries include their own fence/generation and validity flags. */
    const LONG armed=InterlockedCompareExchange(&d->Engine.FaultIsr.Armed,0,0);
    const LONG faulted=InterlockedCompareExchange(&d->Engine.FaultIsr.Faulted,0,0);
    const LONG unreachable=InterlockedCompareExchange(&d->Engine.FaultIsr.GpuUnreachable,0,0);
    const LONG epoch=InterlockedCompareExchange(&d->Engine.FaultIsr.Epoch,0,0);
    const LONG active_epoch=InterlockedCompareExchange(&d->Engine.FaultIsr.ActiveEpoch,0,0);
    const LONG fault_epoch=InterlockedCompareExchange(&d->Engine.FaultIsr.FaultEpoch,0,0);
    ULONG priv_valid=0u;
    (VOID)TgGpuGetCapturedPrivRing(&d->Engine.FaultIsr,fault_epoch,&priv_valid,NULL,NULL);
    /* Bounded seqlock sample of resident commit-phase tuple (N3): DiagCommitSeq
     * is odd ONLY during the 2-3 instruction resident RAM transitions before and
     * after Io->write32 (never across the hardware write itself), so a genuinely
     * stalled write reports coherent mmio_in_flight while a concurrent completed
     * write can never produce a torn {enter=0, done=1} false positive. */
    ULONG permit=0u,step=0u,last_reg=0u,mmio_enter=0u,mmio_done=0u;
    BOOLEAN commit_coherent=FALSE;
    for(UINT attempt=0u;attempt<3u;++attempt) {
        const LONG seq1=InterlockedCompareExchange(&d->Engine.DiagCommitSeq,0,0);
        if(seq1&1)continue;
        permit=(ULONG)InterlockedCompareExchange(&d->Engine.DiagCommitPermitCount,0,0);
        step=(ULONG)InterlockedCompareExchange(&d->Engine.DiagCommitStep,0,0)&3u;
        last_reg=(ULONG)InterlockedCompareExchange(&d->Engine.DiagLastMmioReg,0,0);
        mmio_enter=(ULONG)InterlockedCompareExchange(&d->Engine.DiagMmioEnterCount,0,0);
        mmio_done=(ULONG)InterlockedCompareExchange(&d->Engine.DiagMmioDoneCount,0,0);
        const LONG seq2=InterlockedCompareExchange(&d->Engine.DiagCommitSeq,0,0);
        if(seq1==seq2 &&
           (mmio_enter==mmio_done || mmio_enter==mmio_done+1u) &&
           (step!=0u || mmio_enter==mmio_done)) {
            commit_coherent=TRUE;
            break;
        }
    }
    const ULONG fault_state=(armed?TgGpuBreadcrumbArmed:0u)|
        (faulted?TgGpuBreadcrumbFaulted:0u)|
        (unreachable?TgGpuBreadcrumbGpuUnreachable:0u)|
        (active_epoch?TgGpuBreadcrumbActiveEpoch:0u)|
        ((commit_coherent?step:0u)<<TgGpuBreadcrumbCommitStepShift)|
        ((commit_coherent && mmio_enter==mmio_done+1u)?TgGpuBreadcrumbMmioInFlight:0u)|
        ((!commit_coherent)?TgGpuBreadcrumbCommitUncertain:0u)|
        ((priv_valid&1u)?TgGpuBreadcrumbPrivSysValid:0u)|
        ((priv_valid&2u)?TgGpuBreadcrumbPrivGpcValid:0u);
    TG_GPU_DIAG_BREADCRUMBS breadcrumbs={
        .Version=2u,
        .WorkerStage=(UCHAR)InterlockedCompareExchange(&d->Engine.DiagWorkerStage,0,0),
        .LastPagingKind=(UCHAR)InterlockedCompareExchange(&d->Engine.DiagLastPagingKind,0,0),
        .FaultState=fault_state,
        .Epoch=(ULONG)epoch,
        .ActiveEpoch=(ULONG)active_epoch,
        .FaultEpoch=(ULONG)fault_epoch,
        .IsrProbeEnterCount=(ULONG)InterlockedCompareExchange(&d->Engine.FaultIsr.IsrProbeEnterCount,0,0),
        .IsrProbeDoneCount=(ULONG)InterlockedCompareExchange(&d->Engine.FaultIsr.IsrProbeDoneCount,0,0),
        .IsrEnterCount=(ULONG)InterlockedCompareExchange(&d->Engine.FaultIsr.IsrEnterCount,0,0),
        .IsrMaskCount=(ULONG)InterlockedCompareExchange(&d->Engine.FaultIsr.IsrMaskCount,0,0),
        .IsrUnreachableCount=(ULONG)InterlockedCompareExchange(&d->Engine.FaultIsr.IsrUnreachableCount,0,0),
        .DpcCount=(ULONG)InterlockedCompareExchange(&d->Engine.FaultIsr.DpcCount,0,0),
        .CaptureCount=(ULONG)InterlockedCompareExchange(&d->Engine.FaultIsr.CaptureCount,0,0),
        .CommitPermitCount=permit,
        .LastMmioReg=last_reg,
        .MmioEnterCount=mmio_enter,
        .MmioDoneCount=mmio_done,
    };
    return TgGpuCopyDebugReportEx(&d->Engine.DebugCache,d->Engine.LastCompletedFence,
        a->Reason,&breadcrumbs,a->pBuffer,a->BufferSize);
}
void TgGpuRegisterResetDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiResetFromTimeout=TgGpuDdiResetFromTimeout;
    d->DxgkDdiRestartFromTimeout=TgGpuDdiRestartFromTimeout;
    d->DxgkDdiQueryDependentEngineGroup=query_group;
    d->DxgkDdiQueryEngineStatus=query_engine;
    d->DxgkDdiResetEngine=reset_engine;
    d->DxgkDdiCollectDbgInfo=collect_debug_info;
}
