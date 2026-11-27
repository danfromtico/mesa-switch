/* SPDX-License-Identifier: MIT
 * TSG/channel layout follows Switchroot nvgpu 1ae0167d,
 * gk20a_get_tsg_runlist_entry and gk20a_runlist_construct_locked; see NOTICE.
 */
#ifndef TEGRA_GPU_RUNLIST_H
#define TEGRA_GPU_RUNLIST_H
#include <stdint.h>

#define TG_GPU_RUNLIST_ENTRIES 2u
#define TG_GPU_PREEMPT_TSG0 0x01000000u

/* The backend serializes work on hardware channel zero in TSG zero. The
 * vendor submission path requires a TSG before its channel entries; copying
 * get_ch_runlist_entry alone omits the scheduler's group and timeslice.
 * Four words = two 8-byte entries, not two runnable channels.
 */
static inline void tg_gpu_runlist_single(uint32_t words[4])
{
    words[0] = (1u << 26) | (128u << 18) | (3u << 14) | 0x2000u;
    words[1] = 0;
    words[2] = 0; /* CHID zero; CCSR supplies the instance address */
    words[3] = 0;
}
#endif
