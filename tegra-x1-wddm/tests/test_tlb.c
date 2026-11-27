/* SPDX-License-Identifier: MIT */
#include "../platform/t214_mmu.h"
#include <assert.h>
#include <stdio.h>
static unsigned writes, delays, mode, post_reads, reads_after_fault;
static uint32_t observed_root;
static bool fault_latched, fault_on_space, fault_at_commit, fault_after_trigger, fault_after_drain;
static bool tlb_abort(void *ctx) { (void)ctx; return fault_latched; }
static bool tlb_acquire(void *ctx)
{
    (void)ctx;
    if (fault_at_commit) fault_latched = true;
    return !fault_latched;
}
static void tlb_write32(void *ctx, const struct tg_gpu_power_io *io,
    uint32_t reg, uint32_t val, bool final_step)
{
    (void)ctx; (void)final_step;
    io->write32(io->context, reg, val);
}
static uint32_t read32(void *ctx, uint32_t address)
{
    (void)ctx; assert(address == 0x57100c80);
    if (fault_latched) ++reads_after_fault;
    if (fault_on_space && !writes) fault_latched = true;
    if (fault_after_drain && writes == 2) fault_latched = true;
    if (mode == 1) return 0;
    if (mode == 3 || (mode == 4 && writes)) return UINT32_MAX;
    if (mode == 5 && writes) return ++post_reads < 3 ? 0x10000 : 0x18000;
    return writes && mode != 2 ? 0x18000 : 0x10000;
}
static void write32(void *ctx, uint32_t address, uint32_t value)
{
    (void)ctx;
    if (writes++ == 0) { assert(address == 0x57100cb8); observed_root = value; }
    else {
        assert(address == 0x57100cbc && value == 0x80000001);
        if (fault_after_trigger) fault_latched = true;
    }
}
static void delay(void *ctx, uint32_t us) { (void)ctx; assert(us == 2); ++delays; }
int main(void)
{
    struct tg_gpu_power power = {.owned=true, .ready=true};
    struct tg_gpu_power_io io = {NULL, read32, write32, NULL, NULL, delay, NULL};
    assert(tg_gpu_tlb_invalidate(&io, &power, UINT64_C(0x170020000)) == TG_GPU_TLB_OK);
    assert(observed_root == 0x01700200 && writes == 2 && delays == 0);
    writes = delays = 0; mode = 1;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_NO_SPACE);
    assert(writes == 0 && delays == 10000);
    mode = 2; delays = 0;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_TIMEOUT);
    assert(writes == 2 && delays == 10000);
    writes = 0; mode = 3;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_IO_ERROR);
    assert(writes == 0);
    mode = 4;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_IO_ERROR);
    assert(writes == 2);
    writes = delays = post_reads = 0; mode = 5;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_OK);
    assert(writes == 2 && post_reads == 3 && delays == 1);
    writes = 0; mode = 0; power.ready = false;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_NOT_READY);
    assert(writes == 0);
    assert(tg_gpu_tlb_invalidate(&io, &power, 1) == TG_GPU_TLB_ARGUMENT);
    /* B11 / T2: Fault at initial SPACE, at commit gate, right after TRIGGER,
     * or right after drain read MUST never issue any post-fault FB_MMU reads! */
    power.ready = true;
    const struct tg_gpu_commit_guard guard = {tlb_abort, tlb_acquire, tlb_write32, NULL};
    writes = reads_after_fault = 0; fault_latched = false;
    fault_on_space = true; fault_at_commit = fault_after_trigger = fault_after_drain = false;
    assert(tg_gpu_tlb_invalidate_checked(&io, &power, 0xc0000000, &guard) == TG_GPU_TLB_FAULT);
    assert(writes == 0 && reads_after_fault == 0);

    writes = reads_after_fault = 0; fault_latched = false;
    fault_on_space = false; fault_at_commit = true;
    assert(tg_gpu_tlb_invalidate_checked(&io, &power, 0xc0000000, &guard) == TG_GPU_TLB_FAULT);
    assert(writes == 0 && reads_after_fault == 0);

    writes = reads_after_fault = 0; fault_latched = false;
    fault_at_commit = false; fault_after_trigger = true;
    assert(tg_gpu_tlb_invalidate_checked(&io, &power, 0xc0000000, &guard) == TG_GPU_TLB_FAULT);
    assert(writes == 2 && reads_after_fault == 0);

    writes = reads_after_fault = post_reads = 0; mode = 5; fault_latched = false;
    fault_after_trigger = false; fault_after_drain = true;
    assert(tg_gpu_tlb_invalidate_checked(&io, &power, 0xc0000000, &guard) == TG_GPU_TLB_FAULT);
    assert(writes == 2 && post_reads == 1 && reads_after_fault == 0);

    writes = reads_after_fault = post_reads = 0; mode = 0; fault_latched = false;
    fault_after_drain = false;
    assert(tg_gpu_tlb_invalidate_checked(&io, &power, 0xc0000000, &guard) == TG_GPU_TLB_OK);
    assert(writes == 2 && reads_after_fault == 0);
    puts("GM20B TLB sequencing and timeout tests passed (host model).");
    return 0;
}
