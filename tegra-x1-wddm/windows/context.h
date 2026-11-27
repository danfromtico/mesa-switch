/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_CONTEXT_H
#define TEGRA_GPU_CONTEXT_H
#include "root_table.h"
#include "internal_va.h"
#include "../core/channel.h"
#include "../core/command_stream.h"
#include "../platform/t214_clock.h"
#include "adapter_abi.h"
#include "nvdec_abi.h"
#include "graphics.h"
#include "debug_report.h"
#include "fault_isr.h"

typedef struct _TG_GPU_CONTEXT_ENGINE TG_GPU_CONTEXT_ENGINE;
typedef struct _TG_GPU_PROCESS TG_GPU_PROCESS;
typedef struct _TG_GPU_DEVICE TG_GPU_DEVICE;
typedef struct _TG_GPU_CONTEXT TG_GPU_CONTEXT;
typedef struct _TG_GPU_CONTEXT_WORK TG_GPU_CONTEXT_WORK;

struct _TG_GPU_PROCESS {
    LIST_ENTRY Link;
    TG_GPU_CONTEXT_ENGINE *Engine;
    HANDLE Runtime;
    UINT Devices;
    UINT PagingReferences;
    DXGK_CREATEPROCESSFLAGS Flags;
    TG_GPU_INTERNAL_VA Internal;
    BOOLEAN Closing;
};
struct _TG_GPU_DEVICE {
    LIST_ENTRY Link;
    TG_GPU_CONTEXT_ENGINE *Engine;
    TG_GPU_PROCESS *Process;
    HANDLE Runtime;
    UINT Contexts;
    UINT OpenAllocations;
    UINT VideoReferences;
    BOOLEAN Closing;
};
struct _TG_GPU_CONTEXT {
    LIST_ENTRY Link;
    TG_GPU_DEVICE *Device;
    HANDLE Runtime;
    TG_GPU_PRIVATE_MEMORY Instance;
    TG_GPU_ROOT_TABLE Root;
    DXGK_CREATECONTEXTFLAGS Flags;
    NTSTATUS RootStatus;
    UINT References;
    UINT GraphicsGeneration,ContextId;
    UINT64 Cb0End;
    struct tg_gpu_program_state Programs;
    struct tg_gpu_texture_pool_range TexturePool[TG_GPU_PROGRAM_ENGINES][TG_GPU_TEXTURE_POOLS];
    struct tg_gpu_compute_slm ComputeSlm;
    BOOLEAN Closing,MmeInitialized;
    UINT PagingEvidenceGeneration; /* first observed CE sub-operation per epoch */
};
struct _TG_GPU_CONTEXT_ENGINE {
    DXGKRNL_INTERFACE Dxgk;
    TG_GPU_TABLE_SEGMENT *Tables;
    KSPIN_LOCK Lock;
    LIST_ENTRY Processes, Devices, Contexts;
    TG_GPU_CONTEXT_WORK *Active;
    TG_GPU_CONTEXT_WORK *Notification;
    PVOID QueueOwner;
    PVOID HardwareOwner;
    PVOID VideoOwner;
    NTSTATUS (*VideoCloseDevice)(PVOID Owner, TG_GPU_DEVICE *Device);
    TG_GPU_PRIVATE_MEMORY Commands;
    TG_GPU_GRAPHICS_TEMPLATE Graphics;
    UINT GraphicsGeneration,ContextSerial;
    /* The cold backend must map Commands' USERD page into its retained BAR1
     * root and program FIFO_BAR1_BASE before making this mapping ready. */
    UINT32 UserdBar1Offset;
    BOOLEAN UserdBar1Ready;
    struct tg_gpu_channel Channel;
    volatile UINT LastCompletedFence;
    UINT LastSubmittedFence,EngineQueryFence;
    BOOLEAN SubmittedFenceValid;
    UINT Marker;
    UINT SnapshotUploads,SnapshotBytes;
    UINT Creating;
    UINT AllocationCount,ResourceCount;
    UINT64 AllocationBytes;
    BOOLEAN Stopping;
    LUID AdapterLuid;
    BOOLEAN AdapterIdentityReady, PagingOnly;
    struct tg_nvdec_resource_info NvdecInfo; /* copied start snapshot; no borrowed MMIO */
    struct tg_nvdec_client_info NvdecClients;
    NTSTATUS RecoveryStatus;
    BOOLEAN RecoveryPending,RecoverySending,RecoveryNotified,Resetting,ShutdownFinal;
    ULONG RecoverySendToken,RecoveryActiveToken;
    /* Resident nonpaged diagnostic breadcrumbs (lock-free, safe at any IRQL). */
    volatile LONG DiagWorkerStage;
    volatile LONG DiagLastPagingKind;
    volatile LONG DiagCommitSeq;
    volatile LONG DiagCommitPermitCount;
    volatile LONG DiagCommitStep;
    volatile LONG DiagLastMmioReg;
    volatile LONG DiagMmioEnterCount;
    volatile LONG DiagMmioDoneCount;
    TG_GPU_FAULT_ISR FaultIsr;
    TG_GPU_DEBUG_CACHE DebugCache;
};
/* The single physical execution slot is serialized; the number of software
 * contexts is limited only by allocation failures, not a fixed slot array.
 * Keep this record alive until retirement or verified reset succeeds.
 */
struct _TG_GPU_CONTEXT_WORK {
    TG_GPU_CONTEXT_ENGINE *Engine;
    TG_GPU_CONTEXT *Context;
    TG_GPU_ROOT_TABLE Root;
    UINT64 DmaAddress, IndirectAddress, FencePhysicalAddress, InternalVa;
    UINT DmaBytes, IndirectBytes, IndirectSuffixOffset, GpPut;
    UINT SubmissionFence, CompletionFence, Expected;
    BOOLEAN Signaling, Notified, Prepared, HardwareBusy, Submitted, Attempted, Retired, CompletionReady, Paging;
    BOOLEAN Software,Rejected,Preemption;
    BOOLEAN GpuCompleted,CpuCompleted,UploadsMme;
    UINT PagingOffset, PagingBytes, PagingCursor, PagingDmaBytes;
    BOOLEAN PagingStarted;
    LONG FaultEpoch;
    /* Last completed observation, copied under Engine.Lock. No extra MMIO. */
    UINT TracePolls,TraceObserved;
    UINT TraceFifo,TraceMmu,TracePbdma,TracePbdma1;
    UINT TraceMc,TraceGr,TraceFecsError,TraceFecsHost;
    BOOLEAN TraceObservedValid,TraceObservedCpu,TraceMcValid,TraceGrValid;
};

/* Validate driver handles by list membership before dereferencing. A video
 * operation/session retains the actual device until its last DMA is retired. */
NTSTATUS TgGpuReferenceVideoDevice(TG_GPU_CONTEXT_ENGINE *, HANDLE, TG_GPU_DEVICE **);
void TgGpuDereferenceVideoDevice(TG_GPU_DEVICE *);
/* Owner storage is zeroed once before first use (nonpaged zeroed pool in the
 * adapter/diagnostic callers). The resident cache then lives until owner free. */
NTSTATUS TgGpuInitializeContextEngine(TG_GPU_CONTEXT_ENGINE *, const DXGKRNL_INTERFACE *,
                                     TG_GPU_TABLE_SEGMENT *);
/* Install the trusted cold backend's immutable golden copy once. Submission
 * remains unavailable until both this template and the BAR1 mapping exist.
 */
NTSTATUS TgGpuInitializeGraphicsTemplate(TG_GPU_CONTEXT_ENGINE *,
    const struct tg_gpu_gr_work *,const struct tg_gpu_gr_context_status *);
/* Stops admission; fails until contexts/devices/processes and work retire. */
NTSTATUS TgGpuStopContextEngine(TG_GPU_CONTEXT_ENGINE *);
BOOLEAN TgGpuIsGdiContext(TG_GPU_CONTEXT_ENGINE *,HANDLE);
/* Populate only these implemented entries in a caller-owned initialization
 * table. This module does not register a display adapter by itself.
 */
void TgGpuRegisterContextDdis(DRIVER_INITIALIZATION_DATA *);

DXGKDDI_CREATEPROCESS TgGpuDdiCreateProcess;
DXGKDDI_DESTROYPROCESS TgGpuDdiDestroyProcess;
DXGKDDI_CREATEDEVICE TgGpuDdiCreateDevice;
DXGKDDI_DESTROYDEVICE TgGpuDdiDestroyDevice;
DXGKDDI_CREATECONTEXT TgGpuDdiCreateContext;
DXGKDDI_DESTROYCONTEXT TgGpuDdiDestroyContext;
DXGKDDI_GETROOTPAGETABLESIZE TgGpuDdiGetRootPageTableSize;
DXGKDDI_SETROOTPAGETABLE TgGpuDdiSetRootPageTable;
DXGKDDI_QUERYCURRENTFENCE TgGpuDdiQueryCurrentFence;
DXGKDDI_DPC_ROUTINE TgGpuDdiDpcRoutine;

/* Submission worker helpers, not SubmitCommandVirtual DDIs. In particular,
 * STATUS_DEVICE_BUSY must stay inside the software queue: the actual DDI may
 * return only the statuses allowed by Dxgkrnl's submission contract.
 * The caller still validates command packets and VidMm residency before
 * programming hardware; a bounded virtual address is not that validation.
 */
NTSTATUS TgGpuAcquireContextWork(TG_GPU_CONTEXT_ENGINE *,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *, TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuGetCommandVa(TG_GPU_CONTEXT_ENGINE *,HANDLE,struct tg_gpu_command_va *);
/* Queue admission retains the context/root without touching shared commands,
 * RAMIN or the fence. Activation occurs only when this record reaches the head.
 * A successful capture must be activated/retired or canceled exactly once.
 */
NTSTATUS TgGpuCaptureContextWork(TG_GPU_CONTEXT_ENGINE *,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *, const struct tg_gpu_command_info *,
    TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuCapturePagingContext(TG_GPU_CONTEXT_ENGINE *,HANDLE,TG_GPU_CONTEXT_WORK *,TG_GPU_CONTEXT_EVENT *);
VOID TgGpuTracePagingCompletion(TG_GPU_CONTEXT_ENGINE *,TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuActivateContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
                                  const struct tg_gpu_command_info *);
NTSTATUS TgGpuCancelCapturedWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuCaptureSoftwareWork(TG_GPU_CONTEXT_ENGINE *,const DXGKARG_SUBMITCOMMANDVIRTUAL *,TG_GPU_CONTEXT_WORK *);
NTSTATUS TgGpuArmSoftwareCompletion(TG_GPU_CONTEXT_ENGINE *,TG_GPU_CONTEXT_WORK *);
/* DISPATCH-safe internal recovery request; no waits/MMIO or completed fence. */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuRequestEngineRecovery(TG_GPU_CONTEXT_ENGINE *,NTSTATUS);
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuRequestEngineRecoveryAt(TG_GPU_CONTEXT_ENGINE *,NTSTATUS,
    enum tg_gpu_recovery_source,const DXGKARG_SUBMITCOMMANDVIRTUAL *);
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuRequestEngineRecoveryForEpoch(TG_GPU_CONTEXT_ENGINE *,NTSTATUS,
    enum tg_gpu_recovery_source,const DXGKARG_SUBMITCOMMANDVIRTUAL *,LONG FaultEpoch);
/* Kernel-generated paging records share the ordered execution slot. CPU PTE
 * publication and real TLB completion precede arming the OS notification. */
NTSTATUS TgGpuExecutePagingWork(TG_GPU_CONTEXT_ENGINE *,TG_GPU_CONTEXT_WORK *,
    const void *,SIZE_T,const struct tg_gpu_power_io *,const struct tg_gpu_power *);
/* After acquisition and before hardware binding, repair the reserved PDE and
 * encode the USER workload plus a driver-generated KERNEL completion packet.
 * This performs no MMIO. The hardware backend still binds RAMIN, invalidates
 * the root TLB and submits the runlist before polling completion.
 */
NTSTATUS TgGpuPrepareContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* Worker path after cold GPU/FIFO/LTC/MMU/clock initialization. Queuing is
 * distinct from completion. These helpers are not yet registered submission
 * DDIs; their internal failure statuses must not be returned directly to
 * Dxgkrnl's SubmitCommandVirtual callback.
 */
NTSTATUS TgGpuSubmitContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, const struct tg_gpu_power *);
/* Check the GPU-owned fence without notification. STATUS_PENDING is not a pass. */
NTSTATUS TgGpuContextWorkFinished(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* Two-phase worker gate: before retirement require the fault-free GPU BAR1
 * Reference; after retirement require its saved CPU readback after L2 sync.
 * USERD.Reference may remain in the active channel until its context saves.
 * A CPU word alone is insufficient. Call again after Quiesce succeeds.
 */
NTSTATUS TgGpuObserveContextWork(TG_GPU_CONTEXT_ENGINE *,TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *,const struct tg_gpu_power *);
/* Quiesce before notifying VidSch, which may immediately recycle allocations
 * and private submission buffers. Keep the work reference until notification.
 */
NTSTATUS TgGpuQuiesceContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, const struct tg_gpu_power *);
/* Render work requires a matching GPU fence plus successful retirement.
 * Paging work enters the common notification phase only after the encoded
 * table updates and requested TLB invalidations have completed successfully.
 * Drop context ownership before notification; it may immediately cause object
 * destruction. A failed synchronization retains an engine-only notification
 * record for retry/reset and prevents the next work from overtaking its fence.
 * STATUS_PENDING is not completion, even though NT_SUCCESS accepts it.
 */
NTSTATUS TgGpuSignalContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
                               const struct tg_gpu_command_info *);
NTSTATUS TgGpuCancelContextNotification(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *);
/* Releases only after the actual channel-retirement sequence succeeds.
 * Timeout keeps the context/RAMIN/commands pinned. Aborts do not notify a
 * completed fence. An unsubmitted work item can be canceled without MMIO.
 */
NTSTATUS TgGpuRetireContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, const struct tg_gpu_power *);
/* Recovery: disable fetch, drain MC, stop clocks and power down. Hardware
 * clamp/reset readback is required before releasing a failed submission.
 * Reinitialize the GPU before subsequent submissions. TDR reporting remains
 * the adapter's responsibility; this routine never invents fence completion.
 */
NTSTATUS TgGpuResetContextWork(TG_GPU_CONTEXT_ENGINE *, TG_GPU_CONTEXT_WORK *,
    const struct tg_gpu_power_io *, struct tg_gpu_power *, struct tg_gpu_clock *);
#endif
