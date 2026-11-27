/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_INTERNAL_VA_H
#define TEGRA_GPU_INTERNAL_VA_H
#include "root_table.h"
#include "../core/submission.h"
#define TG_GPU_INTERNAL_COVERAGE (UINT64_C(1)<<TG_GPU_DIRECTORY_SHIFT)
typedef struct _TG_GPU_INTERNAL_VA {
    UINT64 Base;
    TG_GPU_PRIVATE_MEMORY Leaf;
    UINT64 GraphicsPhysical;
} TG_GPU_INTERNAL_VA;

/* Call only within CreateProcess. VidMm owns the VA reservation lifetime;
 * the KMD owns its leaf table. Do not let the UMD map into this reservation.
 * The shared command backing is protected by the engine's execution slot.
 */
NTSTATUS TgGpuCreateInternalVa(const DXGKRNL_INTERFACE *, HANDLE RuntimeProcess,
    const TG_GPU_PRIVATE_MEMORY *Commands, TG_GPU_INTERNAL_VA *);
/* Context must be idle. VidMm can clear reserved PDEs when making a root
 * resident, so republish this entry before binding/submitting the context.
 * The submit worker must perform the ordered TLB invalidate afterward.
 */
NTSTATUS TgGpuInstallInternalRoot(const TG_GPU_TABLE_SEGMENT *, const TG_GPU_ROOT_TABLE *,
                                const TG_GPU_INTERNAL_VA *);
#endif
