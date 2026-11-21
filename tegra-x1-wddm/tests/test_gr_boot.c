/* SPDX-License-Identifier: MIT */
#include "../platform/t214_gr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GPU 0x57000000u
struct model {
    uint32_t *reg;
    unsigned writes, delays, commands, started[2], dma_reads[2], header_words[2];
    uint32_t headers[2][17], stop_reg;
    bool no_ready, pri_error, secure, stuck_current;
    unsigned watchdog, context_queries;
    uint32_t timeout_method, bad_method;
    unsigned gpc_writes;
    bool gpc_pri;
};
static uint32_t read32(void *ctx, uint32_t address)
{
    struct model *m = ctx;
    uint32_t r = address-GPU;
    assert(address >= GPU && r < 0x600000 && !(r & 3));
    assert(r != 0x4091c4 && r != 0x41a1c4); /* streaming DMEM is write-only here */
    if (r == m->stop_reg) return m->reg[r/4];
    if (r == 0x409a10) m->reg[r/4] = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (r == (i ? 0x41a118u : 0x409118u) && m->dma_reads[i] &&
            ++m->dma_reads[i] == 5) m->reg[r/4] |= 2;
    }
    return m->reg[r/4];
}
static void write32(void *ctx, uint32_t address, uint32_t value)
{
    struct model *m = ctx;
    uint32_t r = address-GPU;
    assert(address >= GPU && r < 0x600000 && !(r & 3));
    /* No fuse, memory-controller, channel, global IRQ, or PMIC writes. */
    assert(r >= 0x400000 && r < 0x600000);
    ++m->writes;
    if(r>=0x418880 && r<=0x4188b8) {
        const uint32_t order[]={0x418880,0x418890,0x418894,0x4188b0,0x4188b4,0x4188b8,0x4188ac};
        assert(m->context_queries==3 && m->gpc_writes<7 && r==order[m->gpc_writes]);
        ++m->gpc_writes;
        if(m->gpc_pri) m->reg[0x120058/4]=0x100;
    }
    if (m->secure) {
        assert(r!=0x409100 && r!=0x409104 && r!=0x40910c &&
               r!=0x409110 && r!=0x409114 && r!=0x409118 &&
               r!=0x40911c && r!=0x4091c0 && r!=0x4091c4);
        if (r==0x409130) {
            assert(value==2 && m->started[1]==1 && !m->header_words[0]);
            assert(m->reg[0x409804/4]==1 && m->reg[0x409858/4]==UINT32_MAX);
            ++m->started[0];
            if (!m->no_ready) m->reg[0x409800/4]=1;
        }
    }
    if (r == 0x400100) { m->reg[r/4] &= ~value; return; }
    if (r == 0x409840) { m->reg[0x409800/4] &= ~value; return; }
    if (r == 0x409b00 && value==0) {
        assert(m->started[0] && m->started[1] && m->reg[0x409800/4]==1);
        if(m->stuck_current) return;
    }
    if (r == 0x409504) {
        const uint32_t methods[]={0x10,0x16,0x25},sizes[]={0x17000,0x4000,0x9000};
        assert(m->started[0] && m->started[1]);
        assert(!(m->reg[0x409b00/4]&0x80000000) && !m->reg[0x409800/4]);
        if(value==0x21) {
            assert(!m->watchdog && !m->context_queries && m->reg[0x409500/4]==0x7fffffff);
            ++m->watchdog;
        } else {
            assert(m->watchdog && m->context_queries<3 && !m->reg[0x409500/4]);
            assert(value==methods[m->context_queries]);
            m->reg[0x409800/4]=value==m->timeout_method?0:
                value==m->bad_method?UINT32_MAX:sizes[m->context_queries];
            ++m->context_queries;
        }
    }
    if (r == 0x409a10) {
        assert(value == (m->commands ? 5u : 7u));
        assert(m->reg[0x409b04/4] == 0x80080040 && m->reg[0x409a0c/4] == 0x80040);
        if (m->commands) assert(m->reg[0x409b00/4] == 0xb0080040);
        ++m->commands;
    }
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t base = i ? 0x41a000 : 0x409000;
        if (r == base+0x1c4) {
            assert(m->commands == 2 && m->header_words[i] < 17);
            m->headers[i][m->header_words[i]++] = value;
        }
        if (r == base+0x118) {
            assert(value == 0x610 && m->header_words[i] == 17);
            m->dma_reads[i] = 1;
        }
        if (r == base+0x100) {
            assert(value == 2 && (m->reg[(base+0x118)/4] & 2));
            assert(m->reg[(base+0x104)/4] == (i ? 0x3400u : 0x5c00u));
            ++m->started[i];
            if (i && !m->no_ready) m->reg[0x409800/4] = 1;
            if (i && m->pri_error) m->reg[0x120058/4] = 0x100;
        }
    }
    m->reg[r/4] = value;
}
static void delay(void *ctx, uint32_t us)
{ struct model *m = ctx; assert(us == 10); ++m->delays; }
static void reset(struct model *m)
{
    uint32_t *reg = m->reg;
    memset(reg,0,0x600000); memset(m,0,sizeof(*m)); m->reg=reg;
    reg[0x409108/4] = reg[0x41a108/4] = 0x20080; /* 32 KiB IMEM, 64 KiB DMEM */
    reg[0x2640/4] = 0x2000; /* valid, idle context; exercise the second idle case */
    reg[0x409800/4] = 0xfeed; /* an old mailbox value cannot prove startup */
}
static enum tg_gpu_gr_result run(struct model *m, struct tg_gpu_gr_image *image,
    struct tg_gpu_gr_netlist *netlist, struct tg_gpu_gr_status *s)
{
    struct tg_gpu_power_io io = {m,read32,write32,NULL,NULL,delay, NULL};
    struct tg_gpu_power power = {.owned=true,.ready=true};
    struct tg_gpu_frontend_status frontend = {.stage=11};
    if (m->secure) {
        struct tg_gpu_acr_hw_status acr={.stage=9,.security=1};
        struct tg_gpu_pmu_status pmu={.stage=6};
        return tg_gpu_gr_boot_secure(&io,&power,&frontend,netlist,image,0x20000,
                                    0x80040000,&acr,&pmu,s);
    }
    return tg_gpu_gr_boot(&io,&power,&frontend,netlist,image,0x20000,0x80040000,s);
}
int main(void)
{
    uint8_t regs[] = {0x80,0,0x40,0, 0x34,0x12,0,0};
    struct tg_gpu_gr_netlist net = {0};
    struct tg_gpu_gr_image image = {.size=0x600};
    struct tg_gpu_gr_status s;
    struct model m = {.reg=calloc(1,0x600000)}; assert(m.reg);
    net.region[TG_GPU_GR_NONCONTEXT] = (struct tg_gpu_gr_blob){regs,sizeof(regs)};
    for (unsigned i=0;i<2;++i) {
        struct tg_gpu_gr_falcon_image *f = &image.falcon[i];
        f->boot=(struct tg_gpu_gr_segment){i*0x300,4};
        f->code=(struct tg_gpu_gr_segment){i*0x300+0x100,4};
        f->data=(struct tg_gpu_gr_segment){i*0x300+0x200,4};
        f->imem_offset=f->entry=i ? 0x3400 : 0x5c00;
        f->boot_signature=i ? 0x3d3d65e2 : 0x9121ab5c;
    }
    reset(&m); assert(run(&m,&image,&net,&s)==TG_GPU_GR_OK);
    assert(s.stage==13 && s.fecs_mailbox==0x9000 && m.started[0]==1 && m.started[1]==1);
    assert(!s.current_context && s.context_bytes==0x17000 && s.zcull_bytes==0x4000 && s.pm_bytes==0x9000);
    assert(m.watchdog==1 && m.context_queries==3);
    assert(m.delays>=4 && m.commands==2 && m.reg[0x400080/4]==0x1234);
    for (unsigned i=0;i<2;++i) {
        assert(m.headers[i][8]==4 && m.headers[i][9]==0x201+i*3);
        assert(m.headers[i][15]==0x202+i*3 && m.headers[i][16]==4);
    }
    reset(&m); m.reg[0x21434/4]=1;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_SECURITY_REQUIRED && !m.writes);
    reset(&m); m.reg[0x21434/4]=UINT32_MAX;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_REGISTER && !m.writes);
    reset(&m); regs[2]=0x70;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_FIRMWARE_FORMAT && !m.writes);
    regs[2]=0x40;
    reset(&m); m.reg[0x41a108/4]=1;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_REGISTER && !m.writes);
    reset(&m); image.falcon[1].entry++;
    image.falcon[1].imem_offset+=256;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_FIRMWARE_FORMAT && !m.writes);
    image.falcon[1].entry--; image.falcon[1].imem_offset-=256;
    reset(&m); m.reg[0x41a10c/4]=6;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_SCRUB_TIMEOUT && !m.commands);
    reset(&m); m.reg[0x40060c/4]=1; m.reg[0x2640/4]=0xa000;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_IDLE_TIMEOUT && !m.commands);
    reset(&m); m.stop_reg=0x409a10;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_ARB_TIMEOUT && !m.started[0]);
    reset(&m); m.stop_reg=0x409118;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_DMA_TIMEOUT && !m.started[0]);
    reset(&m); m.stop_reg=0x41a118;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_DMA_TIMEOUT && m.started[0] && !m.started[1]);
    reset(&m); m.no_ready=true;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_READY_TIMEOUT && m.started[0] && m.started[1]);
    reset(&m); m.pri_error=true;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_PRI_FAULT);
    reset(&m); m.secure=true; m.reg[0x21434/4]=1; m.reg[0x409100/4]=0x40;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_OK && s.stage==13);
    assert(m.started[0]==1 && m.started[1]==1 && !m.header_words[0] && m.header_words[1]==17);
    reset(&m); m.secure=true; m.reg[0x21434/4]=1;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_SECURITY_REQUIRED && !m.writes);
    reset(&m); m.secure=true; m.reg[0x21434/4]=1;
    m.reg[0x409100/4]=m.reg[0x41a100/4]=0x40;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_SECURITY_REQUIRED && !m.writes);
    reset(&m); m.secure=true; m.no_ready=true;
    m.reg[0x21434/4]=1; m.reg[0x409100/4]=0x40;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_READY_TIMEOUT);
    assert(m.started[0]==1 && m.started[1]==1 && !m.header_words[0]);
    assert(tg_gpu_gr_boot_secure(NULL,NULL,NULL,NULL,NULL,0,0,NULL,NULL,&s)
           ==TG_GPU_GR_SECURITY_REQUIRED);
    {
        struct tg_gpu_power_io io={&m,read32,write32,NULL,NULL,delay, NULL};
        struct tg_gpu_power power={.owned=true,.ready=true};
        struct tg_gpu_frontend_status fe={.stage=11,.ltc_count=2};
        reset(&m);assert(run(&m,&image,&net,&s)==TG_GPU_GR_OK);
        m.reg[0x100c80/4]=0x8123987f;m.reg[0x100cc4/4]=0x10555;
        m.reg[0x100cc8/4]=0xc00480;m.reg[0x100ccc/4]=0xc00470;
        assert(tg_gpu_gr_gpc_mmu(&io,&power,&fe,&s)==TG_GPU_GR_OK && s.stage==15);
        assert(m.gpc_writes==7 && m.reg[0x418880/4]==0x8000187f);
        assert(!m.reg[0x418890/4] && !m.reg[0x418894/4]);
        /* Bit 16 (DEBUG) is explicitly cleared while non-debug bits are preserved. */
        assert(m.reg[0x4188b0/4]==0x555 && m.reg[0x4188b4/4]==0xc00480);
        assert(m.reg[0x4188b8/4]==0xc00470 && m.reg[0x4188ac/4]==2);
        unsigned writes=m.writes;
        assert(tg_gpu_gr_gpc_mmu(&io,&power,&fe,&s)==TG_GPU_GR_ARGUMENT && m.writes==writes);
        const uint32_t src[]={0x100c80,0x100cc4,0x100cc8,0x100ccc};
        for(unsigned i=0;i<4;++i) {
            reset(&m);assert(run(&m,&image,&net,&s)==TG_GPU_GR_OK);m.writes=0;
            m.reg[src[i]/4]=UINT32_MAX;
            assert(tg_gpu_gr_gpc_mmu(&io,&power,&fe,&s)==TG_GPU_GR_REGISTER && !m.writes);
            assert(s.stage==14 && s.last_register==src[i]);
        }
        reset(&m);assert(run(&m,&image,&net,&s)==TG_GPU_GR_OK);m.gpc_pri=true;
        assert(tg_gpu_gr_gpc_mmu(&io,&power,&fe,&s)==TG_GPU_GR_PRI_FAULT && m.gpc_writes==7);
    }
    reset(&m);m.stuck_current=true;
    assert(run(&m,&image,&net,&s)==TG_GPU_GR_REGISTER && s.stage==8 && !m.watchdog);
    for(unsigned i=0;i<3;++i) {
        const uint32_t methods[]={0x10,0x16,0x25};
        reset(&m);m.timeout_method=methods[i];
        assert(run(&m,&image,&net,&s)==TG_GPU_GR_CONTEXT_TIMEOUT && s.stage==10+i);
        assert(m.context_queries==i+1 && !s.pm_bytes);
        reset(&m);m.bad_method=methods[i];
        assert(run(&m,&image,&net,&s)==TG_GPU_GR_REGISTER && s.stage==10+i);
    }
    free(m.reg);
    puts("GM20B GR bootstrap order, DMA idle, security, and bounded failure paths: PASS (model)");
    return 0;
}
