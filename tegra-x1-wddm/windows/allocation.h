/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_ALLOCATION_H
#define TEGRA_GPU_ALLOCATION_H
#include "context.h"
#include "allocation_abi.h"

/* Page tables use segment 1, the paging aperture uses segment 2, and display
 * mode adds scanout segment 3. VidMm appends the implicit system-memory
 * segment. These callbacks allocate metadata only; VidMm owns pages and VA.
 */
#define TG_GPU_EXPLICIT_TABLE_SEGMENT 1u

/* Explicit storage geometry is shared with the D3D UMD. NVK buffers retain
 * the original 16-byte allocation request. */
typedef struct tg_gpu_primary_allocation_request TG_GPU_PRIMARY_ALLOCATION_REQUEST;

/* Linear cross-adapter surfaces use their own geometry, not the fixed panel's
 * primary layout. Raw standard heaps keep the 16-byte buffer description. */
typedef struct tg_gpu_linear_allocation_request TG_GPU_LINEAR_ALLOCATION_REQUEST;

void TgGpuRegisterAllocationDdis(DRIVER_INITIALIZATION_DATA *);
/* OS-supplied KMD handle, held alive by VidMm for this paging call. */
NTSTATUS TgGpuValidatePagingAllocation(TG_GPU_CONTEXT_ENGINE *,HANDLE,UINT64,UINT64);
DXGKDDI_CREATEALLOCATION TgGpuDdiCreateAllocation;
DXGKDDI_DESTROYALLOCATION TgGpuDdiDestroyAllocation;
DXGKDDI_OPENALLOCATIONINFO TgGpuDdiOpenAllocation;
DXGKDDI_CLOSEALLOCATION TgGpuDdiCloseAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA TgGpuDdiGetStandardAllocationDriverData;
/* Check a device-specific open handle supplied by Dxgkrnl, never a UMD raw
 * pointer. This does not prove residency, pin pages or validate GPU commands.
 * The caller must hold the open object's lifetime through this operation.
 */
NTSTATUS TgGpuValidateOpenAllocationRange(TG_GPU_DEVICE *,HANDLE,
                                         UINT64 Offset,UINT64 Bytes,BOOLEAN Write);
NTSTATUS TgGpuGetPresentSurfaces(TG_GPU_DEVICE *,HANDLE Source,HANDLE Destination,
                                 TG_GPU_PRIMARY_ALLOCATION_REQUEST *,TG_GPU_PRIMARY_ALLOCATION_REQUEST *);
/* Immutable metadata through an OS-supplied KMD allocation handle. Dxgkrnl
 * keeps it alive for scanout/check DDIs, including their high-IRQL calls. */
NTSTATUS TgGpuGetScanoutSurface(TG_GPU_CONTEXT_ENGINE *,HANDLE,
                                TG_GPU_PRIMARY_ALLOCATION_REQUEST *);
#endif
