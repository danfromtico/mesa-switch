/* SPDX-License-Identifier: MIT
 * CCSR, runlist and preempt sequence follows Switchroot nvgpu 1ae0167d
 * fifo_gk20a.c and nouveau gk104.c/gf100.c. See ../NOTICE.
 */
#include "channel.h"
#include "fault.h"
#include "runlist.h"
#include "../windows/control.h"
#include <stddef.h>

static uint32_t get(const struct tg_gpu_power_io *io,uint32_t reg)
{ return io->read32(io->context,0x57000000u+reg); }
static void put(const struct tg_gpu_power_io *io,uint32_t reg,uint32_t value)
{
    io->write32(io->context,0x57000000u+reg,value);
    (void)get(io,reg);
}
static bool ready(const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{ return io && io->read32 && io->write32 && io->delay_us && power && power->owned && power->ready; }
static bool physical_page(uint64_t address)
{ return address>=UINT64_C(0x80000000) && address<=UINT64_C(0xfffff000) && !(address&0xfff); }
static bool wait_bits(const struct tg_gpu_power_io *io,struct tg_gpu_channel *c,
                      uint32_t reg,uint32_t mask,uint32_t wanted)
{
    unsigned retry;
    c->last_register=reg;
    for(retry=0;retry<20000;++retry) {
        c->last_value=get(io,reg);
        if(c->last_value==UINT32_MAX) return false;
        if((c->last_value&mask)==wanted) return true;
        io->delay_us(io->context,5);
    }
    return false;
}
static enum tg_gpu_channel_result fault(const struct tg_gpu_power_io *io,struct tg_gpu_channel *c)
{
    struct tg_gpu_fault_status status;
    bool pending=tg_gpu_fault_poll(io,&status);
    c->fifo_interrupt=status.fifo; c->mmu_fault=status.mmu;
    c->pbdma_interrupt=status.pbdma; c->pbdma_interrupt_1=status.pbdma1;
    tg_gpu_frontend_snapshot(io,&c->frontend);
    return pending || c->frontend.result ?
        TG_GPU_CHANNEL_FAULT : TG_GPU_CHANNEL_OK;
}
static bool userd_page(uint32_t offset)
{ return offset<0x01000000u && !(offset&0xfff); }
void tg_gpu_channel_snapshot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,const void *owner,const struct tg_gpu_channel *c,
    struct tg_gpu_channel_snapshot *s)
{
    *s=(struct tg_gpu_channel_snapshot){0};
    if(!io || !io->read32 || !power || !power->owned || !power->ready ||
       !owner || !c || !c->referenced || !c->bound || c->owner!=owner || c->power!=power)return;
    s->mc_enable=get(io,0x200);
    if(s->mc_enable==UINT32_MAX || !(s->mc_enable&0x100))return;
    s->flags|=1;
    s->engine0=get(io,0x2640);s->engine1=get(io,0x2648);
    s->pbdma_enable=get(io,0x204);
    if(s->pbdma_enable!=UINT32_MAX && (s->pbdma_enable&1)) {
        s->pbdma_before=get(io,0x3080);
        if(tg_gpu_pbdma_owns_slot(s->pbdma_before)) {
            /* NVIDIA GM20B hw_pbdma_gm20b.h, pinned Switchroot 1ae0167d.
             * PB GET and PB FETCH are distinct; retain raw high/low words. */
            s->gp_get=get(io,0x40014);s->gp_put=get(io,0x40000);s->gp_fetch=get(io,0x40050);
            s->pb_get_lo=get(io,0x40018);s->pb_get_hi=get(io,0x4001c);
            s->pb_fetch_lo=get(io,0x40054);s->pb_fetch_hi=get(io,0x40058);
            s->pb_header=get(io,0x40084);s->method0=get(io,0x400c0);s->data0=get(io,0x400c4);
        }
        s->pbdma_after=get(io,0x3080);
        if(s->pbdma_before==s->pbdma_after && tg_gpu_pbdma_owns_slot(s->pbdma_after))s->flags|=2;
    }
    if(c->submitted && userd_page(c->userd_offset)) {
        uint32_t bar1=get(io,0x1704),base=get(io,0x2254);
        if(bar1!=UINT32_MAX && (bar1&0x80000000) && base!=UINT32_MAX &&
           (base&0x10000000) && (base&0xfffff)==(c->userd_offset>>12)) {
            const uint32_t alias=0x58000000u+c->userd_offset;
            s->userd_get=io->read32(io->context,alias+34*4);
            s->userd_put=io->read32(io->context,alias+35*4);
            s->userd_reference=io->read32(io->context,alias+0x48);s->flags|=4;
        }
    }
    if(s->mc_enable&0x1000) {
        s->gr_interrupt=get(io,0x400100);s->gr_current=get(io,0x409b00);s->flags|=8;
    }
}
enum tg_gpu_channel_result tg_gpu_channel_bind(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    const void *owner,struct tg_gpu_channel *c)
{
    uint32_t value,inst;
    if(!c || !owner || !physical_page(root) || !physical_page(instance) || !physical_page(runlist))
        return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power)) return TG_GPU_CHANNEL_NOT_READY;
    if(c->referenced) return TG_GPU_CHANNEL_BUSY;
    c->stage=1;
    /* Do not read channel SRAM while PFIFO is reset. */
    value=get(io,0x200);
    if(value==UINT32_MAX || !(value&0x100)) return TG_GPU_CHANNEL_NOT_READY;
    value=get(io,0x204);
    if(value==UINT32_MAX || !(value&1)) return TG_GPU_CHANNEL_NOT_READY;
    value=get(io,0x2390);
    if(value==UINT32_MAX || !(value&1)) return TG_GPU_CHANNEL_NOT_READY;
    value=get(io,0x800000);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    if(value&0x80000000) return TG_GPU_CHANNEL_BUSY;
    if(fault(io,c)!=TG_GPU_CHANNEL_OK) return TG_GPU_CHANNEL_FAULT;
    c->owner=owner;c->power=power;c->root=root;c->referenced=true;
    c->bound=c->submitted=false;c->userd_offset=c->last_put=0;
    c->stage=2;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x2630,value|1);
    if(!wait_bits(io,c,0x2630,1,1)) return TG_GPU_CHANNEL_IO;
    if(tg_gpu_tlb_invalidate(io,power,root)!=TG_GPU_TLB_OK) return TG_GPU_CHANNEL_TLB;
    c->stage=3;
    /* GM20B maps SoC DRAM through VIDMEM (0), including CCSR/runlists. */
    inst=(uint32_t)(instance>>12)|0x80000000;
    put(io,0x800000,inst);
    if(!wait_bits(io,c,0x800000,0xbfffffff,inst)) return TG_GPU_CHANNEL_IO;
    value=get(io,0x800004);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x800004,(value&~0xc00u)|0x400);
    put(io,0x2270,(uint32_t)(runlist>>12));
    put(io,0x2274,TG_GPU_RUNLIST_ENTRIES);
    if(!wait_bits(io,c,0x2284,0x100000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x2630,value&~1u);
    if(!wait_bits(io,c,0x2630,1,0)) return TG_GPU_CHANNEL_IO;
    if(fault(io,c)!=TG_GPU_CHANNEL_OK) return TG_GPU_CHANNEL_FAULT;
    c->stage=4;c->bound=true;
    return TG_GPU_CHANNEL_OK; /* bound, still no published workload */
}
enum tg_gpu_channel_result tg_gpu_channel_publish(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint32_t offset,uint32_t gp_put,
    const void *owner,struct tg_gpu_channel *c)
{
    uint32_t base,bar1,address;
    if(!c || !owner || !userd_page(offset) || !gp_put || gp_put>=512)
        return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power))return TG_GPU_CHANNEL_NOT_READY;
    if(!c->referenced || !c->bound)return TG_GPU_CHANNEL_NOT_READY;
    if(c->owner!=owner || c->power!=power)return TG_GPU_CHANNEL_BUSY;
    if(gp_put<=c->last_put || (c->last_put && offset!=c->userd_offset))
        return TG_GPU_CHANNEL_ARGUMENT;
    bar1=get(io,0x1704);base=get(io,0x2254);
    if(bar1==UINT32_MAX || base==UINT32_MAX)return TG_GPU_CHANNEL_IO;
    if(!(bar1&0x80000000) || !(base&0x10000000) || (base&0xfffffu)!=(offset>>12))
        return TG_GPU_CHANNEL_NOT_READY;
    if(fault(io,c)!=TG_GPU_CHANNEL_OK)return TG_GPU_CHANNEL_FAULT;
    address=0x58000000u+offset+35*4;
    /* The alias write is the notification, not a CPU store to saved RAMFC.
     * Record the possible DMA reference before attempting the posted write.
     */
    c->submitted=true;c->userd_offset=offset;c->last_put=gp_put;
    io->write32(io->context,address,gp_put);
    c->last_register=address;c->last_value=io->read32(io->context,address);
    if(c->last_value!=gp_put)return TG_GPU_CHANNEL_IO;
    return fault(io,c);
}
enum tg_gpu_channel_result tg_gpu_channel_submit_entries(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    uint32_t userd_bar1_offset,uint32_t gp_put,const void *owner,struct tg_gpu_channel *c)
{
    enum tg_gpu_channel_result result;
    if(!userd_page(userd_bar1_offset) || gp_put<2 || gp_put>4)return TG_GPU_CHANNEL_ARGUMENT;
    result=tg_gpu_channel_bind(io,power,root,instance,runlist,owner,c);
    if(result!=TG_GPU_CHANNEL_OK)return result;
    return tg_gpu_channel_publish(io,power,userd_bar1_offset,gp_put,owner,c);
}
enum tg_gpu_channel_result tg_gpu_channel_submit(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    uint32_t userd_bar1_offset,const void *owner,struct tg_gpu_channel *c)
{
    return tg_gpu_channel_submit_entries(io,power,root,instance,runlist,userd_bar1_offset,2,owner,c);
}
enum tg_gpu_channel_result tg_gpu_channel_retire(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,const void *owner,struct tg_gpu_channel *c)
{
    uint32_t value;
    if(!c || !owner) return TG_GPU_CHANNEL_ARGUMENT;
    if(!c->referenced) return TG_GPU_CHANNEL_OK;
    if(c->owner!=owner) return TG_GPU_CHANNEL_BUSY;
    if(c->power!=power) return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power)) return TG_GPU_CHANNEL_NOT_READY;
    c->stage=5;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x2630,value|1);
    if(!wait_bits(io,c,0x2630,1,1)) return TG_GPU_CHANNEL_IO;
    value=get(io,0x800004);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    put(io,0x800004,(value&~0xc00u)|0x800);
    if(!wait_bits(io,c,0x800004,1,0)) return TG_GPU_CHANNEL_IO;
    put(io,0x2634,TG_GPU_PREEMPT_TSG0); /* preempt the group, not a bare CHID */
    if(!wait_bits(io,c,0x2634,0x100000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    value=get(io,0x200);
    if(value==UINT32_MAX)return TG_GPU_CHANNEL_IO;
    /* A GR context can still reference RAMIN/image backing after its FIFO
     * reader becomes idle. nvgpu gk20a_is_channel_ctx_resident explicitly
     * tests FECS CURRENT_CTX.VALID: the instance bits may remain after unload.
     * Never clear that bit ourselves to manufacture retirement evidence.
     */
    if((value&0x1000) && !wait_bits(io,c,0x409b00,0x80000000,0))
        return TG_GPU_CHANNEL_TIMEOUT;
    put(io,0x2274,0);
    if(!wait_bits(io,c,0x2284,0x100000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    /* With no other channels in this engine, PBDMA must become invalid/idle.
     * An inactive reader's other SRAM fields are not completion evidence.
     */
    if(!wait_bits(io,c,0x3080,0xe000,0)) return TG_GPU_CHANNEL_TIMEOUT;
    put(io,0x800000,0);
    if(!wait_bits(io,c,0x800000,0x80000000,0)) return TG_GPU_CHANNEL_IO;
    c->stage=6;
    if(tg_gpu_frontend_flush(io,power,&c->frontend)!=TG_GPU_FE_OK) return TG_GPU_CHANNEL_FLUSH;
    if(tg_gpu_tlb_invalidate(io,power,c->root)!=TG_GPU_TLB_OK) return TG_GPU_CHANNEL_TLB;
    c->owner=NULL;c->referenced=c->bound=c->submitted=false;c->stage=7;
    return TG_GPU_CHANNEL_OK;
}
void tg_gpu_channel_disable(const struct tg_gpu_power_io *io,const struct tg_gpu_power *power)
{
    uint32_t value;
    if(!ready(io,power)) return;
    value=get(io,0x2630);if(value!=UINT32_MAX) put(io,0x2630,value|1);
    put(io,0x204,0); /* exclusive GPU recovery; stop all pushbuffer readers */
    value=get(io,0x200);if(value!=UINT32_MAX) put(io,0x200,value&~0x100u);
    io->delay_us(io->context,20);
}
