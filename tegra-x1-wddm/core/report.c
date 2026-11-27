/* SPDX-License-Identifier: MIT */
#include "report.h"
#include "gm20b_mmu.h"
#include "nouveau/horizon/nouveau_horizon_gm20b.h"

_Static_assert(TG_GPU_REPORT_WORDS == NOUVEAU_HORIZON_GM20B_REPORT_WORDS,
               "mesa-switch report packet size changed");

bool
tg_gpu_build_report(uint32_t *commands, size_t capacity_words,
                     uint64_t report_va, uint32_t value)
{
    if (!commands || capacity_words < TG_GPU_REPORT_WORDS ||
        (report_va & 3u) != 0 || report_va >= TG_GPU_VA_LIMIT)
        return false;
    return nouveau_horizon_gm20b_build_report(commands, report_va, value) ==
           TG_GPU_REPORT_WORDS;
}
