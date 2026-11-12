/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_SUBMISSION_H
#define TEGRA_GPU_SUBMISSION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_GPU_COMMAND_CAPTURE 0x4000u
#define TG_GPU_COMMAND_CAPTURE_BYTES (64u*1024u)
#define TG_GPU_COMMAND_QMD (TG_GPU_COMMAND_CAPTURE+TG_GPU_COMMAND_CAPTURE_BYTES)
#define TG_GPU_COMMAND_QMD_BYTES TG_GPU_COMMAND_CAPTURE_BYTES
#define TG_GPU_COMMAND_BYTES (TG_GPU_COMMAND_QMD+TG_GPU_COMMAND_QMD_BYTES)
#define TG_GPU_COMMAND_RING 0u
#define TG_GPU_COMMAND_PUSH 0x1000u
#define TG_GPU_COMMAND_USERD 0x2000u
#define TG_GPU_COMMAND_FENCE (TG_GPU_COMMAND_USERD+0x48u) /* USERD.Reference */
#define TG_GPU_COMMAND_RUNLIST 0x3000u
/* Driver-owned ring/USERD privileged pages, a read-only fetchable packet,
 * and a physical runlist. WFI + SET_REFERENCE updates USERD through the host
 * channel rather than a virtual semaphore store to its privileged PTE.
 * The application's packet stays USER; only the generated completion packet
 * is KERNEL. Caller owns all backing and must quiesce the hardware slot before
 * preparing it. QMDs use a separate writable data mirror: DFALCON writes
 * them during dispatch, while the captured command stream stays read-only.
 * RAMIN PDB words are preserved, not exposed in the mapped pages.
 */
bool tg_gpu_prepare_submission(void *commands, size_t bytes, uint64_t physical,
    uint64_t internal_va, void *instance, size_t instance_bytes,
    uint64_t instance_physical, uint64_t application_va, uint32_t application_bytes,
    uint32_t fence_value);
/* Optional indirect GPFIFO segment (SYNC_WAIT, USER) and continuation suffix
 * (PROCEED, USER) inside application_va..application_va+application_bytes.
 * Writes the total GPFIFO entry count (2..4) to *gp_put_out when non-NULL.
 */
bool tg_gpu_prepare_indirect_submission(void *commands, size_t bytes, uint64_t physical,
    uint64_t internal_va, void *instance, size_t instance_bytes,
    uint64_t instance_physical, uint64_t application_va, uint32_t application_bytes,
    uint64_t indirect_va, uint32_t indirect_bytes, uint32_t suffix_offset,
    uint32_t fence_value, uint32_t *gp_put_out);
#endif
