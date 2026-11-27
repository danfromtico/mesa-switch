/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_DIAGNOSTIC_EVENTS_H
#define TG_GPU_DIAGNOSTIC_EVENTS_H
#include <ntddk.h>
#include "../core/channel.h"

/* Provider c27c1f9b-62e8-4e21-b29b-6bb2d585c6d0, event 1/version 1.
 * Fixed-width payload; no referenced command data or pointer dereferences. */
enum tg_gpu_recovery_source {
    TgGpuRecoveryUnspecified, TgGpuRecoverySubmitGate, TgGpuRecoverySubmit,
    TgGpuRecoveryRejectDrain, TgGpuRecoveryPreempt, TgGpuRecoveryWorker,
    TgGpuRecoveryLegacySubmit, TgGpuRecoveryIsr
};
typedef struct _TG_GPU_RECOVERY_EVENT {
    ULONG Version,Source;
    LUID AdapterLuid;
    NTSTATUS Status;
    ULONG Flags;
    ULONGLONG Context;
    ULONG DmaBytes,PrivateBytes,UmdPrivateBytes,SubmissionFence;
    ULONG LastCompletedFence,GraphicsGeneration;
    ULONG CachedFifo,CachedMmu,CachedPbdma,CachedPbdma1;
} TG_GPU_RECOVERY_EVENT;
C_ASSERT(sizeof(TG_GPU_RECOVERY_EVENT)==72);

/* Event 2/version 1: queue state at preemption, failure and reset boundaries.
 * Addresses are opaque identities; the trace never reads command memory. */
enum tg_gpu_execution_source {
    TgGpuExecutionPreempt=1, TgGpuExecutionWorkerFailure,
    TgGpuExecutionSubmitFailure, TgGpuExecutionResetBegin, TgGpuExecutionResetEnd
};
enum tg_gpu_execution_stage {
    TgGpuExecutionIdle, TgGpuExecutionSoftware, TgGpuExecutionPaging,
    TgGpuExecutionMappings, TgGpuExecutionActivate, TgGpuExecutionPrepare,
    TgGpuExecutionSubmit, TgGpuExecutionGpuFence, TgGpuExecutionRetire,
    TgGpuExecutionCpuFence, TgGpuExecutionNotify, TgGpuExecutionPreemptPrefix
};
typedef struct _TG_GPU_EXECUTION_EVENT {
    ULONG Version,Source;
    LUID AdapterLuid;
    NTSTATUS Status;
    ULONG RequestFence,RequestFlags,Stage;
    ULONGLONG RequestContext,Context,DmaAddress;
    ULONG SubmissionFence,LastCompletedFence,Expected,Observed;
    ULONG DmaBytes,WorkFlags,QueueFlags;
    NTSTATUS QueueFailure;
    ULONG GraphicsGeneration,ChannelStage,ChannelRegister,ChannelValue;
    ULONG CachedFifo,CachedMmu,CachedPbdma,CachedPbdma1;
    ULONG CachedMc,CachedGr,CachedFecsError,CachedFecsHost;
    ULONG PollCount,RequestDmaBytes,PrivateBytes,UmdPrivateBytes;
} TG_GPU_EXECUTION_EVENT;
C_ASSERT(sizeof(TG_GPU_EXECUTION_EVENT)==152);

/* Event 8/version 1: direct TDR DDI observations. Phase 0 is entry (Status
 * is not a return value); phase 1 is return. ETW thread/QPC plus Engine and
 * adapter/boot identity associate synchronous entry/return pairs. Engine is
 * opaque. Result/identity/snapshot flags distinguish unavailable fields. */
enum tg_gpu_tdr_operation {
    TgGpuTdrResetAdapter=1,TgGpuTdrRestartAdapter,TgGpuTdrQueryEngine,
    TgGpuTdrResetEngine,TgGpuTdrQueryFence
};
enum tg_gpu_tdr_flags {
    TgGpuTdrSubmittedValid=1,TgGpuTdrResultValid=2,TgGpuTdrEngineSnapshot=4,
    TgGpuTdrQuerySampleValid=8,TgGpuTdrHealthy=16,TgGpuTdrIdle=32,
    TgGpuTdrRecoveryPending=64,TgGpuTdrResetting=128,TgGpuTdrQueuePaused=256,
    TgGpuTdrQueueFailure=512,TgGpuTdrQueueServicing=1024,TgGpuTdrQueuePending=2048,
    TgGpuTdrIdentityValid=4096
};
typedef struct _TG_GPU_TDR_EVENT {
    ULONG Version,Operation,Phase,Flags;
    ULONGLONG Engine;
    LUID AdapterLuid;
    NTSTATUS Status;
    ULONG NodeOrdinal,EngineOrdinal,LastSubmittedFence,LastCompletedFence;
    ULONG Result,QuerySampleFence;
    NTSTATUS QueueFailure;
} TG_GPU_TDR_EVENT;
C_ASSERT(sizeof(TG_GPU_TDR_EVENT)==64);

/* Event 9/version 1: actual context identity and paging lifecycle observations.
 * Pointer equality alone is not an object-lifetime or SystemContext claim.
 * ContextId comes from this engine's creation serial; named flags are read
 * from the live object under Engine.Lock. No referenced memory is exported. */
enum tg_gpu_context_operation {
    TgGpuContextCreate=1,TgGpuContextDestroy,TgGpuContextSetRoot,
    TgGpuContextPrepare,TgGpuContextPagingDmaComplete,TgGpuContextCaptureFailure
};
enum tg_gpu_context_flags {
    TgGpuContextFound=1,TgGpuContextProcessPresent=2,TgGpuContextAdapterValid=4,
    TgGpuContextRootValid=8,TgGpuContextRebuilding=16,TgGpuContextGpuCompleted=32,
    TgGpuContextCpuCompleted=64,TgGpuContextRetired=128,TgGpuContextObservedValid=256,
    TgGpuContextObservedCpu=512,TgGpuContextSystem=1024,TgGpuContextVirtual=2048,
    TgGpuContextGdi=4096,TgGpuContextSystemProcess=8192,TgGpuContextRecovery=16384,
    TgGpuContextResetting=32768,TgGpuContextStopping=65536,TgGpuContextGraphicsReady=131072,
    TgGpuContextClosing=262144,TgGpuContextWorkRootValid=524288,
    TgGpuContextWorkRootMatches=1048576,TgGpuContextWorkSnapshot=2097152,
    TgGpuContextSubmitted=4194304,TgGpuContextHardwareBusy=8388608,
    TgGpuContextAttempted=16777216,TgGpuContextPaging=33554432
};
enum tg_gpu_context_decision {
    TgGpuContextNoDecision,TgGpuContextEngineUnavailable,TgGpuContextInvalid,
    TgGpuContextRootUnavailable,TgGpuContextRootFailed,TgGpuContextGraphicsUnavailable,
    TgGpuContextStaleGeneration
};
typedef struct _TG_GPU_CONTEXT_EVENT {
    ULONG Version,Operation,Flags;
    NTSTATUS Status;
    ULONGLONG Engine,Context,Device,Process;
    LUID AdapterLuid;
    ULONG ContextId,ContextFlags,ProcessFlags,EngineGeneration,ContextGeneration,PriorGeneration;
    NTSTATUS RootStatus;
    ULONG RootEntries;
    ULONGLONG RootSegmentOffset;
    ULONG RootSegmentId,SubmissionFence,Expected,Observed,Decision,Reserved;
} TG_GPU_CONTEXT_EVENT;
C_ASSERT(sizeof(TG_GPU_CONTEXT_EVENT)==120);

/* Event 11/version 1: first unsupported paging request per StartDevice.
 * UpdateFieldsValid distinguishes an update request from other operations.
 * Detail is the rejection's scalar value, never a dereferenced allocation. */
typedef struct _TG_GPU_PAGING_REJECTION_EVENT {
    ULONG Version,Operation;
    NTSTATUS Status;
    ULONG Site,PageTableLevel,UpdateMode,HasAllocation,UpdateFieldsValid;
    ULONGLONG Detail;
} TG_GPU_PAGING_REJECTION_EVENT;
C_ASSERT(sizeof(TG_GPU_PAGING_REJECTION_EVENT)==40);
VOID TgGpuTracePagingRejection(const TG_GPU_PAGING_REJECTION_EVENT *record);
/* Event 12/version 1 uses the same payload for the first accepted
 * GPU_PHYSICAL level-1 request carrying a nonzero leaf kind. */
VOID TgGpuTracePagingDirectory(const TG_GPU_PAGING_REJECTION_EVENT *record);

/* Event 3/version 1: immutable owned command bytes before destructive reset.
 * One 144-byte header plus at most 1024 command bytes per event. */
#define TG_GPU_TRACE_COMMAND_CHUNK_BYTES 1024u
typedef struct _TG_GPU_RESET_PACKET_EVENT {
    ULONG Version,ChunkBytes;
    LUID AdapterLuid;
    ULONGLONG Context,DmaAddress;
    ULONG SubmissionFence,GraphicsGeneration,Expected,DmaBytes,CommandBytes,Offset;
    struct tg_gpu_channel_snapshot Hardware;
} TG_GPU_RESET_PACKET_EVENT;
C_ASSERT(sizeof(TG_GPU_RESET_PACKET_EVENT)==144);

/* Event 4/version 1: mode-path entry/return, not a Present completion.
 * RequestedRotation is zero when the request carries a VidPN handle rather
 * than an inline angle. PathId is a source ID, or target ID for SetTimings.
 * Flags is Commit flags, SetTimings flags, scaling for Update, or visibility. */
enum tg_gpu_display_operation {
    TgGpuDisplayCommit=1, TgGpuDisplaySetTimings, TgGpuDisplayUpdatePath,
    TgGpuDisplayVisibility
};
typedef struct _TG_GPU_DISPLAY_EVENT {
    ULONG Version,Operation;
    LUID AdapterLuid;
    NTSTATUS Status;
    ULONG Phase,PathId,RequestedRotation,BeforeRotation,AfterRotation;
    ULONG SourceVisible,TargetActive,Flags;
} TG_GPU_DISPLAY_EVENT;
C_ASSERT(sizeof(TG_GPU_DISPLAY_EVENT)==52);

/* Versioned payload returned to Windows for WDDM 2.6 startup diagnostics.
 * A missing adapter leaves Stage and StartStatus unavailable. */
enum tg_gpu_startup_stage {
    TgGpuStartupUnavailable, TgGpuStartupAdded, TgGpuStartupEntering,
    TgGpuStartupPermit, TgGpuStartupResources, TgGpuStartupHardware,
    TgGpuStartupPanel, TgGpuStartupComplete
};
typedef struct _TG_GPU_STARTUP_DIAGNOSTIC {
    ULONG Version,Type,AdapterAvailable,Stage;
    NTSTATUS StartStatus;
} TG_GPU_STARTUP_DIAGNOSTIC;
C_ASSERT(sizeof(TG_GPU_STARTUP_DIAGNOSTIC)==20);

/* Event 6/version 1 is emitted only when the OS callback writes its payload. */
typedef struct _TG_GPU_STARTUP_DIAGNOSTIC_EVENT {
    TG_GPU_STARTUP_DIAGNOSTIC Snapshot;
    ULONG BufferSizeIn,BufferSizeOut;
} TG_GPU_STARTUP_DIAGNOSTIC_EVENT;
C_ASSERT(sizeof(TG_GPU_STARTUP_DIAGNOSTIC_EVENT)==28);

/* Event 10/version 2: GM20B stall fault latch (DIRQL top-half mask-only plus
 * optional single-owner PASSIVE raw register capture before cold reset).
 * Hardware register fields remain raw; software classification lives in Flags. */
enum tg_gpu_fault_isr_flags {
    TgGpuFaultFlagUnreachable     = 0x01u,
    TgGpuFaultFlagStallIrq        = 0x02u,
    TgGpuFaultFlagDetailsCaptured = 0x04u,
    TgGpuFaultFlagMultiMmu        = 0x08u,
    TgGpuFaultFlagPriTimeout      = 0x10u,
    TgGpuFaultFlagFecsError       = 0x20u,
};
enum tg_gpu_fault_isr_phase {
    TgGpuFaultPhaseTopHalf        = 1u,
    TgGpuFaultPhasePassiveCapture = 2u,
};
typedef struct _TG_GPU_FAULT_ISR_EVENT {
    ULONG Version,Flags;
    LUID AdapterLuid;
    ULONG Epoch,McIntr0,FifoIntr,MmuFaultMask;
    ULONG MmuFaultCount,FaultId,FaultInst,FaultAddrLo;
    ULONG FaultAddrHi,FaultInfo,Pbdma0,Pbdma1;
    ULONG BusIntr,PriStatus0,PriStatus1,GrIntr;
    ULONG PriSave0,PriSave1,PriFecs,IsrCount;
    ULONG DpcCount,Phase;
} TG_GPU_FAULT_ISR_EVENT;
C_ASSERT(sizeof(TG_GPU_FAULT_ISR_EVENT)==104);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuTraceInitialize(VOID);
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgGpuTraceShutdown(VOID);
/* EtwWrite accepts nonpaged system-memory payloads at elevated IRQL. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceRecovery(const TG_GPU_RECOVERY_EVENT *);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceFaultIsr(const TG_GPU_FAULT_ISR_EVENT *);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceExecution(const TG_GPU_EXECUTION_EVENT *);
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN TgGpuTraceTdrEnabled(VOID);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceTdr(const TG_GPU_TDR_EVENT *);
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN TgGpuTraceContextEnabled(VOID);
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceContext(const TG_GPU_CONTEXT_EVENT *);
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN TgGpuTraceResetPacketEnabled(VOID);
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgGpuTraceResetPacket(const TG_GPU_RESET_PACKET_EVENT *,const VOID *Commands);
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgGpuTraceDisplay(const TG_GPU_DISPLAY_EVENT *);
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgGpuTraceStartupDiagnostic(const TG_GPU_STARTUP_DIAGNOSTIC_EVENT *);
#endif
