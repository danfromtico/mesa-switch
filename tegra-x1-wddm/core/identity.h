/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TEGRA_GPU_IDENTITY_H
#define TEGRA_GPU_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

/* NVIDIA nvgpu: GK20A_GPUID_GM20B / GK20A_GPUID_GM20B_B. */
#define TG_GPU_CHIP_GM20B   0x12bu
#define TG_GPU_CHIP_GM20B_B 0x12eu

struct tg_gpu_identity {
    uint32_t chipset;
    uint8_t major_revision;
    uint8_t minor_revision;
};

/* Decode the real PMC_BOOT_0 value; do not substitute the older GM20B ID.
 * An identity match does not establish firmware compatibility or execution.
 */
static inline bool
tg_gpu_identify(uint32_t boot0, struct tg_gpu_identity *identity)
{
    if (!identity)
        return false;
    identity->chipset = (boot0 >> 20) & 0x1ffu;
    identity->major_revision = (uint8_t)((boot0 >> 4) & 0xfu);
    identity->minor_revision = (uint8_t)(boot0 & 0xfu);
    return identity->chipset == TG_GPU_CHIP_GM20B ||
           identity->chipset == TG_GPU_CHIP_GM20B_B;
}

#endif
