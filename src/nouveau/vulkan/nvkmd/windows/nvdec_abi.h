/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_NVDEC_QUERY_ABI_H
#define TG_GPU_NVDEC_QUERY_ABI_H
#include "adapter_abi.h"

#define TG_NVDEC_QUERY_ABI 0x00010001u
#define TG_NVDEC_QUERY_RESOURCES_VALID 1u
#define TG_NVDEC_QUERY_MAPPED 2u
#define TG_NVDEC_QUERY_BOOT_REGS_VALID 4u

enum tg_nvdec_resource_status {
    TG_NVDEC_RESOURCES_ABSENT = 0,
    TG_NVDEC_RESOURCES_PARTIAL,
    TG_NVDEC_RESOURCES_INVALID,
    TG_NVDEC_RESOURCES_MAP_FAILED,
    TG_NVDEC_RESOURCES_REGS_INVALID,
    TG_NVDEC_RESOURCES_CAPTURED,
};

/* Immutable adapter-start snapshot, before graphics power-up. These flags
 * describe resource ownership/readback only, not firmware or decode support.
 * A future DMA owner must recheck live state under its lifetime/clock locks.
 */
struct tg_nvdec_resource_info {
    uint32_t version, size, flags, resource_mask;
    uint32_t status, hidrev, powergate_status, clock_y;
    uint32_t reset_y, nvdec_clock_source, nvjpg_clock_source;
    uint32_t smmu_config, smmu_host1x_asid, smmu_nvdec_asid, smmu_nvjpg_asid;
    uint32_t map_status;
};

#define TG_NVDEC_CLIENT_QUERY_ABI 0x00010001u
#define TG_NVDEC_CLIENT_REGS_VALID 1u
#define TG_NVDEC_CLIENT_TRANSLATION_DISABLED 2u

/* Separate, append-only trailer: a readable client route must not be confused
 * with a readable global SMMU_CONFIG (which may be secure-world-only). A clear
 * route is a prerequisite for the direct-address path, not DMA completion or
 * decode support. These are boot observations, not a live ownership lease. */
struct tg_nvdec_client_info {
    uint32_t version, size, flags;
    uint32_t smmu_translation_enable0, smmu_translation_enable1;
    uint32_t smmu_translation_enable3, mc_flush, mc_flush_status;
    uint32_t pmc_clamp_status, pmc_toggle;
};

/* Optional trailer on the existing private adapter query. The request's
 * base.size remains sizeof(tg_gpu_adapter_info), including on older KMDs.
 * Zero-initialize the trailer: older KMDs leave it untouched. */
struct tg_gpu_adapter_nvdec_info {
    struct tg_gpu_adapter_info base;
    struct tg_nvdec_resource_info nvdec;
};
struct tg_gpu_adapter_nvdec_clients_info {
    struct tg_gpu_adapter_nvdec_info resource;
    struct tg_nvdec_client_info clients;
};
_Static_assert(sizeof(struct tg_nvdec_resource_info) == 64, "NVDEC snapshot ABI");
_Static_assert(sizeof(struct tg_gpu_adapter_nvdec_info) == 112, "adapter trailer ABI");
_Static_assert(sizeof(struct tg_nvdec_client_info) == 40, "NVDEC clients ABI");
_Static_assert(sizeof(struct tg_gpu_adapter_nvdec_clients_info) == 152, "client trailer ABI");
#endif
