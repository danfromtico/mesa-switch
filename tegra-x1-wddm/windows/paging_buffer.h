/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_PAGING_BUFFER_H
#define TEGRA_GPU_PAGING_BUFFER_H
#include "root_table.h"
#include "../core/paging_dma.h"

typedef struct _TG_GPU_PAGING_BUILD {
    const TG_GPU_TABLE_SEGMENT *Tables;
    SIZE_T TableBytes;
    const TG_GPU_ROOT_TABLE *Root;
    UINT64 *Scratch;
    SIZE_T ScratchEntries;
    BOOLEAN GpuPagesUncached;
    BOOLEAN InitialMappingUncached;
    TG_GPU_PAGING_REJECTION *Rejection;
} TG_GPU_PAGING_BUILD;

/* Build private kernel paging records into the supplied DMA buffer. Ordinary
 * GPU_PHYSICAL updates have no hardware side effects here. The mandatory
 * paging-process bootstrap in CPU_VIRTUAL mode updates the validated alias
 * of the owned, uncached table segment and leaves the DMA buffer untouched.
 * TableBytes is the actual target allocation size, including resized roots.
 * The adapter validates hProcess/hAllocation before calling this helper.
 * PTE addresses use the T214 physical VidMm resolver: segment zero is RAM;
 * the sole nonzero segment is table backing. No UMD address resolver is used.
 */
NTSTATUS TgGpuBuildPagingBuffer(DXGKARG_BUILDPAGINGBUFFER *Args,
                               const TG_GPU_PAGING_BUILD *Build);
/* Whole-buffer validation without publishing a PTE or issuing MMIO. */
NTSTATUS TgGpuValidatePagingBuffer(const TG_GPU_TABLE_SEGMENT *,const void *,SIZE_T,
                                  BOOLEAN *RequiresSystemContext);

typedef struct _TG_GPU_PAGING_DMA_STEP {
    struct tg_gpu_paging_dma Operation;
    SIZE_T RecordBytes;
} TG_GPU_PAGING_DMA_STEP;

/* Submit-worker path, not a GPU command packet parser or a registered DDI.
 * Retire earlier GPU work and hold affected address spaces idle/serialized.
 * Keep segment backing and referenced pages resident for the whole queued
 * operation. Buffer must remain private and immutable until it is retired.
 * Publish uncached PTEs and issue real MMIO TLB invalidations in record order.
 * A virtual copy/fill is returned in NextDma without counting its record as
 * complete. The caller executes it on the paging context and resumes after
 * verified retirement. A NULL NextDma accepts CPU/MMIO records only.
 * On failure, CompletedBytes is diagnostic progress, NOT a completed fence;
 * keep old pages pinned until recovery drains/resets the GPU successfully.
 */
NTSTATUS TgGpuExecutePagingBuffer(const TG_GPU_TABLE_SEGMENT *Tables,
    const void *Buffer, SIZE_T Bytes, BOOLEAN AddressSpaceIdle,
    const struct tg_gpu_power_io *Io, const struct tg_gpu_power *Power,
    SIZE_T *CompletedBytes, TG_GPU_PAGING_DMA_STEP *NextDma);
NTSTATUS TgGpuExecutePagingBufferChecked(const TG_GPU_TABLE_SEGMENT *Tables,
    const void *Buffer, SIZE_T Bytes, BOOLEAN AddressSpaceIdle,
    const struct tg_gpu_power_io *Io, const struct tg_gpu_power *Power,
    SIZE_T *CompletedBytes, TG_GPU_PAGING_DMA_STEP *NextDma,
    const struct tg_gpu_commit_guard *Guard,
    volatile LONG *DiagLastKind);
#endif
