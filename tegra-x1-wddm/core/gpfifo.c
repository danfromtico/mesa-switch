/* SPDX-License-Identifier: MIT */
#include "gpfifo.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"

_Static_assert(sizeof(struct tg_gpu_gpfifo_entry) == NVB06F_GP_ENTRY__SIZE,
               "GPFIFO entry size mismatch");

bool
tg_gpu_build_gpfifo(uint64_t va, uint32_t words, bool no_prefetch,
                    struct tg_gpu_gpfifo_entry *entry)
{
    if (!entry || !words || (va & 3u) || va >= TG_GPU_VA_LIMIT ||
        words > DRF_MASK(NVB06F_GP_ENTRY1_LENGTH) ||
        words > (TG_GPU_VA_LIMIT - va) / 4)
        return false;
    entry->low = NVVAL(NVB06F, GP_ENTRY0, GET, va >> 2);
    entry->high = NVVAL(NVB06F, GP_ENTRY1, GET_HI, va >> 32) |
                  NVVAL(NVB06F, GP_ENTRY1, LENGTH, words) |
                  NVDEF(NVB06F, GP_ENTRY1, PRIV, USER) |
                  NVDEF(NVB06F, GP_ENTRY1, LEVEL, MAIN);
    if (no_prefetch)
        entry->high |= NVDEF(NVB06F, GP_ENTRY1, SYNC, WAIT);
    return true;
}
