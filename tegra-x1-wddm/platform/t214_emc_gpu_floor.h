/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2014-2019 NVIDIA CORPORATION.
 * Copyright (c) 2021-2023 CTCaer.
 * T210B01 EMC-to-GPU voltage characterization and process-bin thresholds.
 * Source: CTCaer/switch-l4t-kernel-4.9
 * 2d0059fd3167a8df756de2aa0489d4aa70a9fc15,
 * drivers/soc/tegra/tegra210-dvfs.c and drivers/soc/tegra/fuse/speedo-tegra210.c.
 */
#ifndef TG_GPU_EMC_FLOOR_POLICY_H
#define TG_GPU_EMC_FLOOR_POLICY_H
#include <stdint.h>
static inline uint32_t tg_gpu_emc_floor_process_uv(uint32_t process, uint32_t emc_khz)
{
    /* Switchroot 2d0059fd3167a8df756de2aa0489d4aa70a9fc15,
     * tegra210-dvfs.c gpub01_dvfs_table and speedo-tegra210.c B01 thresholds.
     * The earlier single process-1 row was insufficient on other Lite SoCs. */
    static const uint16_t mv[] = {
        590,600,610,620,630,640,650,660,670,680,690,700,710,720,730,740,760
    };
    static const uint16_t mhz[3][17] = {
        {2166,2233,2300,2366,2400,2466,2533,2566,2633,2700,2766,2833,2900,2966,3033,3100,3200},
        {2233,2333,2366,2433,2500,2533,2600,2666,2733,2800,2866,2933,3000,3066,3133,3200,3200},
        {2466,2500,2600,2633,2666,2733,2800,2866,2900,2966,3033,3100,3166,3200,3200,3200,3200},
    };
    uint32_t i;
    if (process >= 3u || !emc_khz || emc_khz > 3200000u) return UINT32_MAX;
    for (i = 0; i < sizeof(mv) / sizeof(mv[0]); ++i)
        if (emc_khz <= (uint32_t)mhz[process][i] * 1000u) return (uint32_t)mv[i] * 1000u;
    return UINT32_MAX;
}
static inline uint32_t tg_gpu_emc_floor_policy_uv(uint32_t gpu_speedo, uint32_t emc_khz)
{
    uint32_t process;
    if (!gpu_speedo || gpu_speedo > 5000u) return UINT32_MAX;
    process = gpu_speedo < 1584u ? 0u : gpu_speedo < 1695u ? 1u : 2u;
    return tg_gpu_emc_floor_process_uv(process, emc_khz);
}

#endif
