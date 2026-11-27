/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_WORK_QUEUE_H
#define TEGRA_GPU_WORK_QUEUE_H
#include "context.h"
#include "paging_buffer.h"
#include "command_snapshot.h"
#define TG_GPU_DMA_BUFFER_BYTES (64u*1024u)
#define TG_GPU_PAGING_PRIVATE_ENTRIES 8u
#define TG_GPU_PAGING_BUFFER_MAGIC 0x50474754u
#define TG_GPU_QUEUE_TAG 0x51674754u
typedef struct _TG_GPU_PAGING_BUFFER {
    TG_GPU_CONTEXT_ENGINE *Engine;
    PVOID CpuAddress;
    UINT64 GpuAddress;
    UINT WriteOffset,BuiltBytes,Capacity,Magic;
    HANDLE SystemContext; /* identity only, acquired at submit for CE records */
} TG_GPU_PAGING_BUFFER;

/* Dxgkrnl supplies nonpaged private storage for each submitted DMA buffer.
 * No user pointers or caller-initialized list links are trusted. Application
 * admission uses an owned queue node and validated immutable command snapshot,
 * never the borrowed OS/UMD private bytes after the DDI returns. Remaining
 * class-specific command/residency validation is separate.
 * Kernel-built paging metadata retains an OS-owned immutable DMA buffer; it
 * is never interpreted as a USER GPFIFO workload or an application context.
 */
typedef struct _TG_GPU_QUEUE_ENTRY {
    LIST_ENTRY Link;
    TG_GPU_CONTEXT_WORK Work;
    TG_GPU_PAGING_BUFFER Paging;
    TG_GPU_COMMAND_SNAPSHOT *Snapshot;
    BOOLEAN Owned;
} TG_GPU_QUEUE_ENTRY;
#define TG_GPU_PAGING_PRIVATE_BYTES (TG_GPU_PAGING_PRIVATE_ENTRIES*sizeof(TG_GPU_QUEUE_ENTRY))

typedef struct _TG_GPU_WORK_QUEUE {
    TG_GPU_CONTEXT_ENGINE *Engine;
    KSPIN_LOCK Lock;
    KMUTEX DispatchGate; /* PASSIVE DDI admission and worker completion order */
    LIST_ENTRY Pending;
    KEVENT Wake;
    TG_GPU_CONTEXT_WORK Current;
    TG_GPU_PAGING_BUFFER CurrentPaging;
    PVOID CurrentPrivate;
    TG_GPU_QUEUE_ENTRY *CurrentOwned;
    NTSTATUS Failure;
    UINT Preemptions; /* owned markers still in Pending, excludes Current */
    UINT TraceStage; /* worker phase, published under Lock */
    BOOLEAN Servicing, Paused;
} TG_GPU_WORK_QUEUE;

NTSTATUS TgGpuInitializeWorkQueue(TG_GPU_WORK_QUEUE *,TG_GPU_CONTEXT_ENGINE *);
/* DISPATCH-safe, allocation-free trusted internal/model admission using
 * pDmaBufferPrivateData. The untrusted DDI must use SubmitOrReject and a
 * validated snapshot; it cannot invoke this raw-address internal path.
 */
NTSTATUS TgGpuQueueSubmit(TG_GPU_WORK_QUEUE *,const DXGKARG_SUBMITCOMMANDVIRTUAL *);
/* DISPATCH-safe DMA-buffer-boundary preemption. Takes ownership of Reserved
 * only on success, and cuts the queue at the request's admission point.
 * Newer submissions stay after that marker for the next scheduling interval. */
NTSTATUS TgGpuQueuePreempt(TG_GPU_WORK_QUEUE *,const DXGKARG_PREEMPTCOMMAND *,
    TG_GPU_QUEUE_ENTRY *Reserved);
/* PASSIVE DDI reserves a nonpaged node before validation. SUCCESS transfers
 * Snapshot ownership; Used transfers Reserved ownership (also on a rejection,
 * which does not own Snapshot). Caller holds DispatchGate through admission
 * and idle rejection drain. Borrowed paging descriptors remain OS-owned. */
NTSTATUS TgGpuQueueSubmitOrReject(TG_GPU_WORK_QUEUE *,const DXGKARG_SUBMITCOMMANDVIRTUAL *,
    TG_GPU_QUEUE_ENTRY *Reserved,TG_GPU_COMMAND_SNAPSHOT *,BOOLEAN *Used);
/* Caller holds DispatchGate. A rejected packet with no older queue work can
 * advance immediately; this never executes an application or paging record. */
NTSTATUS TgGpuDrainRejectedIdle(TG_GPU_WORK_QUEUE *);
/* PASSIVE worker step: start/poll one item, quiesce, then notify its fence.
 * STATUS_PENDING keeps that item active. The adapter waits for Wake plus an
 * IRQ/timer timeout to call again; it must not spin on STATUS_PENDING.
 * No borrowed private-buffer pointer is touched after notifying Dxgkrnl.
 */
NTSTATUS TgGpuServiceWorkQueue(TG_GPU_WORK_QUEUE *,const struct tg_gpu_power_io *,
                             const struct tg_gpu_power *);
/* Close admission and recover the current item before canceling pending work.
 * A failed isolation keeps references. No canceled fence is reported complete.
 * The queue remains paused until the adapter performs cold reinitialization
 * and explicitly resumes it. These helpers do not replace TDR notification.
 */
NTSTATUS TgGpuResetWorkQueue(TG_GPU_WORK_QUEUE *,const struct tg_gpu_power_io *,
                           struct tg_gpu_power *,struct tg_gpu_clock *);
NTSTATUS TgGpuResumeWorkQueue(TG_GPU_WORK_QUEUE *,const struct tg_gpu_power *);
/* Detach only after the current item and the pending list have been retired. */
NTSTATUS TgGpuStopWorkQueue(TG_GPU_WORK_QUEUE *);
/* Copies scalars under queue/engine locks, emits after releasing both.
 * Caller keeps the engine/queue alive, as for preemption and reset DDIs. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID TgGpuTraceExecutionState(TG_GPU_CONTEXT_ENGINE *,enum tg_gpu_execution_source,
    NTSTATUS,UINT,const DXGKARG_SUBMITCOMMANDVIRTUAL *);
/* Reset only: caller has paused admission, joined the worker and acquired
 * DispatchGate, before ResetWorkQueue releases CurrentOwned. No spinlocks. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgGpuTraceResetWork(TG_GPU_WORK_QUEUE *,const struct tg_gpu_power_io *,
    const struct tg_gpu_power *);
#endif
