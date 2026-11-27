/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_DEBUG_REPORT_H
#define TG_GPU_DEBUG_REPORT_H
#include "diagnostic_events.h"

/* Writers already hold Engine.Lock. CollectDbgInfo may interrupt that writer
 * at arbitrary IRQL, so the reader never acquires a lock or retries. Atomic
 * words avoid torn payload accesses; an odd/changed sequence discards the
 * whole record. Each record is coherent, not an atomic pair or a live GPU
 * snapshot. Only the existing recovery/execution trace boundaries publish. */
typedef struct _TG_GPU_DEBUG_CACHE {
    volatile LONG RecoverySequence,ExecutionSequence;
    volatile LONG RecoveryWords[sizeof(TG_GPU_RECOVERY_EVENT)/sizeof(LONG)];
    volatile LONG ExecutionWords[sizeof(TG_GPU_EXECUTION_EVENT)/sizeof(LONG)];
} TG_GPU_DEBUG_CACHE;

enum tg_gpu_debug_flags {
    TgGpuDebugRecoveryValid=1, TgGpuDebugExecutionValid=2, TgGpuDebugTruncated=4,
    TgGpuDebugRecoveryBusy=8, TgGpuDebugExecutionBusy=16, TgGpuDebugBreadcrumbsValid=32
};
typedef struct _TG_GPU_DEBUG_REPORT {
    ULONG Signature,Version,ReportBytes,WrittenBytes;
    ULONG Reason,Flags,LastCompletedFence,Reserved;
    TG_GPU_RECOVERY_EVENT Recovery;
    TG_GPU_EXECUTION_EVENT Execution;
} TG_GPU_DEBUG_REPORT;
C_ASSERT(sizeof(TG_GPU_DEBUG_REPORT)==256);

/* Optional 64-byte resident nonpaged breadcrumb trailer appended by CollectDbgInfo
 * when the OS buffer holds >= 320 bytes (256-byte base + 64-byte v2 trailer, N3).
 * FaultState bits: bit0=Armed, bit1=Faulted, bit2=GpuUnreachable, bit3=ActiveEpochValid,
 * bits4..5=CommitStep(0=none,1=permitted_pre_mmio,2=after_reg0,3=commit_done),
 * bit6=MmioInFlight (MmioEnterCount != MmioDoneCount), bit8=PrivSysValid,
 * bit9=PrivGpcValid. Safe at arbitrary IRQL without locks or MMIO. */
enum tg_gpu_breadcrumb_fault_state {
    TgGpuBreadcrumbArmed           = 0x001u,
    TgGpuBreadcrumbFaulted         = 0x002u,
    TgGpuBreadcrumbGpuUnreachable  = 0x004u,
    TgGpuBreadcrumbActiveEpoch     = 0x008u,
    TgGpuBreadcrumbCommitStepMask  = 0x030u,
    TgGpuBreadcrumbCommitStepShift = 4u,
    TgGpuBreadcrumbMmioInFlight    = 0x040u,
    TgGpuBreadcrumbCommitUncertain = 0x080u,
    TgGpuBreadcrumbPrivSysValid    = 0x100u,
    TgGpuBreadcrumbPrivGpcValid    = 0x200u,
};
typedef struct _TG_GPU_DIAG_BREADCRUMBS {
    USHORT Version;
    UCHAR WorkerStage;
    UCHAR LastPagingKind;
    ULONG FaultState;
    ULONG Epoch;
    ULONG ActiveEpoch;
    ULONG FaultEpoch;
    ULONG IsrProbeEnterCount;
    ULONG IsrProbeDoneCount;
    ULONG IsrEnterCount;
    ULONG IsrMaskCount;
    ULONG IsrUnreachableCount;
    ULONG DpcCount;
    ULONG CaptureCount;
    ULONG CommitPermitCount;
    ULONG LastMmioReg;
    ULONG MmioEnterCount;
    ULONG MmioDoneCount;
} TG_GPU_DIAG_BREADCRUMBS;
C_ASSERT(sizeof(TG_GPU_DIAG_BREADCRUMBS)==64);
#define TG_GPU_DEBUG_SIGNATURE 0x42444754u /* "TGDB", little endian */
#define TG_GPU_DEBUG_HEADER_BYTES FIELD_OFFSET(TG_GPU_DEBUG_REPORT,Recovery)

static inline VOID TgGpuPublishDebugRecord(volatile LONG *sequence,volatile LONG *words,
    const VOID *record,ULONG bytes)
{
    ULONG next=((ULONG)InterlockedCompareExchange(sequence,0,0)&~1u)+2u;
    if(!next)next=2; /* zero means never published */
    (VOID)InterlockedExchange(sequence,(LONG)(next-1));
    for(ULONG i=0;i<bytes/sizeof(LONG);++i) {
        LONG word;RtlCopyMemory(&word,(const UCHAR *)record+i*sizeof(word),sizeof(word));
        (VOID)InterlockedExchange(&words[i],word);
    }
    (VOID)InterlockedExchange(sequence,(LONG)next);
}
static inline VOID TgGpuCacheRecovery(TG_GPU_DEBUG_CACHE *cache,const TG_GPU_RECOVERY_EVENT *record)
{TgGpuPublishDebugRecord(&cache->RecoverySequence,cache->RecoveryWords,record,sizeof(*record));}
static inline VOID TgGpuCacheExecution(TG_GPU_DEBUG_CACHE *cache,const TG_GPU_EXECUTION_EVENT *record)
{TgGpuPublishDebugRecord(&cache->ExecutionSequence,cache->ExecutionWords,record,sizeof(*record));}

static inline VOID TgGpuInvalidateDebugRecord(volatile LONG *sequence)
{
    ULONG old=(ULONG)InterlockedCompareExchange(sequence,0,0);
    /* Preserve zero for an owner that has never published. Once used, never
     * reset the generation to zero: a reader can span stop/start and a fresh
     * publication. Reusing its old even sequence would validate mixed data. */
    if(old && !(old&1u))(VOID)InterlockedExchange(sequence,(LONG)(old+1u));
}
/* Stop has joined all publishers. Resident words and their sequence lifetime
 * survive engine initialization; an invalidated record stays unavailable
 * until a new publication advances to the next even sequence. */
static inline VOID TgGpuResetDebugCache(TG_GPU_DEBUG_CACHE *cache)
{
    TgGpuInvalidateDebugRecord(&cache->RecoverySequence);
    TgGpuInvalidateDebugRecord(&cache->ExecutionSequence);
}

/* 0 = never published, 1 = coherent copy, 2 = publication/reinitialization unavailable. */
static inline ULONG TgGpuReadDebugRecord(volatile LONG *sequence,volatile LONG *words,
    VOID *record,ULONG bytes)
{
    LONG before=InterlockedCompareExchange(sequence,0,0);
    if(!before)return 0;
    if(before&1)return 2;
    for(ULONG i=0;i<bytes/sizeof(LONG);++i) {
        LONG word=InterlockedCompareExchange(&words[i],0,0);
        RtlCopyMemory((UCHAR *)record+i*sizeof(word),&word,sizeof(word));
    }
    if(before!=InterlockedCompareExchange(sequence,0,0)) {
        RtlZeroMemory(record,bytes);
        return 2;
    }
    return 1;
}
static inline NTSTATUS TgGpuCopyDebugReportEx(TG_GPU_DEBUG_CACHE *cache,ULONG fence,
    ULONG reason,const TG_GPU_DIAG_BREADCRUMBS *breadcrumbs,VOID *buffer,SIZE_T capacity)
{
    TG_GPU_DEBUG_REPORT report={.Signature=TG_GPU_DEBUG_SIGNATURE,.Version=1,
        .ReportBytes=sizeof(report),.Reason=reason,.LastCompletedFence=fence};
    if(!cache || !buffer || capacity<TG_GPU_DEBUG_HEADER_BYTES)return STATUS_UNSUCCESSFUL;
    const BOOLEAN append_breadcrumbs=breadcrumbs &&
        capacity>=sizeof(report)+sizeof(*breadcrumbs);
    const SIZE_T base_bytes=capacity<sizeof(report)?capacity:sizeof(report);
    if(append_breadcrumbs) {
        report.ReportBytes=(ULONG)(sizeof(report)+sizeof(*breadcrumbs));
        report.WrittenBytes=report.ReportBytes;
        report.Flags|=TgGpuDebugBreadcrumbsValid;
    } else {
        report.WrittenBytes=(ULONG)base_bytes;
    }
    if(base_bytes<sizeof(report))report.Flags|=TgGpuDebugTruncated;
    /* Do not claim a valid payload that the caller's buffer cannot hold. */
    if(base_bytes>=FIELD_OFFSET(TG_GPU_DEBUG_REPORT,Execution)) {
        ULONG state=TgGpuReadDebugRecord(&cache->RecoverySequence,cache->RecoveryWords,
            &report.Recovery,sizeof(report.Recovery));
        if(state==1)report.Flags|=TgGpuDebugRecoveryValid;
        else if(state==2)report.Flags|=TgGpuDebugRecoveryBusy;
    }
    if(base_bytes==sizeof(report)) {
        ULONG state=TgGpuReadDebugRecord(&cache->ExecutionSequence,cache->ExecutionWords,
            &report.Execution,sizeof(report.Execution));
        if(state==1)report.Flags|=TgGpuDebugExecutionValid;
        else if(state==2)report.Flags|=TgGpuDebugExecutionBusy;
    }
    RtlCopyMemory(buffer,&report,base_bytes);
    if(append_breadcrumbs)
        RtlCopyMemory((UCHAR *)buffer+sizeof(report),breadcrumbs,sizeof(*breadcrumbs));
    return STATUS_SUCCESS;
}
static inline NTSTATUS TgGpuCopyDebugReport(TG_GPU_DEBUG_CACHE *cache,ULONG fence,
    ULONG reason,VOID *buffer,SIZE_T capacity)
{
    return TgGpuCopyDebugReportEx(cache,fence,reason,NULL,buffer,capacity);
}
#endif
