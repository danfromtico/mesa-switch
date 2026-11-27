/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_PRIVATE_MEMORY_H
#define TEGRA_GPU_PRIVATE_MEMORY_H

#include <ntddk.h>
#include <dispmprt.h>

typedef struct _TG_GPU_PRIVATE_MEMORY {
    HANDLE Handle;
    PVOID CpuAddress;
    PHYSICAL_ADDRESS SystemPhysicalAddress;
    SIZE_T Bytes;
} TG_GPU_PRIVATE_MEMORY;

/* WDDM 2.4 allocation callbacks track private GPU-visible memory and its
 * OS IOMMU mapping lifetime. The T214 bus-address mapping still belongs to
 * the platform backend: SystemPhysicalAddress is not a GPU-VA or a license
 * to bypass Tegra's SMMU. Initial command/fence/page-table buffers use an
 * uncached CPU mapping; the GPU PTE cache policy is configured separately.
 * The caller supplies inclusive physical allocation bounds. Zero-initialize
 * Memory before its first allocation.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuAllocatePrivateMemory(const DXGKRNL_INTERFACE *Dxgk,
    SIZE_T Bytes, UINT64 LowestAddress, UINT64 HighestAddress,
    TG_GPU_PRIVATE_MEMORY *Memory);

/* The adapter must retire all GPU references and remove GPU mappings first.
 * On a failed free, preserve the handle so cleanup can be retried.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgGpuFreePrivateMemory(const DXGKRNL_INTERFACE *Dxgk,
                                TG_GPU_PRIVATE_MEMORY *Memory);

#endif
