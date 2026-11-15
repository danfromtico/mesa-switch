/* SPDX-License-Identifier: MIT */
#include "../platform/t214_pmu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GPU 0x57000000u
#define PMU 0x10a000u
struct model {
    uint32_t regs[1024];
    uint8_t dmem[0x6000];
    unsigned writes, started, commands, delays, bad;
    bool no_init, no_reply, bad_pio, halt_irq, wrap, wrapped;
};
static void u16(uint8_t *p,uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint32_t read32(void *ctx,uint32_t addr)
{
    struct model *m=ctx;
    uint32_t r=addr-GPU-PMU;
    if (addr==GPU+0x140 || addr==GPU+0x144) return 0;
    assert(addr>=GPU+PMU && r<0x1000 && !(r&3));
    if (r==0x1c4) {
        uint32_t pos=m->regs[0x1c0/4]&0xffff,v=0;
        assert(m->regs[0x1c0/4]&0x02000000); /* Catch accidental posted reads. */
        assert(pos<=sizeof(m->dmem)-4);
        for (unsigned i=0;i<4;++i) v|=(uint32_t)m->dmem[pos+i]<<(8*i);
        m->regs[0x1c0/4]+=4;
        return v;
    }
    return m->regs[r/4];
}
static void init(struct model *m)
{
    uint8_t *b=m->dmem+0x1000;
    b[0]=7; b[1]=42;
    u16(b+8,0x100); u16(b+10,0x800); b[12]=2; /* HPQ index need not be zero. */
    u16(b+32,0x200); u16(b+34,0x1000);
    if (m->bad==1) b[1]=41;
    if (m->bad==2) u16(b+10,0x1000);
    if (m->bad==3) b[12]=4;
    if (m->bad==4) u16(b+34,0x5ff0);
    m->regs[0x4c8/4]=0x102c; m->regs[0x4cc/4]=0x1000;
    m->regs[0x4a8/4]=m->regs[0x4b8/4]=m->wrap?0x8f0:0x800;
    if (m->bad==8) m->regs[0x4b8/4]=0x810; /* No room to distinguish full/empty. */
}
static void write32(void *ctx,uint32_t addr,uint32_t value)
{
    struct model *m=ctx;
    uint32_t r=addr-GPU-PMU;
    assert(addr>=GPU+PMU && r<0x1000 && !(r&3));
    ++m->writes;
    if (r==4) { if (!m->halt_irq) m->regs[8/4]&=~value; return; }
    if (r==0x1c4) {
        uint32_t pos=m->regs[0x1c0/4]&0xffff;
        assert(m->regs[0x1c0/4]&0x01000000);
        assert(pos<=sizeof(m->dmem)-4);
        for (unsigned i=0;i<4;++i) m->dmem[pos+i]=(uint8_t)(value>>(8*i));
        if (!m->bad_pio) m->regs[0x1c0/4]+=4;
        return;
    }
    if (r==0x130) {
        const uint8_t *a=m->dmem+0x5fe0;
        assert(value==2 && !m->started && !(m->regs[8/4]&0x10));
        assert(a[20]==1 && !a[8] && !a[24]);
        assert(((uint32_t)a[4]|(uint32_t)a[5]<<8|(uint32_t)a[6]<<16|(uint32_t)a[7]<<24)==204000000);
        ++m->started; m->regs[0x100/4]=0x42;
        if (!m->no_init) init(m);
    }
    if (r==0x4a8) {
        uint8_t *b;
        static const uint8_t command[16]={10,16,3,1,0,0,0,0,1,0,0,0,0,0,0,0};
        assert(m->started && !m->commands && value==0x810);
        assert(!memcmp(m->dmem+0x800,command,sizeof(command)));
        assert(m->regs[0x4cc/4]==0x102c); /* Validated init was acknowledged. */
        if (m->wrap) assert(m->dmem[0x8f0]==0 && m->dmem[0x8f1]==4);
        ++m->commands;
        if (!m->no_reply) {
            if (m->wrap) {
                m->regs[0x4cc/4]=0x11fc;
                m->dmem[0x11fc]=0; m->dmem[0x11fd]=4;
                b=m->dmem+0x1000; m->regs[0x4c8/4]=0x100c;
            } else { b=m->dmem+0x102c; m->regs[0x4c8/4]=0x1038; }
            memset(b,0,12); b[0]=10; b[1]=12; b[3]=m->bad==5?2:1;
            if (m->bad==6) b[8]=1;
            if (m->bad==9) m->regs[0x4c8/4]=0xffffffff;
            if (m->bad==10) b[1]=255; /* Reject incomplete body, never read past head. */
        }
    }
    if (r==0x4cc && m->wrap && m->commands && value==0x1000) m->wrapped=true;
    m->regs[r/4]=value;
}
static void delay(void *ctx,uint32_t us)
{ struct model *m=ctx; assert(us==1000); ++m->delays; }
static void reset(struct model *m)
{ memset(m,0,sizeof(*m)); m->regs[0x100/4]=0x50; m->regs[8/4]=0x10; }
static enum tg_gpu_pmu_result run(struct model *m,struct tg_gpu_pmu_status *s)
{
    struct tg_gpu_power_io io={m,read32,write32,NULL,NULL,delay, NULL};
    struct tg_gpu_power power={.owned=true,.ready=true};
    struct tg_gpu_acr_hw_status acr={.stage=9,.security=1,.dmem_size=0x6000};
    return tg_gpu_pmu_start(&io,&power,&acr,s);
}
int main(void)
{
    struct model m; struct tg_gpu_pmu_status s;
    reset(&m); assert(run(&m,&s)==TG_GPU_PMU_OK);
    assert(s.stage==6 && s.messages==2 && s.reply_code==0 && m.commands==1);
    assert(s.hpq_index==2 && s.init_header==0x2a07 && s.last_header==0x01000c0a);
    reset(&m); m.wrap=true; assert(run(&m,&s)==TG_GPU_PMU_OK && m.wrapped && s.messages==3);
    for (unsigned bad=1;bad<=4;++bad) {
        reset(&m); m.bad=bad; assert(run(&m,&s)==TG_GPU_PMU_INIT_FORMAT);
        assert(!m.commands && m.regs[0x4cc/4]==0x1000);
    }
    reset(&m); m.bad=5; assert(run(&m,&s)==TG_GPU_PMU_REPLY_TIMEOUT && s.stage==5);
    reset(&m); m.bad=6; assert(run(&m,&s)==TG_GPU_PMU_WPR && s.reply_code==1);
    reset(&m); m.bad=8; assert(run(&m,&s)==TG_GPU_PMU_QUEUE && !m.commands);
    reset(&m); m.bad=9; assert(run(&m,&s)==TG_GPU_PMU_QUEUE);
    reset(&m); m.bad=10; assert(run(&m,&s)==TG_GPU_PMU_QUEUE && s.messages==1);
    reset(&m); m.no_reply=true; assert(run(&m,&s)==TG_GPU_PMU_REPLY_TIMEOUT && m.delays==2000);
    reset(&m); m.no_init=true; assert(run(&m,&s)==TG_GPU_PMU_INIT_TIMEOUT && !m.commands);
    reset(&m); m.bad_pio=true; assert(run(&m,&s)==TG_GPU_PMU_PIO && !m.started);
    reset(&m); m.halt_irq=true; assert(run(&m,&s)==TG_GPU_PMU_HALT_IRQ && !m.started);
    reset(&m); m.regs[0x100/4]=0x10; assert(run(&m,&s)==TG_GPU_PMU_SECURITY && !m.writes);
    puts("GM20B PMU RTOS args, init/WPR queues, wrap, reply identity and failures: PASS (model)");
    return 0;
}
