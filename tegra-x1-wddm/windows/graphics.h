/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_WINDOWS_GRAPHICS_H
#define TG_GPU_WINDOWS_GRAPHICS_H
#include "internal_va.h"
#include "../platform/t214_gr_context.h"

/* One owning allocation: RAMIN first, then packed private GR backing.
 * There is no second copy of the golden image in each context. Shader code,
 * application resources and render targets are separate VidMm allocations.
 */
#define TG_GPU_CONTEXT_GR_OFFSET 0x1000u
#define TG_GPU_CONTEXT_GR_VA_OFFSET 0x100000u
#define TG_GPU_CONTEXT_BUNDLE_OFFSET 0x20000u
#define TG_GPU_CONTEXT_PAGEPOOL_OFFSET 0x22000u
#define TG_GPU_CONTEXT_ATTRIB_OFFSET 0x2a000u
#define TG_GPU_CONTEXT_ACCESS_OFFSET 0x72000u
#define TG_GPU_CONTEXT_PATCH_OFFSET 0xf2000u
#define TG_GPU_CONTEXT_GR_BYTES 0xf3000u
#define TG_GPU_CONTEXT_PRIVATE_BYTES (TG_GPU_CONTEXT_GR_OFFSET+TG_GPU_CONTEXT_GR_BYTES)
typedef struct _TG_GPU_GRAPHICS_TEMPLATE {
    TG_GPU_PRIVATE_MEMORY Memory;
    UINT ImageBytes;
} TG_GPU_GRAPHICS_TEMPLATE;

/* Trusted cold-backend input only: require a successful golden save and copy
 * its immutable image. This copy does not retire GPU ownership of the source
 * work allocation; keep the cold arenas through their stop/drain/reset path.
 */
NTSTATUS TgGpuCreateGraphicsTemplate(const DXGKRNL_INTERFACE *,
    const struct tg_gpu_gr_work *,const struct tg_gpu_gr_context_status *,
    TG_GPU_GRAPHICS_TEMPLATE *);
/* CPU initialization only, while the context has no hardware references.
 * Never rebuild a live/saved context for an ordinary repeated submission.
 */
NTSTATUS TgGpuBuildContextGraphics(const TG_GPU_GRAPHICS_TEMPLATE *,
    const TG_GPU_PRIVATE_MEMORY *Instance,UINT64 InternalBase,UINT ContextId);
/* Caller owns the engine slot: the previous channel/GR context must already
 * be retired. Install only KMD mappings, preserving the three command pages.
 * A root TLB invalidate is still required before binding the new context.
 */
NTSTATUS TgGpuMapContextGraphics(TG_GPU_INTERNAL_VA *,const TG_GPU_PRIVATE_MEMORY *);
/* With the context idle, remove only its own stale mapping before freeing.
 * A different active context's mapping is preserved. No MMIO is performed.
 */
VOID TgGpuUnmapContextGraphics(TG_GPU_INTERNAL_VA *,const TG_GPU_PRIVATE_MEMORY *);
#endif
