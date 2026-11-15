/* SPDX-License-Identifier: MIT */
#ifndef TG_EMC_GPU_VOTE_H
#define TG_EMC_GPU_VOTE_H
#include "windows/pmic.h"
#include <stdbool.h>
/* All accesses are serialized by the existing PMIC owner lock. A raised
 * floor becomes mandatory only after the GPU owner has changed/calibrated
 * its rail at an idle boundary. EMC cannot increase its rate before that ack.
 */
struct tg_emc_gpu_vote {
    uint32_t request, acknowledged, guard_uv, failed;
    uint32_t release_value;
    bool released, release_valid;
};
static inline bool tg_emc_gpu_request(struct tg_emc_gpu_vote *s, uint32_t uv,
                                      bool memory_down_complete)
{
    uint32_t code, sequence;
    if (uv && (uv < 590000u || uv > 760000u || (uv - 250000u) % 5000u)) return false;
    if (uv < s->guard_uv && !memory_down_complete) return false;
    /* A busy/quiescent-wait retry is still the same transaction. The GPU may
     * already be applying it; replacing its ID would reject a valid late ACK.
     * A failed request or a different floor starts a new transaction. */
    if (s->request && s->failed != s->request && TG_GPU_EMC_VOTE_UV(s->request) == uv)
        return true;
    code = uv ? (uv - 250000u) / 5000u : 0u;
    sequence = ((s->request >> 8) + 1u) & 0x00ffffffu;
    if (!sequence) sequence = 1u;
    /* Lowering is allowed only after BPMP GET_RATE confirmed the new clock.
     * The old GPU owner may keep a higher voltage until it sees this request. */
    if (uv < s->guard_uv) s->guard_uv = uv;
    s->request = (sequence << 8) | code;
    s->failed = 0;
    return true;
}
static inline bool tg_emc_gpu_ack(struct tg_emc_gpu_vote *s, uint32_t word,
                                  uint32_t actual_uv, bool enabled)
{
    uint32_t uv = TG_GPU_EMC_VOTE_UV(word);
    if (!word || word != s->request || word == s->failed ||
        (uv && (!enabled || actual_uv < uv))) return false;
    s->guard_uv = uv;
    s->acknowledged = word;
    return true;
}
#endif
