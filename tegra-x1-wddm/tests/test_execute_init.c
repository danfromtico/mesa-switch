/* SPDX-License-Identifier: MIT */
#include "../core/execute.h"
#include "../core/gpfifo.h"
#include <assert.h>
#include <string.h>

struct fifo_model {
    uint32_t mc, instance, asid[2], pb, runlist_map, put;
    uint32_t sched, runlist_base, stall, fifo_fault, intr0, intr1, bar1_value;
    unsigned writes, channel_reads, channel_writes, asid_writes, ticks, enabled_at;
    bool fail_enable, unreadable_channel, fail_pb_enable, allow_mmu, runlist;
    bool fail_sched_allow, hce_fault;
    bool bad_ltc, flush_timeout, niso, bar1, bad_bar1_write, bar1_dirty, l2_timeout;
    unsigned fb_flushes, l2_flushes, l2_invalidations;
    uint32_t gr_access, gr_gate[2];
    bool fail_gr_enable;
    bool engine_event, stuck_event, late_pb_fault;
    bool secure, valid_pbdma;
    unsigned event_acks, summary_acks;
    uint32_t pb_fault, mmu_fault;
    bool live_gr_snapshot;
    unsigned gr_reads;
    bool simulated_gpu,notify_armed,fail_notify_arm;
    unsigned notify_generated,notify_waited,fail_notify_wait;
};
static uint32_t *arena_userd, *arena_runlist;
static uint32_t *arena_push,*arena_fence;
static struct tg_gpu_gpfifo_entry *arena_ring;
static uint32_t read32(void *context, uint32_t address)
{
    struct fifo_model *m = context;
    if(m->simulated_gpu && (address==0x58012000 || address==0x58012004))
        return arena_fence[(address-0x58012000)/4];
    if (address==0x57409800 || address==0x57400100 || address==0x57409b00 ||
        address==0x57409b04 || address==0x57409c18 || address==0x57409400 || address==0x57409818) {
        if (m->live_gr_snapshot) { ++m->gr_reads; return address^0x53570000; }
        return 0;
    }
    if (address == 0x57000200) return m->mc;
    if (address == 0x57000204) return m->pb;
    if (address == 0x57021434) return m->secure;
    if (address == 0x57400500) return m->gr_access;
    if (address == 0x57020200 || address == 0x57020204) return m->gr_gate[(address-0x57020200)/4];
    if (address == 0x57002390) return m->runlist_map;
    if (address == 0x57002630) return m->sched;
    if (address == 0x57002280) return m->runlist_base;
    if (address == 0x57002284) return m->runlist ? 2 : 0;
    if (address == 0x5704013c) return m->stall;
    if (address == 0x57040140) return 0xf;
    if (address == 0x5704010c) return m->intr0;
    if (address == 0x57003080) return m->valid_pbdma?0x10003000:0;
    if (address == 0x570025a0) return 1;
    if (address == 0x570400ac || address == 0x57040010 || address == 0x570400c0 ||
        address == 0x570400c4 || address == 0x57040084 || address == 0x57040118 ||
        address == 0x57040094) {
        assert(m->valid_pbdma);
        return address^0x5a5a0000;
    }
    if (address == 0x5704014c) return m->intr1;
    if (address == 0x57002100 && m->runlist && !(m->sched & 1)) return m->fifo_fault |
        ((m->pb_fault || (m->engine_event && (!m->summary_acks || m->stuck_event))) ? 0x20000000 : 0);
    if (address == 0x57040108 && m->runlist && !(m->sched & 1))
        return m->pb_fault | ((m->engine_event && (!m->event_acks || m->stuck_event)) ? 0x1000000 : 0);
    if (address == 0x5700259c) return m->runlist ? m->mmu_fault : 0;
    if (address == 0x5700254c) return (m->fifo_fault & 0x100) ? 0x0a : 0;
    if (address == 0x5700252c) return (m->fifo_fault & 1) ? 0x05 : 0;
    if (address == 0x5700256c) return (m->fifo_fault & 0x10000) ? 0x1234 : 0;
    if (address == 0x57002640) return 0x80008000;
    if (address == 0x57002648) return 0x2000;
    if (address == 0x5700262c) assert(!"RUNLIST_FAULT is not FIFO_INTR_SCHED_ERROR");
    if (address == 0x57040148 && m->hce_fault && m->runlist && !(m->sched & 1)) return 8;
    if (address == 0x57100c80 && m->allow_mmu) return 0x18000;
    if (address == 0x57022454) return 1;
    if (address == 0x5712006c) return m->bad_ltc ? 0 : 1;
    if (address == 0x57070000) return m->flush_timeout ? 3 : 0;
    if (address == 0x57070010) return m->l2_timeout ? 3 : 0;
    if (address == 0x58013000 && m->allow_mmu) return m->bar1_dirty ? m->bar1_value : arena_userd[0];
    if (address == 0x5801308c) return arena_userd[35];
    if (address == 0x57040048 || address == 0x57040050 ||
        address == 0x57040014 || address == 0x57040000)
        { assert(m->valid_pbdma); return 0; }
    if (address == 0x57800000 || address == 0x57800004) {
        /* Reading a reset FIFO cannot establish channel ownership. */
        assert((m->mc & 0x100) && m->ticks - m->enabled_at >= 20);
        ++m->channel_reads;
        return m->unreadable_channel ? UINT32_MAX :
            address == 0x57800000 ? m->instance : 0;
    }
    if (address == 0x70019aa8) return m->asid[0];
    if (address == 0x70019aac) return m->asid[1];
    /* No fake GPU completion: the MMU or first semaphore wait times out. */
    return 0;
}
static void write32(void *context, uint32_t address, uint32_t value)
{
    struct fifo_model *m = context;
    ++m->writes;
    if (address == 0x57000200) {
        if (value & 0x100) {
            assert(m->ticks >= 20);
            m->enabled_at = m->ticks;
        }
        m->mc = m->fail_enable ? value & ~0x100u : value;
        if (m->fail_gr_enable) m->mc &= ~0x1000u;
    } else if (address == 0x57000204) {
        assert(m->mc & 0x100);
        if (!(value & 1) && (m->pb & 1)) assert(!m->runlist && !m->instance);
        m->pb = m->fail_pb_enable ? value & ~1u : value;
    } else if (address == 0x57000140 || address == 0x57000144) {
        assert(!value); /* local fault enables must not turn on CPU IRQs */
    } else if (address == 0x57002274) {
        assert(m->sched & 1);
        if (value) {
            assert((m->mc&0x18001000)==0x18001000 && m->gr_access==0x10001);
            assert((m->gr_gate[0]&0x33)==0x10 && (m->gr_gate[1]&0x33)==0x10);
            assert((m->pb & 1) && m->instance && m->fb_flushes == 6 && m->l2_invalidations == 2);
            /* Decode independently: TSG zero, one member, timeout 128 at
             * scale 3; the following entry is CHID zero, not an instance.
             */
            assert(value == 2 && arena_runlist[1] == 0);
            assert((arena_runlist[0] & 0x3fff) == 0x2000);
            assert(((arena_runlist[0] >> 14) & 15) == 3);
            assert(((arena_runlist[0] >> 18) & 255) == 128);
            assert((arena_runlist[0] >> 26) == 1);
            assert(arena_runlist[2] == 0 && arena_runlist[3] == 0);
        }
        m->runlist = value != 0;
    } else if (address == 0x57002270) {
        assert(m->sched & 1);
        assert(value == 0x000c0045); /* physical runlist, VIDMEM target 0 */
        m->runlist_base = value;
    } else if (address == 0x57002630) {
        if (!(value & 1)) assert(m->runlist && m->instance);
        m->sched = m->fail_sched_allow ? value | 1 : value;
    } else if (address == 0x57020200 || address == 0x57020204) {
        assert(!m->runlist && !m->instance);m->gr_gate[(address-0x57020200)/4]=value;
    } else if (address == 0x57400500) {
        assert(!m->runlist && !m->instance && (m->mc&0x18001000)==0x18001000);
        m->gr_access=value;
    } else if (address == 0x5704013c) {
        m->stall = value;
    } else if (address == 0x5704010c) {
        assert(value == m->stall && !(value & 0x100)); m->intr0=value;
    } else if (address == 0x5704014c) {
        assert(value == 0xe); m->intr1=value;
    } else if (address == 0x57040108 && m->runlist) {
        assert(value == 0x1000000 && !m->pb_fault && !m->hce_fault && !m->mmu_fault && !m->fifo_fault);
        ++m->event_acks;
        if (m->late_pb_fault) m->pb_fault=0x8000;
    } else if (address == 0x57002100 && m->runlist) {
        assert(value == 0x20000000 && m->event_acks); ++m->summary_acks;
    } else if (address == 0x57100c10) {
        m->niso = value != 0;
    } else if (address == 0x57100cc8) {
        assert(value == 0x00c00480); /* debug WR, target 0 */
    } else if (address == 0x57100ccc) {
        assert(value == 0x00c00470); /* debug RD, target 0 */
    } else if (address == 0x57100cb8) {
        assert(value == 0x00c00000); /* invalidate PDB, target 0 */
    } else if (address == 0x57001704) {
        assert(value == 0 || value == 0x800c0040); /* BAR1 instance, target 0 */
        m->bar1 = (value & 0x80000000) != 0;
    } else if (address == 0x57070000) {
        assert(m->niso && m->bar1);
        ++m->fb_flushes;
    } else if (address == 0x57070010) {
        assert(m->fb_flushes >= 4 && value == 1);
        if (!m->l2_timeout) {
            if (m->bar1_dirty) arena_userd[0] = m->bar1_value;
            m->bar1_dirty = false; ++m->l2_flushes;
        }
    } else if (address == 0x57070004) {
        assert(value == 1 && m->l2_flushes > m->l2_invalidations && !m->bar1_dirty);
        ++m->l2_invalidations;
    } else if (address == 0x58013000) {
        assert(m->bar1 && m->fb_flushes >= 2 && !m->runlist);
        assert(value == 0x337755aa || value == 0);
        /* Model posted BAR1 data in GPU cache: a BAR1 read sees it, whereas
         * CPU RAM remains unchanged until L2 dirty writeback completes.
         */
        if (!m->bad_bar1_write) { m->bar1_value=value; m->bar1_dirty=true; }
    } else if (address == 0x5801308c) {
        assert((m->pb & 1) && m->runlist && !(m->sched & 1));
        assert(!(m->stall & 0x100) && arena_userd[0] == 0);
        assert(value == m->put+1); /* neither release can skip its doorbell */
        m->put = value; arena_userd[35] = value;
        if(m->simulated_gpu) {
            assert(m->notify_armed && value>=1 && value<=2);
            unsigned i=value-1;
            assert(arena_ring[i].low==0x11000+i*28);
            assert(((arena_ring[i].high>>10)&0x1fffff)==7);
            assert(arena_push[i*7+5]==0x2001c008 && arena_push[i*7+6]==0);
            assert(arena_push[i*7+2]==0x12000+i*4);
            /* Explicit GPU substitute for this test only. Production code
             * only publishes the doorbell and never assigns a passing fence.
             */
            arena_fence[i]=arena_push[i*7+3];
            ++m->notify_generated;
        }
    } else if (address == 0x57800000 || address == 0x57800004) {
        assert((m->mc & 0x100) && m->ticks - m->enabled_at >= 20);
        ++m->channel_writes;
        if (address == 0x57800000) {
            assert(value == 0 || value == 0x800c0040); /* same CCSR target */
            m->instance = value;
        }
    } else if (address == 0x70019aa8 || address == 0x70019aac) {
        /* Reproduce the failed assumption: an ASID read need not become zero.
         * Direct physical GPU DMA must leave this global state untouched.
         */
        ++m->asid_writes;
    }
}
static void delay(void *context, uint32_t us)
{ ((struct fifo_model *)context)->ticks += us; }
static bool notify_arm(void *context)
{
    struct fifo_model *m=context;
    assert(!m->runlist && !m->put && m->bar1 && (m->pb&1));
    m->notify_armed=!m->fail_notify_arm;
    return m->notify_armed;
}
static bool notify_wait(void *context,uint32_t count)
{
    struct fifo_model *m=context;
    assert(count==m->put && m->notify_generated==count);
    ++m->notify_waited;
    return m->fail_notify_wait!=count;
}

void test_execute_init(void)
{
    static _Alignas(4096) uint8_t arena[TG_GPU_ARENA_BYTES];
    struct fifo_model m = {.instance=0x80000400, .asid={0x80000011,0x80000012}, .pb=2, .runlist_map=1};
    struct tg_gpu_power_io io = {&m,read32,write32,NULL,NULL,delay, NULL};
    struct tg_gpu_power power = {0};
    struct tg_gpu_execution state = {0};
    struct tg_gpu_status result = {.expected={0x13579bdf,0x2468ace0}};
    arena_userd = (uint32_t *)(arena + 0x41000);
    arena_runlist = (uint32_t *)(arena + 0x45000);
    arena_push=(uint32_t *)(arena+0x43000);arena_fence=(uint32_t *)(arena+0x44000);
    arena_ring=(void *)(arena+0x42000);
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_ARGUMENT);
    assert(m.writes == 0 && m.channel_reads == 0);
    {
        struct tg_gpu_gr_work missing={.bytes=TG_GPU_GR_CHANNEL_WORK_BYTES};
        assert(tg_gpu_boot_graphics(&io,&power,arena,0xc0000000,NULL,&missing,&state,&result)==TG_GPU_EXEC_ARGUMENT);
        assert(!state.boot_ready && !state.channel_bound && !m.writes);
    }
    power.owned = power.ready = true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_MMU_TIMEOUT);
    assert(state.stage == 2 && state.owned && state.fifo_ready && !state.boot_ready && !state.channel_bound);
    assert(result.mc_enable_before == 0 && result.channel_instance_before == UINT32_MAX);
    assert(result.mc_enable_after == 0x100 && result.channel_instance_after == 0);
    assert(result.pbdma_enable_before == 2 && result.pbdma_enable_after == 3);
    assert(result.pbdma_runlist_map == 1 && state.pbdma_owned);
    assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
    tg_gpu_execute_stop(&io,&state);
    assert(!(m.mc & 0x100) && !state.fifo_ready && !state.boot_ready && !state.channel_bound);
    assert(m.pb == 2 && !state.pbdma_owned);
    assert(m.asid_writes == 0 && m.asid[0] == 0x80000011 && m.asid[1] == 0x80000012);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    /* Never admit an address carrying GM20B's SMMU translation tag. */
    assert(tg_gpu_execute(&io,&power,arena,UINT64_C(1)<<34,NULL,&state,&result) == TG_GPU_EXEC_ARGUMENT);
    assert(m.writes == 0 && m.channel_reads == 0 && !state.owned);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); m.fail_enable=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_FIFO_INIT);
    assert(state.stage == 1 && !state.fifo_ready && m.channel_reads == 0);
    tg_gpu_execute_stop(&io,&state);
    assert(m.channel_writes == 0);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); m.unreadable_channel=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_FIFO_INIT);
    assert(result.channel_instance_after == UINT32_MAX && m.channel_writes == 0);
    tg_gpu_execute_stop(&io,&state);
    assert(m.channel_writes == 0);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.fail_pb_enable=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_PBDMA_INIT);
    assert(state.stage == 1 && result.pbdma_enable_after == 0 && !m.put);
    tg_gpu_execute_stop(&io,&state);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); m.allow_mmu=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_PBDMA_INIT);
    assert(!state.pbdma_owned && !m.put); /* no PBDMA serves runlist zero */
    tg_gpu_execute_stop(&io,&state);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.allow_mmu=true; m.sched=0xa5; m.stall=UINT32_MAX;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_TIMEOUT);
    assert(state.stage == 4 && result.userd_gp_put == 1 && result.userd_gp_get == 0);
    assert(m.put == 1 && result.pbdma_gp_base == UINT32_MAX);
    assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
    assert(result.scheduler_before == 0xa5 && result.scheduler_after == 0xa4);
    assert(result.runlist_status == 2 && result.runlist_base == 0x000c0045);
    assert(m.stall == 0xfffffeff && result.pbdma_interrupt_1 == 0);
    assert(m.intr0 == 0xfffffeff && m.intr1 == 0xe);
    assert(result.bar1_phase == 5 && !result.bar1_read && !result.bar1_cpu);
    assert(result.frontend.l2_flushes == 2 && result.frontend.l2_invalidations == 2);
    assert(result.engine_status[0] == 0x80008000 && result.engine_status[1] == 0x2000);
    assert(result.pbdma_context.intr_enable==m.intr0 && result.pbdma_context.fifo_pending==1);
    assert(!result.pbdma_context.method0 && !result.pbdma_context.target);
    m.valid_pbdma=true;
    tg_gpu_execute_snapshot(&io,&state,&result);
    assert(result.pbdma_context.method0==(0x570400c0^0x5a5a0000));
    assert(result.pbdma_context.data0==(0x570400c4^0x5a5a0000));
    assert(result.pbdma_context.target==(0x570400ac^0x5a5a0000));
    assert(result.pbdma_context.signature==(0x57040010^0x5a5a0000));
    m.valid_pbdma=false;
    /* Capture GR state after the failing channel operation, not the earlier
     * FECS-ready reply. Reading it requires owned, booted GR. */
    m.live_gr_snapshot=true;result.gr.stage=15;state.gr_owned=false;
    tg_gpu_execute_snapshot(&io,&state,&result);
    assert(m.gr_reads==0);
    state.gr_owned=true;
    tg_gpu_execute_snapshot(&io,&state,&result);
    assert(m.gr_reads==7 && result.gr.current_context==(0x57409b00^0x53570000));
    assert(result.gr.fecs_mailbox==(0x57409800^0x53570000));
    assert(result.gr.gr_interrupt==(0x57400100^0x53570000));
    assert(result.gr.fecs_host_interrupt==(0x57409c18^0x53570000));
    assert(result.gr.fecs_error_mailbox==(0x57409818^0x53570000));
    assert(result.gr.fecs_ctxsw_status==(0x57409400^0x53570000));
    assert(result.gr.new_context==(0x57409b04^0x53570000));
    state.ce_reset_mask=0x200000;m.mc|=state.ce_reset_mask;
    tg_gpu_execute_stop(&io,&state);
    assert(!(m.mc&0x200000) && !state.ce_reset_mask);
    assert(!m.pb && !m.runlist && !m.instance && !state.pbdma_owned);
    assert(m.sched == 0xa5);

    memset(&m,0,sizeof(m));memset(&state,0,sizeof(state));memset(&result.frontend,0,sizeof(result.frontend));
    m.runlist_map=1;m.allow_mmu=true;m.fail_gr_enable=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result)==TG_GPU_EXEC_FRONTEND);
    assert(state.stage==7 && result.frontend.stage==10 && !m.runlist && !m.put && !m.instance);
    tg_gpu_execute_stop(&io,&state);
    assert(!m.pb && !m.runlist && !m.instance);

    /* A secure GPU without a usable WPR must stop before firmware DMA,
     * channel binding or semaphore submission. Cleanup also resets PMU.
     */
    {
        uint8_t surface[0x1600]={0};
        struct tg_gpu_gr_payload fw={0};
        struct tg_gpu_acr_files acr={.gr=&fw};
        fw.acr=&acr;fw.surface=surface;fw.image.size=sizeof(surface);
        for(unsigned j=0;j<2;++j) {
            struct tg_gpu_gr_falcon_image *f=&fw.image.falcon[j];
            f->boot=(struct tg_gpu_gr_segment){j*0x1000,4};
            f->code=(struct tg_gpu_gr_segment){j*0x1000+0x100,4};
            f->data=(struct tg_gpu_gr_segment){j*0x1000+0x200,4};
            f->boot_signature=j?0x3d3d65e2:0x9121ab5c;
        }
        memset(&m,0,sizeof(m));memset(&state,0,sizeof(state));memset(&result,0,sizeof(result));
        result.expected[0]=0x13579bdf;result.expected[1]=0x2468ace0;
        m.runlist_map=1;m.allow_mmu=true;m.secure=true;
        assert(tg_gpu_execute(&io,&power,arena,0xc0000000,&fw,&state,&result)==TG_GPU_EXEC_ACR_BOOT);
        assert(state.stage==9 && state.acr_owned && result.acr.result==TG_GPU_ACR_HW_WPR);
        assert(!m.runlist && !m.put && !m.instance && result.observed[0]==0xdeadbeef && result.observed[1]==0xdeadbeef);
        tg_gpu_execute_stop(&io,&state);
        assert(!(m.mc&0x2000) && !state.acr_owned);
    }

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); memset(&result.frontend,0,sizeof(result.frontend));
    m.runlist_map=1; m.allow_mmu=true; m.l2_timeout=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_FRONTEND);
    assert(result.frontend.stage == 7 && result.frontend.result == TG_GPU_FE_L2_TIMEOUT);
    assert(result.bar1_phase == 3 && result.bar1_read == 0x337755aa && result.bar1_cpu == 0x55aa3377);
    assert(!m.runlist && !m.put && !m.l2_invalidations);
    tg_gpu_execute_stop(&io,&state);
    assert(!m.pb && !m.runlist && !m.instance && !state.pbdma_owned);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.allow_mmu=true; m.fail_sched_allow=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_SCHEDULER);
    assert(state.stage == 3 && !m.put && (result.scheduler_after & 1));
    tg_gpu_execute_stop(&io,&state);
    assert(!m.pb && !m.runlist && !m.instance);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    m.runlist_map=1; m.allow_mmu=true; m.hce_fault=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_FAULT);
    assert(state.stage == 4 && result.pbdma_interrupt_1 == 8);
    assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
    tg_gpu_execute_stop(&io,&state);
    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    memset(&result.frontend,0,sizeof(result.frontend));
    m.runlist_map=1; m.allow_mmu=true; m.bad_ltc=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_FRONTEND);
    assert(result.frontend.result == TG_GPU_FE_LTC && !m.runlist && !m.fb_flushes);
    tg_gpu_execute_stop(&io,&state);
    assert(!m.pb && !m.instance);

    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state));
    memset(&result.frontend,0,sizeof(result.frontend));
    m.runlist_map=1; m.allow_mmu=true; m.flush_timeout=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_FRONTEND);
    assert(result.frontend.result == TG_GPU_FE_FLUSH_TIMEOUT && !m.runlist && !m.put);
    tg_gpu_execute_stop(&io,&state);
    assert(!m.pb && !m.instance && !m.bar1);

    /* BAR1 read-only visibility is insufficient for the GP_PUT doorbell. */
    memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); memset(&result.frontend,0,sizeof(result.frontend));
    m.runlist_map=1; m.allow_mmu=true; m.bad_bar1_write=true;
    assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) == TG_GPU_EXEC_BAR1);
    assert(state.stage == 2 && !m.runlist && !m.put);
    tg_gpu_execute_stop(&io,&state);

    /* An actual scheduler fault must not be disguised as another E6 timeout.
     * Runlist event bit 30 alone remains nonfatal and cannot create a PASS.
     */
    const uint32_t faults[] = {1,0x100,0x10000,0x20000000,0x40000000};
    for (unsigned j=0;j<sizeof(faults)/sizeof(faults[0]);++j) {
        memset(&m,0,sizeof(m)); memset(&state,0,sizeof(state)); memset(&result.frontend,0,sizeof(result.frontend));
        m.runlist_map=1; m.allow_mmu=true; m.fifo_fault=faults[j];
        assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result) ==
               (j<4 ? TG_GPU_EXEC_FAULT : TG_GPU_EXEC_TIMEOUT));
        assert(result.fifo_interrupt == faults[j] && m.put == (j<4 ? 0u : 1u));
        assert(result.bind_error == (j==0 ? 5u : 0u));
        assert(result.scheduler_error == (j==1 ? 0xau : 0u));
        assert(result.chsw_error == (j==2 ? 0x1234u : 0u));
        assert(result.observed[0] == 0xdeadbeef && result.observed[1] == 0xdeadbeef);
        tg_gpu_execute_stop(&io,&state);
    }
    /* Reproduce the live ENGINE notification, including immediate reassertion.
     * Both allow GP_PUT but must time out without GPU-written fences. True
     * concurrent faults and faults arriving during W1C still stop submission.
     */
    for (unsigned j=0;j<7;++j) {
        memset(&m,0,sizeof(m));memset(&state,0,sizeof(state));memset(&result.frontend,0,sizeof(result.frontend));
        m.runlist_map=1;m.allow_mmu=true;m.engine_event=true;
        m.stuck_event=j==1;m.pb_fault=j==2?0x8000:0;m.hce_fault=j==3;
        m.fifo_fault=j==4?1:0;m.mmu_fault=j==5?1:0;m.late_pb_fault=j==6;
        assert(tg_gpu_execute(&io,&power,arena,0xc0000000,NULL,&state,&result)==
               (j<2?TG_GPU_EXEC_TIMEOUT:TG_GPU_EXEC_FAULT));
        assert(m.put==(j<2?1u:0u));
        if (j==1) {
            assert(m.event_acks>1 && m.ticks>=100000 && m.ticks<200000);
            assert(result.pbdma_interrupt==0x01000000 && result.fifo_interrupt==0x20000000);
        } else assert(m.event_acks==((j==0 || j==6)?1u:0u));
        assert(m.summary_acks==m.event_acks);
        assert(result.observed[0]==0xdeadbeef && result.observed[1]==0xdeadbeef);
        tg_gpu_execute_stop(&io,&state);
    }
    const struct tg_gpu_execution_notify notify={&m,notify_arm,notify_wait};
    for(unsigned fail=0;fail<=3;++fail) {
        memset(&m,0,sizeof(m));memset(&state,0,sizeof(state));
        memset(&result.frontend,0,sizeof(result.frontend));
        m.runlist_map=1;m.allow_mmu=true;m.simulated_gpu=true;
        m.fail_notify_arm=fail==3;m.fail_notify_wait=fail<3?fail:0;
        assert(tg_gpu_execute_irq(&io,&power,arena,0xc0000000,NULL,&notify,&state,&result)==
               (fail?TG_GPU_EXEC_IRQ:TG_GPU_EXEC_OK));
        assert(m.put==(fail==3?0u:fail==1?1u:2u));
        assert(result.irq_expected==2 && m.notify_waited==(fail==3?0u:fail==1?1u:2u));
        assert(state.boot_ready==(fail!=3) && state.channel_bound==(fail!=3));
        if(!fail) {
            assert(result.observed[0]==result.expected[0]&&result.observed[1]==result.expected[1]);
            assert(state.stage==6);
        }
        tg_gpu_execute_stop(&io,&state);
        assert(!state.boot_ready && !state.channel_bound);
    }
    {
        memset(&m,0,sizeof(m));memset(&state,0,sizeof(state));
        const uint32_t values[]={0x13579bdf,0x2468ace0};
        assert(tg_gpu_prepare_3d_arena(arena,0xc0000000,TG_GPU_ARENA_BYTES,values));
        assert(arena_push[0]==0x20010000&&arena_push[1]==0xb197);
        assert(arena_push[8]==values[0]&&arena_push[16]==values[1]);
        assert(arena_ring[0].low==0x11000&&arena_ring[0].high==(10u<<10));
        assert(arena_ring[1].low==0x11028&&arena_ring[1].high==((8u<<10)|0x80000000u));
        assert(arena_fence[0]==0xdeadbeef&&arena_fence[1]==0xdeadbeef);
        struct tg_gpu_gr_work work={0};work.bytes=TG_GPU_GR_CHANNEL_WORK_BYTES;
        assert(tg_gpu_execute_3d(&io,&power,arena,0xc0000000,NULL,&work,&state,&result)==TG_GPU_EXEC_ARGUMENT);
        assert(!m.writes&&!state.owned); /* Never replace missing GR firmware with FIFO success. */
    }
}
