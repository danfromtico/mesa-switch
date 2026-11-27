/* SPDX-License-Identifier: MIT */
#include "../platform/t214_frontend.h"
#include "../platform/t214_power.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t regs[0x420000 / 4];
static unsigned writes, flushes, busy_reads, fail_flush, secure_writes, irq_masked;
static uint32_t fault_on;
static unsigned l2_flushes, l2_invalidations;
static uint32_t fail_l2;
static uint32_t ignore_write;
static unsigned ce_delays, pri_timeout_reads;
static uint32_t read32(void *ctx, uint32_t address)
{
    uint32_t reg = address - 0x57000000;
    (void)ctx;
    assert(address >= 0x57000000 && reg < sizeof(regs));
    if (reg == 0x9084 || reg == 0x9088 || reg == 0x908c) ++pri_timeout_reads;
    if (reg == 0x70000) {
        if (fail_flush == flushes && flushes) return 3;
        if (busy_reads) { --busy_reads; return 3; }
        return 0;
    }
    if (reg == 0x70010 || reg == 0x70004) return reg == fail_l2 ? 3 : 0;
    return regs[reg / 4];
}
static void write32(void *ctx, uint32_t address, uint32_t value)
{
    uint32_t reg = address - 0x57000000;
    (void)ctx;
    assert(address >= 0x57000000 && reg < sizeof(regs));
    assert(reg != 0x100d1c && reg != 0x21434);
    assert(reg != 0x9084 && reg != 0x9088 && reg != 0x908c); /* never clear PRI timeout latches */
    ++writes;
    if (reg == ignore_write) return;
    if (reg == 0x106f28) assert((regs[0x200/4]&0x200000) && ce_delays==1 && value==0x7fe);
    if (reg >= 0x400000)
        assert((regs[0x200 / 4] & 0x18001000) == 0x18001000);
    if (reg == 0x140 || reg == 0x144) { assert(value == 0); ++irq_masked; }
    if (reg == 0x1140) assert(irq_masked == 2 && value == 0xe);
    if (reg == 0x1100) { regs[reg / 4] &= ~value; return; }
    if (reg == 0x70000) {
        assert(value == 1 && regs[0x100c10 / 4] && (regs[0x1704 / 4] & 0x80000000));
        ++flushes; busy_reads = 3;
    }
    if (reg == 0x70010) { assert(value == 1 && flushes >= 2); ++l2_flushes; }
    if (reg == 0x70004) { assert(value == 1 && l2_flushes > l2_invalidations); ++l2_invalidations; }
    if (reg == 0x100ce4) { assert(!regs[0x21434 / 4]); ++secure_writes; }
    regs[reg / 4] = value;
    if (reg == fault_on) {
        regs[0x120058 / 4] = 0x100;
        regs[0x122120 / 4] = address;
        regs[0x12212c / 4] = 0xbad;
    }
}
static void delay(void *ctx, uint32_t us) {
    (void)ctx;
    if (us==500) { assert(!(regs[0x200/4]&0x200000)); ++ce_delays; }
    else assert(us<=20);
}
static void reset(void)
{
    memset(regs, 0, sizeof(regs));
    writes = flushes = busy_reads = fail_flush = secure_writes = irq_masked = 0;
    fault_on = UINT32_MAX;
    ignore_write = UINT32_MAX;
    fail_l2 = l2_flushes = l2_invalidations = 0;
    ce_delays = pri_timeout_reads = 0;
    regs[0x22454 / 4] = regs[0x12006c / 4] = 1;
    regs[0x21434 / 4] = 1;
    regs[0x140518 / 4] = 0xa0;
    regs[0x17e20c / 4] = 0x40300008;
    regs[0x2a04 / 4] = 0x80000000;
}
int main(void)
{
    struct tg_gpu_power_io io = {NULL,read32,write32,NULL,NULL,delay, NULL};
    struct tg_gpu_power power = {0};
    struct tg_gpu_frontend_status s = {0};
    reset();
    assert(tg_gpu_frontend_start(&io, &power, &s) == TG_GPU_FE_ARGUMENT && !writes);
    power.owned = power.ready = true;
    assert(tg_gpu_frontend_start(&io, &power, &s) == TG_GPU_FE_OK);
    assert(tg_gpu_frontend_fifo(&io, &power, &s) == TG_GPU_FE_OK);
    assert(regs[0x2a04 / 4] == 0xbfffffff && regs[0x4012c / 4] == UINT32_MAX);
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_OK);
    assert(s.stage == 4 && s.ltc_count == 1 && !secure_writes);
    assert(regs[0x17e27c / 4] == 1 && regs[0x17e000 / 4] == 1 && regs[0x100800 / 4] == 1);
    assert(regs[0x17e318 / 4] == 0x80a0 && regs[0x17e20c / 4] == 8);
    regs[0x100c10 / 4] = 0xc000046; regs[0x1704 / 4] = 0xb00c0040;
    assert(tg_gpu_frontend_flush(&io, &power, &s) == TG_GPU_FE_OK);
    assert(s.stage == 6 && s.fb_flushes == 2 && flushes == 2);
    assert(tg_gpu_frontend_sync_memory(&io, &power, &s) == TG_GPU_FE_OK);
    assert(s.stage == 9 && s.l2_flushes == 1 && s.l2_invalidations == 1);
    fail_l2 = 0x70010;
    assert(tg_gpu_frontend_sync_memory(&io, &power, &s) == TG_GPU_FE_L2_TIMEOUT);
    assert(s.stage == 7 && s.l2_invalidations == 1 && l2_invalidations == 1);
    fail_l2 = 0x70004;
    assert(tg_gpu_frontend_sync_memory(&io, &power, &s) == TG_GPU_FE_L2_TIMEOUT);
    assert(s.stage == 8 && s.l2_flushes == 2 && s.l2_invalidations == 1);

    reset(); memset(&s,0,sizeof(s)); regs[0x21434 / 4] = 0;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_OK && secure_writes == 1);
    reset(); memset(&s,0,sizeof(s)); regs[0x12006c / 4] = 0;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_LTC && !writes);
    reset(); memset(&s,0,sizeof(s)); regs[0x21434 / 4] = UINT32_MAX;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_REGISTER && !secure_writes);
    reset(); memset(&s,0,sizeof(s)); fault_on = 0x17e000;
    assert(tg_gpu_frontend_memory(&io, &power, &s) == TG_GPU_FE_PRI_FAULT);
    assert(s.stage == 3 && s.pri_error_address == 0x5717e000 && s.pri_error_code == 0xbad);
    assert(!regs[0x20c / 4]); /* fail before memory-client release */
    reset(); memset(&s,0,sizeof(s));
    regs[0x100c10 / 4] = 0xc000046; regs[0x1704 / 4] = 0xb00c0040; fail_flush=2;
    assert(tg_gpu_frontend_flush(&io, &power, &s) == TG_GPU_FE_FLUSH_TIMEOUT);
    assert(s.stage == 5 && s.fb_flushes == 1 && flushes == 2);
    reset();memset(&s,0,sizeof(s));
    assert(tg_gpu_frontend_gr_prepare(&io,&power,&s)==TG_GPU_FE_REGISTER && !writes);
    regs[0x200/4]=0xc0012120;regs[0x20200/4]=0xa5a50126;regs[0x20204/4]=0x5a5a0215;
    assert(tg_gpu_frontend_gr_prepare(&io,&power,&s)==TG_GPU_FE_OK);
    assert(s.stage==11 && s.gr_enable==0xd8013120 && s.gr_fifo_access==0x10001);
    assert(s.gr_gate_ctrl[0]==0xa5a50114 && s.gr_gate_ctrl[1]==0x5a5a0214);
    assert(regs[0x4041f4/4]==0x03fffffe && regs[0x419ffc/4]==0x1fffe);
    assert(regs[0x4041f0/4]==0 && regs[0x4089b8/4]==0);
    assert(!irq_masked && !secure_writes); /* no IRQ, fuse or PMU/Falcon start */
    regs[0x100c10/4]=0xc000046; regs[0x1704/4]=0xb00c0040;
    assert(tg_gpu_frontend_sync_memory(&io,&power,&s)==TG_GPU_FE_OK);
    assert(s.stage==11 && s.gr_fifo_access==0x10001 && s.l2_flushes==1);
    fail_l2=0x70010;
    assert(tg_gpu_frontend_sync_memory(&io,&power,&s)==TG_GPU_FE_L2_TIMEOUT);
    assert(s.stage==7 && s.result==TG_GPU_FE_L2_TIMEOUT);
    reset();memset(&s,0,sizeof(s));regs[0x200/4]=0x100;ignore_write=0x200;
    assert(tg_gpu_frontend_gr_prepare(&io,&power,&s)==TG_GPU_FE_REGISTER);
    assert(s.stage==10 && !regs[0x4041f4/4] && !s.gr_fifo_access);
    reset();memset(&s,0,sizeof(s));regs[0x200/4]=0x100;fault_on=0x4041f4;
    assert(tg_gpu_frontend_gr_prepare(&io,&power,&s)==TG_GPU_FE_PRI_FAULT);
    assert(s.stage==10 && s.pri_error_address==0x574041f4 && !s.gr_fifo_access);
    reset();memset(&s,0,sizeof(s));regs[0x200/4]=0x100;ignore_write=0x20200;
    assert(tg_gpu_frontend_gr_prepare(&io,&power,&s)==TG_GPU_FE_REGISTER && !s.gr_fifo_access);
    reset();memset(&s,0,sizeof(s));regs[0x200/4]=0x100;ignore_write=0x400500;
    assert(tg_gpu_frontend_gr_prepare(&io,&power,&s)==TG_GPU_FE_REGISTER && s.stage==10);
    /* Discover CE2 from a chained TOP record before releasing its reset.
     * An unsupported/missing topology must not toggle unrelated MC units. */
    for (unsigned test=0;test<6;++test) {
        uint32_t ce_mask=0;
        reset();s=(struct tg_gpu_frontend_status){.stage=11};regs[0x200/4]=0xd8013120;
        regs[0x22700/4]=0x80001836;regs[0x22704/4]=3; /* graphics/reset 12 */
        regs[0x22708/4]=0x84002a36;regs[0x2270c/4]=0xf; /* CE2/reset 21, runlist 0 */
        if (test==1) regs[0x2270c/4]=0; /* no recognized copy engine */
        if (test==2) regs[0x22708/4]=0x84001836; /* wrong reset selector */
        if (test==3) regs[0x22700/4]=UINT32_MAX;
        if (test==4) ignore_write=0x200;
        if (test==5) fault_on=0x106f28;
        enum tg_gpu_frontend_result wanted=test==0?TG_GPU_FE_OK:
            test<3?TG_GPU_FE_TOPOLOGY:test<5?TG_GPU_FE_REGISTER:TG_GPU_FE_PRI_FAULT;
        assert(tg_gpu_frontend_ce_prepare(&io,&power,&s,&ce_mask)==wanted);
        if (test==0) {
            assert(ce_mask==0x200000 && s.gr_enable==0xd8213120 && s.stage==11);
            assert(ce_delays==1 && regs[0x106f28/4]==0x7fe && !irq_masked && !secure_writes);
        } else if (test<4) assert(!writes && !ce_mask && !ce_delays);
        else assert(ce_mask==0x200000 && ce_delays==1 && s.stage==13);
    }
    /* Non-destructive PBUS-gated read-only observation of 0x9084/0x9088/0x908c. */
    {
        struct tg_gpu_pri_timeout_snapshot p;
        reset();
        regs[0x9084/4] = 0x12345678u;
        regs[0x9088/4] = 0x89abcdefu;
        regs[0x908c/4] = 0xdead0001u;
        tg_gpu_frontend_pri_timeout_snapshot(&io, 0, &p);
        assert(!p.valid && !p.fecs_valid && !pri_timeout_reads && !writes);
        tg_gpu_frontend_pri_timeout_snapshot(&io, UINT32_MAX, &p);
        assert(!p.valid && !p.fecs_valid && !pri_timeout_reads && !writes);
        tg_gpu_frontend_pri_timeout_snapshot(&io, 0x8u, &p);
        assert(p.valid && !p.fecs_valid && p.bus_interrupt == 0x8u);
        assert(p.save0 == 0x12345678u && p.save1 == 0x89abcdefu && !p.fecs_errcode);
        assert(pri_timeout_reads == 2 && !writes);
        assert(regs[0x9084/4] == 0x12345678u && regs[0x9088/4] == 0x89abcdefu);
        regs[0x9084/4] = 0x80400100u; /* bit 31 FECS_TGT set */
        pri_timeout_reads = 0;
        tg_gpu_frontend_pri_timeout_snapshot(&io, 0x4u, &p);
        assert(p.valid && p.fecs_valid && p.bus_interrupt == 0x4u);
        assert(p.save0 == 0x80400100u && p.save1 == 0x89abcdefu && p.fecs_errcode == 0xdead0001u);
        assert(pri_timeout_reads == 3 && !writes);
        assert(regs[0x9084/4] == 0x80400100u && regs[0x9088/4] == 0x89abcdefu);
    }
    puts("PASS: cold frontend/GR/CE ordering, topology, secure policy, bounded cache flush and PRI faults (model only)");
    return 0;
}
