/* SPDX-License-Identifier: MIT */
#include "../core/channel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Ordering/lifetime model only. No GPU or fence completion is simulated. */
struct model {
    uint32_t sched,instance,control,runlist,runbase,pbdma,preempt,root;
    uint32_t bar1,userd_base,put,fifo,pbdma_intr,gr_current;
    uint32_t bus_intr,pri_save0,pri_save1,pri_fecs;
    unsigned reads,writes,doorbells,tlbs,flushes,pri_timeout_reads,reads_after_abort,writes_after_abort;
    bool bad_post,fail_tlb,fail_preempt,hold_gr,hold_ce,fault_on_put,change_slot;
    bool abort_flag,latch_abort_on_bar1,latch_abort_on_runlist,latch_abort_in_commit;
    bool latch_abort_after_doorbell,latch_abort_after_sched_enable;
    bool latch_abort_on_pfifo_poll,latch_abort_on_pbus_poll,latch_abort_before_engine_w1c;
    uint32_t engine0_val,engine1_val;
    unsigned slot_reads,context_reads;
};
static bool abort_check(void *context)
{
    const struct model *m=context;
    return m->abort_flag;
}
static bool commit_acquire(void *context)
{
    struct model *m=context;
    if(m->latch_abort_in_commit)m->abort_flag=true;
    return !m->abort_flag;
}
static void commit_write32(void *context,const struct tg_gpu_power_io *io,
    uint32_t reg,uint32_t val,bool final_step)
{
    (void)context;(void)final_step;
    io->write32(io->context,reg,val);
}
static uint32_t read32(void *context,uint32_t address)
{
    struct model *m=context;++m->reads;
    if(m->abort_flag)++m->reads_after_abort;
    switch(address) {
    case 0x57000200:return 0x1100;
    case 0x57000204:case 0x57002390:return 1;
    case 0x57002630:return m->sched;
    case 0x57800000:return m->instance;
    case 0x57800004:return m->control;
    case 0x57002270:return m->runbase;
    case 0x57002274:return m->runlist;
    case 0x57002284:case 0x57070000:return 0;
    case 0x57002634:return m->preempt;
    case 0x57003080:return m->change_slot && m->slot_reads++ ? 0x3001 : m->pbdma;
    case 0x57409b00:return m->gr_current;
    case 0x57100c80:return m->fail_tlb?0x10000:0x18000;
    case 0x57001704:
        if(m->latch_abort_on_bar1)m->abort_flag=true;
        return m->bar1;
    case 0x57002254:return m->userd_base;
    case 0x5801308c:return m->bad_post?UINT32_MAX:m->put;
    case 0x58013088:return 1;
    case 0x58013048:return 0;
    case 0x57002648:return m->engine1_val?m->engine1_val:(m->hold_ce?0x80002000u:0u);
    case 0x57002640:return m->engine0_val;
    case 0x57400100:return 0;
    case 0x57040014:case 0x57040000:case 0x57040050:
    case 0x57040018:case 0x5704001c:case 0x57040054:case 0x57040058:
    case 0x57040084:case 0x570400c0:case 0x570400c4:
        ++m->context_reads;return address;
    case 0x57002100:
        if(m->latch_abort_on_pfifo_poll)m->abort_flag=true;
        return m->fifo;
    case 0x57001100:
        if(m->latch_abort_on_pbus_poll)m->abort_flag=true;
        return m->bus_intr;
    case 0x57009084:++m->pri_timeout_reads;return m->pri_save0;
    case 0x57009088:++m->pri_timeout_reads;return m->pri_save1;
    case 0x5700908c:++m->pri_timeout_reads;return m->pri_fecs;
    case 0x5700259c:return 0;
    case 0x57040108:return m->pbdma_intr;
    case 0x57040148:
        if(m->latch_abort_before_engine_w1c)m->abort_flag=true;
        return 0;
    case 0x57120058:case 0x5712005c:return 0;
    default:fprintf(stderr,"unexpected register read: %08x\n",address);assert(0);return UINT32_MAX;
    }
}
static void write32(void *context,uint32_t address,uint32_t value)
{
    struct model *m=context;++m->writes;
    if(m->abort_flag)++m->writes_after_abort;
    switch(address) {
    case 0x57040108:assert(value==0x01000000u);m->pbdma_intr&=~value;break;
    case 0x57002100:assert(value==0x20000000u);m->fifo&=~value;break;
    case 0x57002630:
        if(!(value&1)){
            assert(m->instance && m->runlist==2 && m->control);
            if(m->latch_abort_after_sched_enable)m->abort_flag=true;
        }
        m->sched=value;break;
    case 0x57100cb8:m->root=value;break;
    case 0x57100cbc:assert(value==0x80000001);++m->tlbs;break;
    case 0x57800000:
        assert(m->sched&1);
        if(value)assert(m->tlbs && m->root==0xc00000);
        else assert(!m->runlist && !m->pbdma && !m->preempt);
        m->instance=value;if(!value)m->put=0;break;
    case 0x57800004:
        if(value&0x400){assert(m->instance);m->control=1;}
        if(value&0x800)m->control=0;
        break;
    case 0x57002270:assert(m->sched&1);m->runbase=value;break;
    case 0x57002274:
        assert(m->sched&1);
        if(value){assert(value==2 && m->control && m->runbase==0xc0045);m->pbdma=0x3000;if(m->latch_abort_on_runlist)m->abort_flag=true;}
        else assert(!m->control && !m->preempt);
        m->runlist=value;break;
    case 0x5801308c:
        assert(m->instance && m->control && m->runlist==2 && !m->sched);
        assert(value>m->put && value<512);
        m->put=value;++m->doorbells;m->gr_current=0x800c0040;
        if(m->fault_on_put)m->fifo=0x100;
        if(m->latch_abort_after_doorbell)m->abort_flag=true;
        break;
    case 0x57002634:
        assert(value==0x01000000 && !m->control && (m->sched&1));
        m->preempt=m->fail_preempt?0x100000:0;
        if(!m->preempt){m->pbdma=0;if(!m->hold_gr)m->gr_current&=~0x80000000u;}
        break;
    case 0x57070000:assert(value==1 && !m->instance);++m->flushes;break;
    default:assert(!"unexpected register write");
    }
}
static void delay(void *context,uint32_t us)
{ (void)context;assert(us==2 || us==5); }
static void init(struct model *m,struct tg_gpu_channel *c)
{
    memset(m,0,sizeof(*m));memset(c,0,sizeof(*c));
    m->sched=1;m->bar1=0x800c0040;m->userd_base=0x10000013;
}
int main(void)
{
    struct model m;struct tg_gpu_channel c;
    struct tg_gpu_power power={.owned=true,.ready=true},other=power;
    struct tg_gpu_power_io io={.context=&m,.read32=read32,.write32=write32,.delay_us=delay};
    int owner=1,wrong=2;unsigned before;
    init(&m,&c);
    assert(tg_gpu_channel_publish(&io,&power,0x13000,1,&owner,&c)==TG_GPU_CHANNEL_NOT_READY);
    assert(!m.reads && !m.writes);
    assert(tg_gpu_channel_submit(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x1000000,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(!m.reads && !m.writes);
    m.fifo=0x100;
    assert(tg_gpu_channel_bind(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c)==TG_GPU_CHANNEL_FAULT);
    assert(!c.referenced && !m.writes);
    m.fifo=0;
    assert(tg_gpu_channel_bind(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c)==TG_GPU_CHANNEL_OK);
    assert(c.referenced && c.bound && !c.submitted && !m.doorbells && !m.put);
    before=m.reads+m.writes;
    assert(tg_gpu_channel_bind(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c)==TG_GPU_CHANNEL_BUSY);
    assert(tg_gpu_channel_publish(&io,&power,0x13000,1,&wrong,&c)==TG_GPU_CHANNEL_BUSY);
    assert(tg_gpu_channel_publish(&io,&other,0x13000,1,&owner,&c)==TG_GPU_CHANNEL_BUSY);
    assert(tg_gpu_channel_publish(&io,&power,0x13000,0,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(tg_gpu_channel_publish(&io,&power,0x13000,512,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(tg_gpu_channel_publish(&io,&power,0x13001,1,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(m.reads+m.writes==before);
    m.userd_base=0x13;
    assert(tg_gpu_channel_publish(&io,&power,0x13000,1,&owner,&c)==TG_GPU_CHANNEL_NOT_READY);
    m.userd_base=0x10000014;
    assert(tg_gpu_channel_publish(&io,&power,0x13000,1,&owner,&c)==TG_GPU_CHANNEL_NOT_READY);
    m.userd_base=0x10000013;m.bar1=0;
    assert(tg_gpu_channel_publish(&io,&power,0x13000,1,&owner,&c)==TG_GPU_CHANNEL_NOT_READY);
    assert(!m.doorbells && !c.submitted && c.referenced);
    m.bar1=0x800c0040;
    assert(tg_gpu_channel_publish(&io,&power,0x13000,1,&owner,&c)==TG_GPU_CHANNEL_OK);
    assert(c.submitted && c.last_put==1 && m.doorbells==1 && m.put==1);
    assert(tg_gpu_channel_publish(&io,&power,0x13000,2,&owner,&c)==TG_GPU_CHANNEL_OK);
    struct tg_gpu_channel_snapshot snapshot;
    unsigned writes=m.writes;
    tg_gpu_channel_snapshot(&io,&power,&owner,&c,&snapshot);
    assert(snapshot.flags==15 && snapshot.userd_get==1 && snapshot.userd_put==2 &&
           snapshot.pb_get_lo==0x57040018 && snapshot.pb_fetch_lo==0x57040054 &&
           snapshot.method0==0x570400c0 && m.context_reads==10 && m.writes==writes);
    m.change_slot=true;m.slot_reads=0;
    tg_gpu_channel_snapshot(&io,&power,&owner,&c,&snapshot);
    assert(!(snapshot.flags&2) && snapshot.pbdma_before==0x3000 && snapshot.pbdma_after==0x3001);
    m.change_slot=false;m.pbdma=0x3001;
    unsigned context_reads=m.context_reads;
    tg_gpu_channel_snapshot(&io,&power,&owner,&c,&snapshot);
    assert(!(snapshot.flags&2) && !snapshot.pb_get_lo && m.context_reads==context_reads);
    m.pbdma=0x3000;before=m.reads;
    tg_gpu_channel_snapshot(&io,&power,&wrong,&c,&snapshot);
    assert(!snapshot.flags && m.reads==before && m.writes==writes);
    before=m.reads+m.writes;
    assert(tg_gpu_channel_publish(&io,&power,0x13000,2,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(tg_gpu_channel_publish(&io,&power,0x14000,3,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(m.reads+m.writes==before);
    m.fail_preempt=true;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_TIMEOUT);
    assert(c.referenced && c.bound && c.owner==&owner && m.instance && m.runlist);
    m.fail_preempt=false;
    m.hold_gr=true;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_TIMEOUT);
    assert(c.referenced && m.instance && m.runlist && !m.pbdma && (m.gr_current&0x80000000));
    m.hold_gr=false;
    m.hold_ce=true;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_TIMEOUT);
    assert(c.referenced && m.instance && m.runlist && !m.pbdma);
    m.hold_ce=false;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    assert(!c.referenced && !c.bound && !c.submitted && !m.instance && !m.runlist && m.tlbs==2 && m.flushes==2);
    assert(m.gr_current==0xc0040); /* invalid context may retain its instance bits */
    init(&m,&c);m.bad_post=true;
    assert(tg_gpu_channel_submit(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x13000,&owner,&c)==TG_GPU_CHANNEL_IO);
    assert(c.referenced && c.submitted && c.last_put==2 && m.doorbells==1);
    m.bad_post=false;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    init(&m,&c);m.fail_tlb=true;
    assert(tg_gpu_channel_submit(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x13000,&owner,&c)==TG_GPU_CHANNEL_TLB);
    assert(c.referenced && !c.bound && !m.instance && !m.doorbells);
    m.fail_tlb=false;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    init(&m,&c);m.fault_on_put=true;
    assert(tg_gpu_channel_submit(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x13000,&owner,&c)==TG_GPU_CHANNEL_FAULT);
    assert(c.referenced && c.submitted && m.doorbells==1);
    m.fifo=0;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    init(&m,&c);
    assert(tg_gpu_channel_submit_entries(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x13000,1,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(tg_gpu_channel_submit_entries(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x13000,5,&owner,&c)==TG_GPU_CHANNEL_ARGUMENT);
    assert(tg_gpu_channel_submit_entries(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x13000,3,&owner,&c)==TG_GPU_CHANNEL_OK);
    assert(c.submitted && c.last_put==3 && m.put==3 && m.doorbells==1);
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    init(&m,&c);
    assert(tg_gpu_channel_submit_entries(&io,&power,0xc0000000,0xc0040000,0xc0045000,0x13000,4,&owner,&c)==TG_GPU_CHANNEL_OK);
    assert(c.submitted && c.last_put==4 && m.put==4 && m.doorbells==1 && !m.pri_timeout_reads && !c.pri_timeout.valid);
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    /* PBUS PRI fault triggers read-only 0x9084/0x9088/(0x908c) observation without clearing. */
    init(&m,&c);m.bus_intr=0x8;m.pri_save0=0x00123400;m.pri_save1=0x00abcdef;m.pri_fecs=0xbad00002;
    assert(tg_gpu_channel_bind(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c)==TG_GPU_CHANNEL_FAULT);
    assert(!m.writes && m.pri_timeout_reads==2 && c.pri_timeout.valid && !c.pri_timeout.fecs_valid);
    assert(c.pri_timeout.save0==0x00123400 && c.pri_timeout.save1==0x00abcdef && !c.pri_timeout.fecs_errcode);
    init(&m,&c);m.bus_intr=0x4;m.pri_save0=0x80123400;m.pri_save1=0x00abcdef;m.pri_fecs=0xbad00002;
    assert(tg_gpu_channel_bind(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c)==TG_GPU_CHANNEL_FAULT);
    assert(!m.writes && m.pri_timeout_reads==3 && c.pri_timeout.valid && c.pri_timeout.fecs_valid);
    assert(c.pri_timeout.save0==0x80123400 && c.pri_timeout.save1==0x00abcdef && c.pri_timeout.fecs_errcode==0xbad00002);
    /* B10: ENGINE_STATUS bit 30 (FAULTED) must immediately return FAULT (not timeout),
     * bit 13 (VALID) alone is allowed idle, and bit 31|15 (BUSY|CTXSW) times out. */
    init(&m,&c);m.engine0_val=0x40000000u;
    assert(tg_gpu_channel_bind(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c)==TG_GPU_CHANNEL_FAULT);
    assert(c.last_register==0x2640u && c.last_value==0x40000000u);
    init(&m,&c);m.engine0_val=0x2000u;m.engine1_val=0x2000u;
    assert(tg_gpu_channel_bind(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c)==TG_GPU_CHANNEL_OK);
    m.engine1_val=0x40000000u;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_FAULT);
    m.engine1_val=0x2000u;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    /* M1/B04/T1: Both PRI-read fault latch AND post-guard commit-point fault
     * latch must abort with 0 doorbells and submitted==false! */
    const struct tg_gpu_commit_guard guard={abort_check,commit_acquire,commit_write32,&m};
    init(&m,&c);m.latch_abort_on_bar1=true;
    assert(tg_gpu_channel_submit_entries_checked(&io,&power,0xc0000000,0xc0040000,0xc0045000,
                                                 0x13000,2,&owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(m.doorbells==0 && m.put==0 && !c.submitted && c.referenced && m.reads_after_abort==0);
    /* B04/B11: Retire checked aborts with zero writes or reads when fault is latched. */
    writes=m.writes;before=m.reads;
    assert(tg_gpu_channel_retire_checked(&io,&power,&owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(m.writes==writes && m.reads==before);
    m.abort_flag=false;m.latch_abort_on_bar1=false;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    /* T1a: bind succeeds, then fault latches AT doorbell acquire -> 0 doorbells, submitted==0! */
    init(&m,&c);
    assert(tg_gpu_channel_bind_checked(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c,&guard)==TG_GPU_CHANNEL_OK);
    m.latch_abort_in_commit=true;
    assert(tg_gpu_channel_publish_checked(&io,&power,0x13000,2,&owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(m.doorbells==0 && m.put==0 && !c.submitted && c.referenced && m.reads_after_abort==0);
    /* T1b (B04 v5 counterexample): doorbell wins (GP_PUT=2 written), then fault
     * latches immediately before BAR1 readback -> 1 doorbell, submitted==1,
     * referenced==1, and ZERO post-fault BAR1/PRI reads! */
    m.abort_flag=false;m.latch_abort_in_commit=false;m.latch_abort_after_doorbell=true;
    assert(tg_gpu_channel_publish_checked(&io,&power,0x13000,2,&owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(m.doorbells==1 && m.put==2 && c.submitted && c.referenced && m.reads_after_abort==0);
    m.abort_flag=false;m.latch_abort_after_doorbell=false;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    /* B04: Fault latched before or at SCHED_DISABLE release must NOT re-enable scheduler,
     * and fault latched right after SCHED_DISABLE=0 write issues 0 post-fault PRI reads. */
    init(&m,&c);m.latch_abort_on_runlist=true;
    assert(tg_gpu_channel_bind_checked(&io,&power,0xc0000000,0xc0040000,0xc0045000,
                                       &owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert((m.sched&1)==1 && !c.bound && m.doorbells==0 && m.reads_after_abort==0);
    m.abort_flag=false;m.latch_abort_on_runlist=false;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    init(&m,&c);m.latch_abort_after_sched_enable=true;
    assert(tg_gpu_channel_bind_checked(&io,&power,0xc0000000,0xc0040000,0xc0045000,
                                       &owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(!c.bound && c.referenced && m.doorbells==0 && m.reads_after_abort==0);
    m.abort_flag=false;m.latch_abort_after_sched_enable=false;
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    /* B04 v6 counterexamples: fault latching on PFIFO first poll read, PBUS
     * first snapshot read, or right before lone ENGINE_EVENT W1C ack must stop
     * with ZERO post-latch PRI reads and ZERO W1C writes! */
    init(&m,&c);
    assert(tg_gpu_channel_bind_checked(&io,&power,0xc0000000,0xc0040000,0xc0045000,&owner,&c,&guard)==TG_GPU_CHANNEL_OK);
    m.latch_abort_on_pfifo_poll=true;
    assert(tg_gpu_channel_publish_checked(&io,&power,0x13000,2,&owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(m.reads_after_abort==0 && m.writes_after_abort==0 && m.doorbells==0 && !c.submitted);
    m.abort_flag=false;m.latch_abort_on_pfifo_poll=false;
    m.latch_abort_on_pbus_poll=true;
    assert(tg_gpu_channel_publish_checked(&io,&power,0x13000,2,&owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(m.reads_after_abort==0 && m.writes_after_abort==0 && m.doorbells==0 && !c.submitted);
    m.abort_flag=false;m.latch_abort_on_pbus_poll=false;
    m.fifo=0x20000000u;m.pbdma_intr=0x01000000u;m.latch_abort_before_engine_w1c=true;
    assert(tg_gpu_channel_publish_checked(&io,&power,0x13000,2,&owner,&c,&guard)==TG_GPU_CHANNEL_FAULT);
    assert(m.reads_after_abort==0 && m.writes_after_abort==0 && m.pbdma_intr==0x01000000u && m.doorbells==0);
    m.abort_flag=false;m.latch_abort_before_engine_w1c=false;
    /* Normal lone ENGINE_EVENT W1C still succeeds when healthy: */
    assert(tg_gpu_channel_publish_checked(&io,&power,0x13000,2,&owner,&c,&guard)==TG_GPU_CHANNEL_OK);
    assert(m.pbdma_intr==0 && m.fifo==0 && m.doorbells==1 && c.submitted);
    assert(tg_gpu_channel_retire(&io,&power,&owner,&c)==TG_GPU_CHANNEL_OK);
    puts("GM20B hot channel binding, BAR1 notification and retained ownership: PASS (model only)");
    return 0;
}
