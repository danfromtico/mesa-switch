/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_WINDOWS_PAGING_H
#define TEGRA_GPU_WINDOWS_PAGING_H
#include <ntddk.h>
#include <dispmprt.h>
#include "../core/gm20b_mmu.h"

/* Drain non-cacheable CPU store buffers to Tegra MC DRAM before MMIO/DMA. */
static __forceinline VOID TgGpuDataSyncBarrier(VOID)
{
#if defined(_M_ARM64) || defined(_ARM64_)
    _DataSynchronizationBarrier();
#else
    KeMemoryBarrier();
#endif
}

/* The adapter supplies a pinned table mapping and reusable nonpaged scratch
 * memory. Updates run with the address space idle and serialized. The
 * resolver translates a VidMm segment offset/system byte address into an owned GPU
 * bus mapping; it must validate the entire requested span (including a
 * contiguous 128 KiB child table). It does not allocate or change residency.
 * Scratch is separate from the target mapping and the input PTE array.
 */
typedef NTSTATUS (*TG_GPU_RESOLVE_PAGE)(PVOID Context, UINT Segment,
    UINT64 ByteOffset, SIZE_T Bytes, UINT64 *DeviceAddress);

/* Values are part of the one-use paging probe's stop screen. */
typedef enum _TG_GPU_PAGING_REJECT_SITE {
    TgGpuPagingRejectUnknown = 0,
    TgGpuPagingRejectOperation = 1,
    TgGpuPagingRejectDirectoryAllocation = 2,
    TgGpuPagingRejectUpdateMode = 3,
    TgGpuPagingRejectUpdateFlags = 4,
    TgGpuPagingRejectDriverProtection = 5,
    TgGpuPagingRejectPteFlags = 6,
    TgGpuPagingRejectDirectorySegment = 7,
    TgGpuPagingRejectCpuAlias = 8,
    TgGpuPagingRejectVirtualTransfer = 9,
    TgGpuPagingRejectApertureFlags = 10,
    TgGpuPagingRejectBufferOperation = 11
} TG_GPU_PAGING_REJECT_SITE;

typedef struct _TG_GPU_PAGING_REJECTION {
    TG_GPU_PAGING_REJECT_SITE Site;
    UINT64 Detail;
} TG_GPU_PAGING_REJECTION;

static __forceinline NTSTATUS TgGpuRejectPaging(TG_GPU_PAGING_REJECTION *Rejection,
    TG_GPU_PAGING_REJECT_SITE Site, UINT64 Detail)
{
    if (Rejection) {
        Rejection->Site = Site;
        Rejection->Detail = Detail;
    }
    return STATUS_NOT_SUPPORTED;
}

typedef struct _TG_GPU_PAGE_UPDATE_TARGET {
    volatile UINT64 *Entries;
    SIZE_T Size;
    UINT64 *Scratch;
    SIZE_T ScratchEntries;
    BOOLEAN CpuMappingUncached;
    BOOLEAN GpuPagesUncached;
    TG_GPU_RESOLVE_PAGE Resolve;
    PVOID ResolveContext;
    TG_GPU_PAGING_REJECTION *Rejection;
} TG_GPU_PAGE_UPDATE_TARGET;

/* Translate into caller-owned scratch only. Used while building an ordered
 * paging buffer; it must not publish PTEs or retain VidMm's input pointers.
 */
NTSTATUS TgGpuEncodePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *Update,
                             const TG_GPU_PAGE_UPDATE_TARGET *Target);

/* No unsupported attribute is silently dropped. Validate/translate the
 * complete update before publishing any entry. On success the caller must
 * still submit and wait for a real GPU TLB invalidate before page reuse.
 * Does not advertise an adapter or register a WDDM DDI by itself.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuUpdatePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *Update,
                              const TG_GPU_PAGE_UPDATE_TARGET *Target);

/* GM20B has 128 KiB tables; Microsoft limits system-segment tables to 4 KiB.
 * Require a nonzero, CPU-visible table segment backed by contiguous memory.
 * A zero/simulated segment ID is deliberately not advertised as usable.
 */
NTSTATUS TgGpuDescribePageTable(UINT Level, UINT TableSegment,
                               DXGK_PAGE_TABLE_LEVEL_DESC *Description);
#endif
