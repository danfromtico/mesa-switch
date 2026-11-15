/* SPDX-License-Identifier: MIT */
#include "../platform/t214_mmu.h"
#include <assert.h>
#include <stdio.h>
static unsigned writes, delays, mode;
static uint32_t observed_root;
static uint32_t read32(void *ctx, uint32_t address)
{
    (void)ctx; assert(address == 0x57100c80);
    if (mode == 1) return 0;
    if (mode == 3 || (mode == 4 && writes)) return UINT32_MAX;
    return writes && mode != 2 ? 0x18000 : 0x10000;
}
static void write32(void *ctx, uint32_t address, uint32_t value)
{
    (void)ctx;
    if (writes++ == 0) { assert(address == 0x57100cb8); observed_root = value; }
    else { assert(address == 0x57100cbc && value == 0x80000001); }
}
static void delay(void *ctx, uint32_t us) { (void)ctx; assert(us == 2); ++delays; }
int main(void)
{
    struct tg_gpu_power power = {.owned=true, .ready=true};
    struct tg_gpu_power_io io = {NULL, read32, write32, NULL, NULL, delay, NULL};
    assert(tg_gpu_tlb_invalidate(&io, &power, UINT64_C(0x170020000)) == TG_GPU_TLB_OK);
    assert(observed_root == 0x01700200 && writes == 2);
    writes = delays = 0; mode = 1;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_NO_SPACE);
    assert(writes == 0 && delays == 1000);
    mode = 2; delays = 0;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_TIMEOUT);
    assert(writes == 2 && delays == 1000);
    writes = 0; mode = 3;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_IO_ERROR);
    assert(writes == 0);
    mode = 4;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_IO_ERROR);
    assert(writes == 2);
    writes = 0; power.ready = false;
    assert(tg_gpu_tlb_invalidate(&io, &power, 0xc0000000) == TG_GPU_TLB_NOT_READY);
    assert(writes == 0);
    assert(tg_gpu_tlb_invalidate(&io, &power, 1) == TG_GPU_TLB_ARGUMENT);
    puts("GM20B TLB sequencing and timeout tests passed (host model).");
    return 0;
}
