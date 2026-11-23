/* SPDX-License-Identifier: MIT */
#include "work_queue.h"
#include "../core/nvk_mme_programs.h"

VOID TgGpuTraceExecutionState(TG_GPU_CONTEXT_ENGINE *e,enum tg_gpu_execution_source source,
    NTSTATUS status,UINT request_fence,const DXGKARG_SUBMITCOMMANDVIRTUAL *submit)
{
    TG_GPU_EXECUTION_EVENT record={.Version=1,.Source=source,.Status=status,.RequestFence=request_fence};
    KIRQL irql,queue_irql;TG_GPU_WORK_QUEUE *q;
    if(!e)return;
    if(submit){
        record.RequestFence=submit->SubmissionFenceId;record.RequestFlags=submit->Flags.Value;
        record.RequestContext=(UINT64)(ULONG_PTR)submit->hContext;
        record.RequestDmaBytes=submit->DmaBufferSize;record.PrivateBytes=submit->DmaBufferPrivateDataSize;
        record.UmdPrivateBytes=submit->DmaBufferUmdPrivateDataSize;
    }
    q=e->QueueOwner;
    if(q)KeAcquireSpinLock(&q->Lock,&queue_irql);
    KeAcquireSpinLock(&e->Lock,&irql);
    record.AdapterLuid=e->AdapterLuid;record.LastCompletedFence=e->LastCompletedFence;
    record.GraphicsGeneration=e->GraphicsGeneration;
    record.QueueFlags=(e->RecoveryPending?32u:0u)|(e->Resetting?64u:0u)|(e->Stopping?128u:0u);
    if(q){
        const TG_GPU_CONTEXT_WORK *w=&q->Current;
        record.Stage=q->TraceStage;record.QueueFailure=q->Failure;
        record.QueueFlags|=(q->Engine?1u:0u)|(q->Paused?2u:0u)|(q->Servicing?4u:0u)|
            (q->Preemptions?8u:0u)|(!IsListEmpty(&q->Pending)?16u:0u);
        record.Context=(UINT64)(ULONG_PTR)w->Context;record.DmaAddress=w->DmaAddress;
        record.SubmissionFence=w->SubmissionFence;record.Expected=w->Expected;
        record.Observed=w->TraceObserved;record.DmaBytes=w->DmaBytes;
        record.WorkFlags=(w->Engine?1u:0u)|(w->Paging?2u:0u)|(w->Software?4u:0u)|
            (w->Preemption?8u:0u)|(w->Rejected?16u:0u)|(w->Prepared?32u:0u)|
            (w->HardwareBusy?64u:0u)|(w->Attempted?128u:0u)|(w->Submitted?256u:0u)|
            (w->Retired?512u:0u)|(w->GpuCompleted?1024u:0u)|(w->CpuCompleted?2048u:0u)|
            (w->CompletionReady?4096u:0u)|(w->Signaling?8192u:0u)|(w->Notified?16384u:0u)|
            (w->TraceObservedValid?32768u:0u)|(w->TraceObservedCpu?65536u:0u)|
            (w->TraceMcValid?131072u:0u)|(w->TraceGrValid?262144u:0u);
        record.PollCount=w->TracePolls;
        record.CachedFifo=w->TraceFifo;record.CachedMmu=w->TraceMmu;
        record.CachedPbdma=w->TracePbdma;record.CachedPbdma1=w->TracePbdma1;
        record.CachedMc=w->TraceMc;record.CachedGr=w->TraceGr;
        record.CachedFecsError=w->TraceFecsError;record.CachedFecsHost=w->TraceFecsHost;
        /* Channel helpers mutate these outside Engine.Lock while HardwareBusy.
         * Do not race those writes; the published worker stage still identifies
         * a blocked helper. Poll caches above are always copied under the lock. */
        if(!w->HardwareBusy && q->Engine==e){
            record.QueueFlags|=256u;
            record.ChannelStage=e->Channel.stage;record.ChannelRegister=e->Channel.last_register;
            record.ChannelValue=e->Channel.last_value;
        }
    }
    TgGpuCacheExecution(&e->DebugCache,&record);
    KeReleaseSpinLock(&e->Lock,irql);
    if(q)KeReleaseSpinLock(&q->Lock,queue_irql);
    TgGpuTraceExecution(&record);
}

static VOID trace_stage(TG_GPU_WORK_QUEUE *q,enum tg_gpu_execution_stage stage)
{
    KIRQL irql;KeAcquireSpinLock(&q->Lock,&irql);q->TraceStage=stage;KeReleaseSpinLock(&q->Lock,irql);
}

VOID TgGpuTraceResetWork(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power)
{
    PAGED_CODE();
    if(!q || !q->Engine || !q->Paused || q->Servicing || !TgGpuTraceResetPacketEnabled())return;
    const TG_GPU_CONTEXT_WORK *w=&q->Current;
    TG_GPU_CONTEXT_ENGINE *e=q->Engine;
    if(w->Engine!=e || !w->Context || w->Paging || w->Software || w->Preemption || w->HardwareBusy)return;
    TG_GPU_RESET_PACKET_EVENT record={.Version=1,.AdapterLuid=e->AdapterLuid,
        .Context=(ULONGLONG)(ULONG_PTR)w->Context,.DmaAddress=w->DmaAddress,
        .SubmissionFence=w->SubmissionFence,.GraphicsGeneration=e->GraphicsGeneration,
        .Expected=w->Expected,.DmaBytes=w->DmaBytes};
    const TG_GPU_COMMAND_SNAPSHOT *snapshot=q->CurrentOwned?q->CurrentOwned->Snapshot:NULL;
    if(snapshot && snapshot->Bytes==w->DmaBytes && snapshot->Bytes &&
       !(snapshot->Bytes&3) && snapshot->Bytes<=TG_GPU_COMMAND_CAPTURE_BYTES)
        record.CommandBytes=snapshot->Bytes;
    tg_gpu_channel_snapshot(io,power,w,&e->Channel,&record.Hardware);
    /* Only the KMD-owned immutable snapshot is read, never the application's
     * VA, borrowed UMD/private bytes, shader memory or mutable GPU mirror.
     * This remains bounded to one 64-KiB packet without allocating memory. */
    do {
        record.ChunkBytes=record.CommandBytes-record.Offset;
        if(record.ChunkBytes>TG_GPU_TRACE_COMMAND_CHUNK_BYTES)
            record.ChunkBytes=TG_GPU_TRACE_COMMAND_CHUNK_BYTES;
        TgGpuTraceResetPacket(&record,record.ChunkBytes?(const UCHAR *)snapshot->Words+record.Offset:NULL);
        record.Offset+=record.ChunkBytes;
    }while(record.Offset<record.CommandBytes);
}

static VOID free_entry(TG_GPU_QUEUE_ENTRY *entry)
{
    TgGpuFreeCommandSnapshot(entry->Snapshot);
    ExFreePoolWithTag(entry,TG_GPU_QUEUE_TAG);
}

NTSTATUS TgGpuInitializeWorkQueue(TG_GPU_WORK_QUEUE *q,TG_GPU_CONTEXT_ENGINE *e)
{
    KIRQL irql;
    if(!q || !e || !e->Commands.Handle) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->QueueOwner || e->Active || e->Notification) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    RtlZeroMemory(q,sizeof(*q));q->Engine=e;
    KeInitializeSpinLock(&q->Lock);InitializeListHead(&q->Pending);
    KeInitializeMutex(&q->DispatchGate,0);
    KeInitializeEvent(&q->Wake,SynchronizationEvent,FALSE);e->QueueOwner=q;
    e->EngineQueryFence=e->LastCompletedFence;
    KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
}
static NTSTATUS paging_range(TG_GPU_CONTEXT_ENGINE *engine,const DXGKARG_SUBMITCOMMANDVIRTUAL *a,
    UINT *offset,HANDLE *system_context)
{
    const TG_GPU_QUEUE_ENTRY *entries=a->pDmaBufferPrivateData;
    const TG_GPU_PAGING_BUFFER *first=&entries->Paging;
    UINT count=a->DmaBufferPrivateDataSize/sizeof(*entries);
    UINT end=first->WriteOffset;

    if(a->NodeOrdinal || a->EngineOrdinal || !count || count>TG_GPU_PAGING_PRIVATE_ENTRIES ||
       a->DmaBufferPrivateDataSize!=count*sizeof(*entries) || !first->CpuAddress ||
       ((ULONG_PTR)first->CpuAddress&7) || first->Capacity>TG_GPU_DMA_BUFFER_BYTES ||
       a->DmaBufferVirtualAddress<first->GpuAddress ||
       a->DmaBufferVirtualAddress-first->GpuAddress!=end)
        return STATUS_INVALID_PARAMETER;

    *offset=end;
    *system_context=NULL;
    for(UINT i=0;i<count;++i) {
        const TG_GPU_PAGING_BUFFER *p=&entries[i].Paging;
        if(p->Magic!=TG_GPU_PAGING_BUFFER_MAGIC || p->Engine!=engine ||
           p->CpuAddress!=first->CpuAddress || p->GpuAddress!=first->GpuAddress ||
           p->Capacity!=first->Capacity || p->WriteOffset!=end ||
           p->BuiltBytes<=end || p->BuiltBytes>p->Capacity || ((end|p->BuiltBytes)&7))
            return STATUS_INVALID_PARAMETER;
        if(p->SystemContext) {
            if(*system_context && *system_context!=p->SystemContext)
                return STATUS_INVALID_PARAMETER;
            *system_context=p->SystemContext;
        }
        end=p->BuiltBytes;
    }
    return end-*offset==a->DmaBufferSize?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
}

/* Caller holds Queue.Lock; the worker uses the same Queue -> Engine order. */
static VOID begin_progress_interval(TG_GPU_WORK_QUEUE *q)
{
    KIRQL irql;
    if(!q->Servicing && !q->Current.Engine && IsListEmpty(&q->Pending)) {
        KeAcquireSpinLock(&q->Engine->Lock,&irql);
        q->Engine->EngineQueryFence=q->Engine->LastCompletedFence;
        KeReleaseSpinLock(&q->Engine->Lock,irql);
    }
}

static NTSTATUS queue_submit(TG_GPU_WORK_QUEUE *q,const DXGKARG_SUBMITCOMMANDVIRTUAL *a,
    TG_GPU_QUEUE_ENTRY *reserved,TG_GPU_COMMAND_SNAPSHOT *snapshot,BOOLEAN *used)
{
    TG_GPU_QUEUE_ENTRY *entry;LIST_ENTRY *l;KIRQL irql,engine_irql;NTSTATUS status;
    TG_GPU_CONTEXT_EVENT context_evidence={0};
    const BOOLEAN context_trace=TgGpuTraceContextEnabled();
    if(used)*used=FALSE;
    if(!q || !q->Engine || !a)return STATUS_INVALID_PARAMETER;
    entry=a->pDmaBufferPrivateData;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Paused || !NT_SUCCESS(q->Failure)) {
        KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_NOT_READY;
    }
    const BOOLEAN gdi_present=a->Flags.Present && TgGpuIsGdiContext(q->Engine,a->hContext);
    /* The scheduler can suppress rendering or submit an empty packet. Retain
     * only its ordered fence in that case. Every real copy requires a snapshot. */
    const BOOLEAN window_present=a->Flags.Present && reserved && !q->Engine->PagingOnly &&
        !gdi_present && !a->Flags.Paging && !a->Flags.ContextSwitch;
    const BOOLEAN null_rendering=reserved && !a->Flags.Paging && !a->Flags.ContextSwitch &&
        a->Flags.NullRendering;
    const UINT allowed=0xc9u|((gdi_present || window_present)?2u:0u);
    if(a->Flags.Value&~allowed){status=STATUS_NOT_SUPPORTED;goto done;}
    /* Null rendering retains only an ordered fence. Its UMD bytes are not
     * command input, and the queue owns the reserved software record. */
    if (window_present || null_rendering) entry=reserved;
    if(reserved && !a->Flags.ContextSwitch && !a->Flags.Paging &&
       !a->Flags.NullRendering && a->DmaBufferSize) {
        if(!snapshot || snapshot->Bytes!=a->DmaBufferSize ||
           snapshot->Bytes>TG_GPU_COMMAND_CAPTURE_BYTES) {status=STATUS_INVALID_PARAMETER;goto rejected;}
        entry=reserved;
    }
    if(reserved && (!a->DmaBufferUmdPrivateDataSize || gdi_present) &&
       (a->Flags.ContextSwitch || (!a->Flags.Paging && (a->Flags.NullRendering || !a->DmaBufferSize))) &&
       (!entry || a->DmaBufferPrivateDataSize<sizeof(*entry)))entry=reserved;
    if(!entry ||
        ((ULONG_PTR)entry&(TYPE_ALIGNMENT(TG_GPU_QUEUE_ENTRY)-1)) ||
        (entry!=reserved && a->DmaBufferPrivateDataSize<sizeof(*entry)) ||
        (a->DmaBufferUmdPrivateDataSize && !snapshot && !gdi_present && !window_present && !null_rendering)) {
        status=STATUS_INVALID_PARAMETER;goto rejected;
    }
    if(q->CurrentPrivate==entry) { KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY; }
    for(l=q->Pending.Flink;l!=&q->Pending;l=l->Flink)
        if(CONTAINING_RECORD(l,TG_GPU_QUEUE_ENTRY,Link)==entry) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY;
        }
    if(a->Flags.ContextSwitch) {
        RtlZeroMemory(entry,sizeof(*entry));
        status=TgGpuCaptureSoftwareWork(q->Engine,a,&entry->Work);
    } else if(a->Flags.Paging) {
        UINT offset;HANDLE system_context;BOOLEAN requires_context;
        status=paging_range(q->Engine,a,&offset,&system_context);
        if(!NT_SUCCESS(status))goto rejected;
        status=TgGpuValidatePagingBuffer(q->Engine->Tables,
            (UCHAR *)entry->Paging.CpuAddress+offset,a->DmaBufferSize,&requires_context);
        if(NT_SUCCESS(status)) {
            KeAcquireSpinLock(&q->Engine->Lock,&engine_irql);
            status=q->Engine->Stopping?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
            if(NT_SUCCESS(status)) {
                RtlZeroMemory(&entry->Work,sizeof(entry->Work));
                entry->Work.Engine=q->Engine;entry->Work.Paging=TRUE;
                entry->Work.SubmissionFence=entry->Work.CompletionFence=a->SubmissionFenceId;
                entry->Work.PagingOffset=offset;entry->Work.PagingBytes=a->DmaBufferSize;
            }
            KeReleaseSpinLock(&q->Engine->Lock,engine_irql);
            if (NT_SUCCESS(status) && requires_context) {
                if (!system_context || (a->hContext && a->hContext!=system_context))
                    status=STATUS_INVALID_PARAMETER;
                else
                    status=TgGpuCapturePagingContext(q->Engine,system_context,&entry->Work,
                        context_trace?&context_evidence:NULL);
            }
        }
    } else {
        RtlZeroMemory(entry,sizeof(*entry));
        status=(a->Flags.ContextSwitch || a->Flags.NullRendering || !a->DmaBufferSize)?
            TgGpuCaptureSoftwareWork(q->Engine,a,&entry->Work):
            TgGpuCaptureContextWork(q->Engine,a,snapshot?&snapshot->Info:NULL,&entry->Work);
        if(NT_SUCCESS(status) && snapshot) {
            entry->Snapshot=snapshot;
            entry->Work.DmaAddress=entry->Work.InternalVa+TG_GPU_COMMAND_CAPTURE;
            if(snapshot->HasIndirect) {
                entry->Work.IndirectAddress=snapshot->Indirect.address;
                entry->Work.IndirectBytes=snapshot->Indirect.bytes;
                entry->Work.IndirectSuffixOffset=snapshot->Indirect.suffix_offset;
            }
        }
    }
    /* A paging buffer is generated by this KMD, so corruption is an internal
     * recovery condition rather than an application-format rejection. */
rejected:
    if(reserved && status==STATUS_INVALID_PARAMETER && a->Flags.Paging)status=STATUS_DEVICE_DATA_ERROR;
    if(status==STATUS_INVALID_PARAMETER && reserved) {
        RtlZeroMemory(reserved,sizeof(*reserved));reserved->Owned=TRUE;
        reserved->Work.Engine=q->Engine;reserved->Work.Software=reserved->Work.Rejected=TRUE;
        reserved->Work.SubmissionFence=reserved->Work.CompletionFence=a->SubmissionFenceId;
        begin_progress_interval(q);
        InsertTailList(&q->Pending,&reserved->Link);*used=TRUE;
    }
    if(NT_SUCCESS(status)) {
        if(entry==reserved){entry->Owned=TRUE;*used=TRUE;}
        begin_progress_interval(q);
        InsertTailList(&q->Pending,&entry->Link);
    }
done:
    KeReleaseSpinLock(&q->Lock,irql);
    if(context_evidence.Version)TgGpuTraceContext(&context_evidence);
    if(NT_SUCCESS(status) || (used && *used)) (VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    return status;
}
NTSTATUS TgGpuQueueSubmit(TG_GPU_WORK_QUEUE *q,const DXGKARG_SUBMITCOMMANDVIRTUAL *a)
{return queue_submit(q,a,NULL,NULL,NULL);}
NTSTATUS TgGpuQueuePreempt(TG_GPU_WORK_QUEUE *q,const DXGKARG_PREEMPTCOMMAND *a,
    TG_GPU_QUEUE_ENTRY *entry)
{
    KIRQL irql,engine_irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !a || !entry || a->NodeOrdinal || a->EngineOrdinal || a->Flags.Value)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(!q->Engine || q->Paused || !NT_SUCCESS(q->Failure))status=STATUS_DEVICE_NOT_READY;
    else {
        KeAcquireSpinLock(&q->Engine->Lock,&engine_irql);
        if(q->Engine->Stopping || q->Engine->RecoveryPending)status=STATUS_DEVICE_NOT_READY;
        KeReleaseSpinLock(&q->Engine->Lock,engine_irql);
        if(NT_SUCCESS(status)) {
            RtlZeroMemory(entry,sizeof(*entry));entry->Owned=TRUE;
            entry->Work.Engine=q->Engine;entry->Work.Preemption=TRUE;
            entry->Work.SubmissionFence=a->PreemptionFenceId;
            begin_progress_interval(q);
            InsertTailList(&q->Pending,&entry->Link);++q->Preemptions;
        }
    }
    KeReleaseSpinLock(&q->Lock,irql);
    if(NT_SUCCESS(status))(VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    return status;
}
NTSTATUS TgGpuQueueSubmitOrReject(TG_GPU_WORK_QUEUE *q,const DXGKARG_SUBMITCOMMANDVIRTUAL *a,
    TG_GPU_QUEUE_ENTRY *reserved,TG_GPU_COMMAND_SNAPSHOT *snapshot,BOOLEAN *used)
{
    if(!reserved || !used)return STATUS_INVALID_PARAMETER;
    return queue_submit(q,a,reserved,snapshot,used);
}
/* DispatchGate held, Current empty. This never touches hardware:
 * any running DMA buffer must first complete/retire through the normal gate.
 * Drop only the prefix before the first preemption marker, not newer work.
 */
static NTSTATUS preempt_prefix(TG_GPU_WORK_QUEUE *q)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_ENGINE *e=q->Engine;
    NTSTATUS status;KIRQL irql;BOOLEAN aborted=FALSE,owned;
    KeAcquireSpinLock(&e->Lock,&irql);
    status=e->Active || e->Notification || e->Channel.referenced?STATUS_PENDING:STATUS_SUCCESS;
    KeReleaseSpinLock(&e->Lock,irql);if(status!=STATUS_SUCCESS)return status;
    for(;;) {
        KeAcquireSpinLock(&q->Lock,&irql);
        if(IsListEmpty(&q->Pending)) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
        }
        entry=CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link);
        if(entry->Work.Preemption) {
            KeReleaseSpinLock(&q->Lock,irql);return STATUS_SUCCESS;
        }
        RemoveEntryList(&entry->Link);KeReleaseSpinLock(&q->Lock,irql);
        if(entry->Work.Rejected && !aborted) {
            KeAcquireSpinLock(&e->Lock,&irql);e->LastCompletedFence=entry->Work.CompletionFence;
            KeReleaseSpinLock(&e->Lock,irql);
        } else if(!entry->Work.Rejected)aborted=TRUE;
        status=entry->Work.Context?TgGpuCancelCapturedWork(e,&entry->Work):STATUS_SUCCESS;
        if(!NT_SUCCESS(status)) {
            KeAcquireSpinLock(&q->Lock,&irql);InsertHeadList(&q->Pending,&entry->Link);
            KeReleaseSpinLock(&q->Lock,irql);return status;
        }
        owned=entry->Owned;
        /* VidSch can resubmit an aborted paging packet. Preserve the KMD's
         * immutable paging descriptor in OS private storage until OS reuse. */
        RtlZeroMemory(&entry->Work,sizeof(entry->Work));InitializeListHead(&entry->Link);
        if(owned)free_entry(entry);
    }
}
static bool check_mapping(void *data, uint64_t base, uint64_t end, enum tg_gpu_pitch_access access)
{
    const TG_GPU_CONTEXT_WORK *w = data;
    return TgGpuCheckPitchRange(w->Engine->Tables, &w->Root, base, end, access) != FALSE;
}

static bool check_draw_cb0(TG_GPU_CONTEXT_WORK *w, uint64_t end)
{
    return end > TG_GPU_NVK_DRAW_CB0_BYTES &&
           check_mapping(w, end - TG_GPU_NVK_DRAW_CB0_BYTES, end, TG_GPU_PITCH_WRITE);
}

static bool check_command_mappings(TG_GPU_CONTEXT_WORK *w, TG_GPU_COMMAND_SNAPSHOT *snapshot)
{
    /* DispatchGate orders paging and retirement. Resolve the next program
     * state from the retired bindings and publish it only at completion.
     */
    if ((snapshot->Info.cb0_end && !check_draw_cb0(w, snapshot->Info.cb0_end)) ||
        (snapshot->Info.requires_cb0 && !check_draw_cb0(w, w->Context->Cb0End)))
        return false;
    for (UINT engine = 0; engine < TG_GPU_PROGRAM_ENGINES; ++engine) {
        for (UINT pool = 0; pool < TG_GPU_TEXTURE_POOLS; ++pool) {
            const struct tg_gpu_texture_pool_range *range = &w->Context->TexturePool[engine][pool];
            if (snapshot->Info.requires_texture_pool[engine][pool] &&
                !check_mapping(w, range->base, range->end, TG_GPU_PITCH_READ_ONLY))
                return false;
        }
    }
    struct tg_gpu_program_state programs = w->Context->Programs;
    if (!tg_gpu_check_stream_mappings(snapshot->Words, snapshot->Bytes,
                                      snapshot->HasIndirect ? &snapshot->Indirect : NULL,
                                      &programs, check_mapping, w))
        return false;
    snapshot->Info.programs = programs;
    return true;
}

NTSTATUS TgGpuServiceWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
                             const struct tg_gpu_power *power)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_WORK *w;KIRQL irql;NTSTATUS status;
    BOOLEAN wake=FALSE;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    status=KeWaitForSingleObject(&q->DispatchGate,Executive,KernelMode,FALSE,NULL);
    if(status!=STATUS_SUCCESS)return NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Servicing) {
        KeReleaseSpinLock(&q->Lock,irql);(VOID)KeReleaseMutex(&q->DispatchGate,FALSE);return STATUS_DEVICE_BUSY;
    }
    if(q->Paused || !NT_SUCCESS(q->Failure)) {
        status=q->Paused ? STATUS_DEVICE_NOT_READY : q->Failure;
        KeReleaseSpinLock(&q->Lock,irql);(VOID)KeReleaseMutex(&q->DispatchGate,FALSE);return status;
    }
    q->Servicing=TRUE;w=&q->Current;
    if(!w->Engine && q->Preemptions) {
        /* Cancel/free one detached record at a time at PASSIVE, rather than
         * holding a spin lock across an arbitrarily long admitted prefix. */
        KeReleaseSpinLock(&q->Lock,irql);
        trace_stage(q,TgGpuExecutionPreemptPrefix);
        status=preempt_prefix(q);
        if(status!=STATUS_SUCCESS)goto done;
        KeAcquireSpinLock(&q->Lock,&irql);
    }
    if(!w->Engine && !IsListEmpty(&q->Pending)) {
        entry=CONTAINING_RECORD(RemoveHeadList(&q->Pending),TG_GPU_QUEUE_ENTRY,Link);
        *w=entry->Work;q->CurrentPrivate=entry;q->CurrentPaging=entry->Paging;
        q->CurrentOwned=entry->Owned?entry:NULL;
        if(w->Preemption){--q->Preemptions;q->CurrentPrivate=NULL;}
        /* Transfer the retained reference to queue-owned storage. Dxgkrnl may
         * reuse its original buffer immediately from the completion callback.
         */
        entry->Work.Context=NULL;
    }
    KeReleaseSpinLock(&q->Lock,irql);
    TG_GPU_COMMAND_SNAPSHOT *snapshot=q->CurrentOwned?q->CurrentOwned->Snapshot:NULL;
    const struct tg_gpu_command_info *info=snapshot?&snapshot->Info:NULL;
    status=STATUS_SUCCESS;
    if(!w->Engine) goto done;
    if(w->Software || w->Preemption) {
        trace_stage(q,TgGpuExecutionSoftware);
        status=TgGpuArmSoftwareCompletion(q->Engine,w);
        if(status!=STATUS_SUCCESS)goto done;
        goto notify;
    }
    if(w->Paging) {
        trace_stage(q,TgGpuExecutionPaging);
        status=TgGpuExecutePagingWork(q->Engine,w,
            (UCHAR *)q->CurrentPaging.CpuAddress+w->PagingOffset,w->PagingBytes,io,power);
        if(status!=STATUS_SUCCESS)goto done;
        goto notify;
    }
    if(w->CompletionReady)goto notify;
    if(!w->Expected) {
        trace_stage(q,TgGpuExecutionMappings);
        if (snapshot && !check_command_mappings(w, snapshot)) {
            status=STATUS_INVALID_ADDRESS;goto done;
        }
        trace_stage(q,TgGpuExecutionActivate);
        status=TgGpuActivateContextWork(q->Engine,w,info);
        if(status==STATUS_DEVICE_BUSY) { status=STATUS_PENDING;goto done; }
        if(!NT_SUCCESS(status)) goto done;
    }
    if(!w->Prepared) {
        trace_stage(q,TgGpuExecutionPrepare);
        status=TgGpuPrepareContextWork(q->Engine,w);
        if(!NT_SUCCESS(status)) goto done;
    }
    if(!w->Attempted) {
        if(snapshot) {
            if(snapshot->Bytes!=w->DmaBytes || snapshot->Bytes>TG_GPU_COMMAND_CAPTURE_BYTES ||
               w->DmaAddress!=w->InternalVa+TG_GPU_COMMAND_CAPTURE ||
               q->Engine->Commands.Bytes!=TG_GPU_COMMAND_BYTES) {
                status=STATUS_INVALID_PARAMETER;goto done;
            }
            /* Prepare cleared control pages, not this region. Only the
             * serialized execution slot can publish captured USER words. */
            RtlZeroMemory((UCHAR *)q->Engine->Commands.CpuAddress+TG_GPU_COMMAND_CAPTURE,TG_GPU_COMMAND_CAPTURE_BYTES);
            RtlCopyMemory((UCHAR *)q->Engine->Commands.CpuAddress+TG_GPU_COMMAND_CAPTURE,snapshot->Words,snapshot->Bytes);
            /* Refresh the data mirror for each execution slot, including
             * packets without QMDs. GPU writeback cannot alter instructions
             * or the immutable snapshot used by mapping validation.
             */
            RtlZeroMemory((UCHAR *)q->Engine->Commands.CpuAddress+TG_GPU_COMMAND_QMD,TG_GPU_COMMAND_QMD_BYTES);
            if(snapshot->Info.has_qmd)
                RtlCopyMemory((UCHAR *)q->Engine->Commands.CpuAddress+TG_GPU_COMMAND_QMD,snapshot->Words,snapshot->Bytes);
            KeMemoryBarrier();++q->Engine->SnapshotUploads;q->Engine->SnapshotBytes+=snapshot->Bytes;
        }
        trace_stage(q,TgGpuExecutionSubmit);
        status=TgGpuSubmitContextWork(q->Engine,w,io,power);
        if(!NT_SUCCESS(status)) goto done;
    }
    trace_stage(q,TgGpuExecutionGpuFence);
    status=TgGpuObserveContextWork(q->Engine,w,io,power);
    if(status!=STATUS_SUCCESS) goto done;
    if(!w->Retired) {
        trace_stage(q,TgGpuExecutionRetire);
        status=TgGpuQuiesceContextWork(q->Engine,w,io,power);
        if(!NT_SUCCESS(status)) goto done;
    }
    trace_stage(q,TgGpuExecutionCpuFence);
    status=TgGpuObserveContextWork(q->Engine,w,io,power);
    if(status!=STATUS_SUCCESS)goto done;
notify:
    trace_stage(q,TgGpuExecutionNotify);
    /* Admission of rejected records uses this same PASSIVE gate. Consume
     * adjacent rejection markers before the real completion callback, so a
     * concurrent QueryCurrentFence never regresses behind those packets. */
    if(!w->Preemption)for(;;) {
        KeAcquireSpinLock(&q->Lock,&irql);
        entry=IsListEmpty(&q->Pending)?NULL:CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link);
        if(!entry || !entry->Work.Rejected) {KeReleaseSpinLock(&q->Lock,irql);break;}
        RemoveEntryList(&entry->Link);w->CompletionFence=entry->Work.CompletionFence;
        KeReleaseSpinLock(&q->Lock,irql);
        free_entry(entry);
    }
    KeAcquireSpinLock(&q->Lock,&irql);q->CurrentPrivate=NULL;KeReleaseSpinLock(&q->Lock,irql);
    status=TgGpuSignalContextWork(q->Engine,w,info);
    if(status!=STATUS_SUCCESS) goto done;
    KeAcquireSpinLock(&q->Lock,&irql);
    RtlZeroMemory(w,sizeof(*w));RtlZeroMemory(&q->CurrentPaging,sizeof(q->CurrentPaging));
    KeReleaseSpinLock(&q->Lock,irql);
    if(q->CurrentOwned) {
        free_entry(q->CurrentOwned);q->CurrentOwned=NULL;
    }
    KeAcquireSpinLock(&q->Lock,&irql);wake=!IsListEmpty(&q->Pending);KeReleaseSpinLock(&q->Lock,irql);
done:
    KeAcquireSpinLock(&q->Lock,&irql);
    if(!NT_SUCCESS(status)) q->Failure=status;
    if(status==STATUS_SUCCESS)q->TraceStage=TgGpuExecutionIdle;
    q->Servicing=FALSE;KeReleaseSpinLock(&q->Lock,irql);
    /* Preserve the original failure before another admission sees only the
     * failed queue after DispatchGate is released. */
    if(!NT_SUCCESS(status))TgGpuTraceExecutionState(q->Engine,TgGpuExecutionWorkerFailure,status,0,NULL);
    if(wake) (VOID)KeSetEvent(&q->Wake,IO_NO_INCREMENT,FALSE);
    (VOID)KeReleaseMutex(&q->DispatchGate,FALSE);
    return status;
}
NTSTATUS TgGpuDrainRejectedIdle(TG_GPU_WORK_QUEUE *q)
{
    KIRQL irql;BOOLEAN idle;
    if(!q || !q->Engine)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    idle=!q->Servicing && !q->Current.Engine && !IsListEmpty(&q->Pending) &&
        CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link)->Work.Rejected;
    KeReleaseSpinLock(&q->Lock,irql);
    /* Kernel dispatcher mutexes are recursive for the owning thread. This
     * step therefore shares the exact worker completion/release path. */
    return idle?TgGpuServiceWorkQueue(q,NULL,NULL):STATUS_SUCCESS;
}
NTSTATUS TgGpuResetWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
                           struct tg_gpu_power *power,struct tg_gpu_clock *clock)
{
    TG_GPU_QUEUE_ENTRY *entry;TG_GPU_CONTEXT_WORK *w;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);q->Paused=TRUE;
    if(q->Servicing) { KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY; }
    q->Servicing=TRUE;w=&q->Current;KeReleaseSpinLock(&q->Lock,irql);
    if(w->CompletionReady && !w->Notified) {
        status=TgGpuCancelContextNotification(q->Engine,w);
        if(!NT_SUCCESS(status)) goto done;
        KeAcquireSpinLock(&q->Lock,&irql);
        RtlZeroMemory(w,sizeof(*w));q->CurrentPrivate=NULL;
        KeReleaseSpinLock(&q->Lock,irql);
    }
    if(w->PagingStarted && !w->CompletionReady) {
        status=TgGpuResetContextWork(q->Engine,w,io,power,clock);
        if(!NT_SUCCESS(status))goto done;
    } else if(w->Context) {
        if(!w->Expected) status=TgGpuCancelCapturedWork(q->Engine,w);
        else if(!w->Attempted) status=TgGpuRetireContextWork(q->Engine,w,NULL,NULL);
        else status=TgGpuResetContextWork(q->Engine,w,io,power,clock);
        if(!NT_SUCCESS(status)) goto done;
    }
    KeAcquireSpinLock(&q->Lock,&irql);
    RtlZeroMemory(w,sizeof(*w));q->CurrentPrivate=NULL;
    RtlZeroMemory(&q->CurrentPaging,sizeof(q->CurrentPaging));
    KeReleaseSpinLock(&q->Lock,irql);
    if(q->CurrentOwned) {
        free_entry(q->CurrentOwned);q->CurrentOwned=NULL;
    }
    /* Admission is paused and this worker is exclusive. Clear each borrowed
     * queue entry before the adapter reports reset/preemption to Dxgkrnl.
     */
    while(!IsListEmpty(&q->Pending)) {
        entry=CONTAINING_RECORD(q->Pending.Flink,TG_GPU_QUEUE_ENTRY,Link);
        status=entry->Work.Context?TgGpuCancelCapturedWork(q->Engine,&entry->Work):STATUS_SUCCESS;
        if(!NT_SUCCESS(status)) goto done;
        BOOLEAN owned=entry->Owned;
        if(entry->Work.Preemption)--q->Preemptions;
        RemoveEntryList(&entry->Link);
        if(owned)free_entry(entry);else RtlZeroMemory(entry,sizeof(*entry));
    }
done:
    KeAcquireSpinLock(&q->Lock,&irql);q->Failure=status;q->Servicing=FALSE;KeReleaseSpinLock(&q->Lock,irql);
    return status;
}
NTSTATUS TgGpuResumeWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power *power)
{
    KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!q || !q->Engine || !power) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&q->Lock,&irql);
    if(q->Servicing || q->Current.Engine || q->Preemptions || !IsListEmpty(&q->Pending) || !NT_SUCCESS(q->Failure))
        status=STATUS_DEVICE_BUSY;
    else if(!power->owned || !power->ready || !q->Engine->UserdBar1Ready || q->Engine->RecoveryPending) status=STATUS_DEVICE_NOT_READY;
    else q->Paused=FALSE;
    KeReleaseSpinLock(&q->Lock,irql);return status;
}
NTSTATUS TgGpuStopWorkQueue(TG_GPU_WORK_QUEUE *q)
{
    KIRQL irql,engine_irql;TG_GPU_CONTEXT_ENGINE *e;
    if(!q || !q->Engine) return STATUS_INVALID_PARAMETER;
    e=q->Engine;KeAcquireSpinLock(&q->Lock,&irql);q->Paused=TRUE;
    if(q->Servicing || q->Current.Engine || q->Preemptions || !IsListEmpty(&q->Pending)) {
        KeReleaseSpinLock(&q->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    KeAcquireSpinLock(&e->Lock,&engine_irql);e->QueueOwner=NULL;KeReleaseSpinLock(&e->Lock,engine_irql);
    q->Engine=NULL;KeReleaseSpinLock(&q->Lock,irql);return STATUS_SUCCESS;
}
