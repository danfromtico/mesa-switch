/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_ADAPTER_ABI_H
#define TG_GPU_ADAPTER_ABI_H
#include <stdint.h>

/* Shared by the Windows NVK backend and the Tegra KMD. A budget ceiling, not
 * reserved physical RAM or a promise that VidMm can make all of it resident.
 */
#define TG_GPU_MEMORY_BUDGET_BYTES (UINT64_C(512)*1024*1024)
#define TG_GPU_ADAPTER_ABI 1u
#define TG_GPU_ADAPTER_LUID_VALID 1u
#define TG_GPU_ADAPTER_NATIVE_SUBMIT 2u
/* KMTQAITYPE_UMDRIVERPRIVATE returns adapter info. Callers initialize the
 * header for Windows paths that forward input to the miniport. */
struct tg_gpu_adapter_query {uint32_t version,size;};
struct tg_gpu_adapter_info {
    uint32_t version, size, flags, reserved;
    uint64_t memory_budget_bytes;
    uint32_t vendor, chipset, sm, gr_class;
    uint32_t luid_low;
    int32_t luid_high;
};
_Static_assert(sizeof(struct tg_gpu_adapter_info)==48,"KMD/NVK adapter ABI");
#endif
