/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2014-2018 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * Switchroot nvgpu 1ae0167d common/fb/fb_gm20b.c; see NOTICE.
 */
#include "t214_mmu.h"
#include "../core/gm20b_mmu.h"
#include <stddef.h>

static bool tlb_aborted(const struct tg_gpu_commit_guard *guard)
{
    return guard && guard->abort && guard->abort(guard->context);
}

static void tlb_commit_write(const struct tg_gpu_commit_guard *guard,
    const struct tg_gpu_power_io *io, uint32_t reg, uint32_t val, bool final_step)
{
    if (guard && guard->write32)
        guard->write32(guard->context, io, reg, val, final_step);
    else
        io->write32(io->context, reg, val);
}

enum tg_gpu_tlb_result tg_gpu_tlb_invalidate_checked(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint64_t root,
    const struct tg_gpu_commit_guard *guard)
{
    uint32_t i, status;
    if (!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
        root >= TG_GPU_VA_LIMIT || (root & 0xfff)) return TG_GPU_TLB_ARGUMENT;
    if (!power->owned || !power->ready) return TG_GPU_TLB_NOT_READY;
    for (i = 0; i < 10000; ++i) {
        if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
        status = io->read32(io->context, 0x57100c80u);
        if (status == UINT32_MAX) return TG_GPU_TLB_IO_ERROR;
        if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
        if (status & 0x00ff0000u) break;
        io->delay_us(io->context, 2);
    }
    if (i == 10000) return TG_GPU_TLB_NO_SPACE;
    /* Resolve nvgpu_aperture_mask, not just its sysmem argument: the GM20B
     * platform has honors_aperture=false, so SoC DRAM selects VIDMEM (0).
     * Arbitrate software commit permission at DIRQL with zero MMIO (V5-01),
     * then issue PDB and TRIGGER at caller IRQL with per-MMIO gates & breadcrumbs (B11/N3).
     */
    const uint32_t pdb = (uint32_t)(root >> 8);
    if (guard && guard->acquire && !guard->acquire(guard->context))
        return TG_GPU_TLB_FAULT;
    if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
    tlb_commit_write(guard, io, 0x57100cb8u, pdb, false);
    if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
    tlb_commit_write(guard, io, 0x57100cbcu, 0x80000001u, true); /* ALL_VA + trigger */
    /* Gate every post-trigger FB_MMU status read BEFORE issuing MMIO so a fault
     * latched after TRIGGER never issues new FB_MMU reads (B11). */
    if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
    status = io->read32(io->context, 0x57100c80u);
    if (status == UINT32_MAX) return TG_GPU_TLB_IO_ERROR;
    if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
    for (i = 0; i < 10000; ++i) {
        if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
        status = io->read32(io->context, 0x57100c80u);
        if (status == UINT32_MAX) return TG_GPU_TLB_IO_ERROR;
        if (tlb_aborted(guard)) return TG_GPU_TLB_FAULT;
        if (status & 0x8000u) return TG_GPU_TLB_OK;
        io->delay_us(io->context, 2);
    }
    return TG_GPU_TLB_TIMEOUT;
}
enum tg_gpu_tlb_result tg_gpu_tlb_invalidate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint64_t root)
{
    return tg_gpu_tlb_invalidate_checked(io, power, root, NULL);
}
