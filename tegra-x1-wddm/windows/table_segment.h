/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_TABLE_SEGMENT_H
#define TEGRA_GPU_TABLE_SEGMENT_H
#include "private_memory.h"
#include "paging.h"

#define TG_GPU_PAGING_APERTURE_SEGMENT 2u
#define TG_GPU_PAGING_APERTURE_BYTES (1024u * 1024u)
#define TG_GPU_SCANOUT_SEGMENT 3u
#define TG_GPU_SCANOUT_SEGMENT_BYTES (16u * 1024u * 1024u)
/* Direct physical GM20B DMA must leave Tegra SMMU translation bit 34 clear. */
#define T214_SYSTEM_LIMIT (UINT64_C(1) << 34)

/* VidMm owns allocation/residency within this CPU-visible memory segment.
 * The adapter owns its contiguous, uncached backing until StopDevice. This
 * is not a second page allocator and never changes global Tegra SMMU ASIDs.
 * The initial T214 backend uses untagged physical memory below 4 GiB.
 */
typedef struct _TG_GPU_TABLE_SEGMENT {
    TG_GPU_PRIVATE_MEMORY Memory;
    TG_GPU_PRIVATE_MEMORY Scanout;
    UINT SegmentId;
    volatile LONG AperturePfns[TG_GPU_PAGING_APERTURE_BYTES / PAGE_SIZE];
} TG_GPU_TABLE_SEGMENT;

/* StartDevice, before publishing any segment/page-table description. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuCreateTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    UINT SegmentId, SIZE_T Bytes, TG_GPU_TABLE_SEGMENT *Segment);

/* Caller must stop new paging operations and retire all GPU references
 * (completed fences or a successful drain/reset) before freeing the pool.
 * Failed frees retain the allocation and segment ID for cleanup retry.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuDestroyTableSegment(const DXGKRNL_INTERFACE *Dxgk,
    TG_GPU_TABLE_SEGMENT *Segment);

NTSTATUS TgGpuDescribeTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    DXGK_SEGMENTDESCRIPTOR3 *Description);

/* Optional contiguous backing for the fixed-mode panel's VidMm primaries. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuCreateScanoutSegment(const DXGKRNL_INTERFACE *Dxgk,
    TG_GPU_TABLE_SEGMENT *Segment);
NTSTATUS TgGpuDescribeScanoutSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    DXGK_SEGMENTDESCRIPTOR4 *Description);
NTSTATUS TgGpuResolveScanoutSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    UINT64 ByteOffset, SIZE_T Bytes, UINT64 *DeviceAddress);
/* Display DDIs supply BaseAddress + allocation offset, unlike PTE page numbers. */
NTSTATUS TgGpuResolveScanoutAddress(const TG_GPU_TABLE_SEGMENT *,UINT64,SIZE_T,UINT64 *);

/* BuildPagingBuffer serializes changes with the adapter lock. VidMm owns
 * the MDL and retains residency while a mapping is used to build GPU PTEs.
 */
NTSTATUS TgGpuUpdatePagingAperture(TG_GPU_TABLE_SEGMENT *Segment,
    const DXGKARG_BUILDPAGINGBUFFER *Args);

/* TG_GPU_RESOLVE_PAGE callback for this segment only. System-memory pages
 * and other segments require their own pinned/resident mapping resolver.
 * ByteOffset is the segment-relative offset after decoding the PTE page number.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuResolveTableSegment(PVOID Context, UINT SegmentId,
    UINT64 ByteOffset, SIZE_T Bytes, UINT64 *DeviceAddress);

/* ByteAddress is a resident physical page for segment zero, or a byte offset
 * into the table segment or paging aperture. Aperture offsets resolve through
 * the current VidMm mapping before the GM20B PTE is encoded.
 * Caller holds VidMm residency through execution/fence completion. This
 * resolver neither pins pages nor accepts application-supplied addresses.
 * A future logical-DMA/IOMMU path requires a different advertised contract.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuResolveVidMmPage(PVOID Context, UINT SegmentId,
    UINT64 ByteAddress, SIZE_T Bytes, UINT64 *DeviceAddress);

/* Access through the pool's own uncached mapping. A VidMm CPU_VIRTUAL update
 * can carry a different alias; do not compare that VA to this VA as proof
 * of allocation ownership. The adapter must validate its physical span.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS TgGpuMapTableSegment(const TG_GPU_TABLE_SEGMENT *Segment,
    UINT64 ByteOffset, SIZE_T Bytes, PVOID *CpuAddress, UINT64 *DeviceAddress);
#endif
