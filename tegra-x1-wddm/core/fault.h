/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_FAULT_H
#define TG_GPU_FAULT_H
#include "../platform/t214_power.h"
#include "../platform/t214_mmu.h"
struct tg_gpu_fault_status { uint32_t fifo, mmu, pbdma, pbdma1; };
/* Classify without altering the raw evidence. ENGINE alone is a notification,
 * even if it reasserts; an unexplained PBDMA summary remains a fault. */
bool tg_gpu_fault_pending(const struct tg_gpu_fault_status *status);
/* Poll owned PBDMA0. Acknowledge only a lone ENGINE notification following
 * Switchroot; preserve genuine or unknown faults for recovery and diagnosis.
 * Checked variant stops before the next PRI read or W1C write once guard aborts.
 */
bool tg_gpu_fault_poll(const struct tg_gpu_power_io *io, struct tg_gpu_fault_status *status);
bool tg_gpu_fault_poll_checked(const struct tg_gpu_power_io *io,
    struct tg_gpu_fault_status *status, const struct tg_gpu_commit_guard *guard);
#endif
