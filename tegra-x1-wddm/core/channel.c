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
static bool guard_aborted(const struct tg_gpu_commit_guard *guard)
{ return guard && guard->abort && guard->abort(guard->context); }
static void put_checked(const struct tg_gpu_power_io *io,uint32_t reg,
    uint32_t value,const struct tg_gpu_commit_guard *guard)
{
    if(guard_aborted(guard)) return;
    io->write32(io->context,0x57000000u+reg,value);
    if(!guard_aborted(guard))
        (void)get(io,reg);
}
static bool guard_acquire(const struct tg_gpu_commit_guard *guard)
{ return !guard || !guard->acquire || guard->acquire(guard->context); }
static void guard_write32(const struct tg_gpu_commit_guard *guard,
    const struct tg_gpu_power_io *io, uint32_t address, uint32_t value, bool final_step)
{
    if(guard && guard->write32) guard->write32(guard->context,io,address,value,final_step);
    else io->write32(io->context,address,value);
}
static enum tg_gpu_channel_result wait_bits_checked(const struct tg_gpu_power_io *io,
    struct tg_gpu_channel *c, uint32_t reg, uint32_t mask, uint32_t wanted,
    enum tg_gpu_channel_result fail_code, const struct tg_gpu_commit_guard *guard)
{
    unsigned retry;
    c->last_register=reg;
    for(retry=0;retry<20000;++retry) {
        if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
        c->last_value=get(io,reg);
        if(c->last_value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
        if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
        if((c->last_value&mask)==wanted) return TG_GPU_CHANNEL_OK;
        io->delay_us(io->context,5);
    }
    return fail_code;
}
static enum tg_gpu_channel_result wait_engine_idle(const struct tg_gpu_power_io *io,
                                                   struct tg_gpu_channel *c, uint32_t reg,
                                                   const struct tg_gpu_commit_guard *guard)
{
    unsigned retry;
    c->last_register=reg;
    for(retry=0;retry<20000;++retry) {
        if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
        c->last_value=get(io,reg);
        if(c->last_value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
        if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
        /* ENGINE_STATUS bit 30 is FAULTED; bits 31|15 are BUSY|CTXSW.
         * Bit 13 (CTX_STATUS_VALID) may remain set while an engine is idle. */
        if(c->last_value&0x40000000u) return TG_GPU_CHANNEL_FAULT;
        if(!(c->last_value&0x80008000u)) return TG_GPU_CHANNEL_OK;
        io->delay_us(io->context,5);
    }
    return TG_GPU_CHANNEL_TIMEOUT;
}
static enum tg_gpu_channel_result fault_checked(const struct tg_gpu_power_io *io,
    struct tg_gpu_channel *c, const struct tg_gpu_commit_guard *guard)
{
    struct tg_gpu_fault_status status;
    c->frontend=(struct tg_gpu_frontend_status){0};
    c->pri_timeout=(struct tg_gpu_pri_timeout_snapshot){0};
    if(guard_aborted(guard)){c->frontend.result=TG_GPU_FE_FAULT;return TG_GPU_CHANNEL_FAULT;}
    bool pending=tg_gpu_fault_poll_checked(io,&status,guard);
    c->fifo_interrupt=status.fifo; c->mmu_fault=status.mmu;
    c->pbdma_interrupt=status.pbdma; c->pbdma_interrupt_1=status.pbdma1;
    if(guard_aborted(guard)){c->frontend.result=TG_GPU_FE_FAULT;return TG_GPU_CHANNEL_FAULT;}
    tg_gpu_frontend_snapshot_checked(io,&c->frontend,guard);
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    tg_gpu_frontend_pri_timeout_snapshot_checked(io,c->frontend.bus_interrupt,&c->pri_timeout,guard);
    return pending || c->frontend.result || guard_aborted(guard) ?
        TG_GPU_CHANNEL_FAULT : TG_GPU_CHANNEL_OK;
}
static bool userd_page(uint32_t offset)
{ return offset<0x01000000u && !(offset&0xfff); }
void tg_gpu_channel_snapshot_checked(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,const void *owner,const struct tg_gpu_channel *c,
    struct tg_gpu_channel_snapshot *s,const struct tg_gpu_commit_guard *guard)
{
    *s=(struct tg_gpu_channel_snapshot){0};
    if(!io || !io->read32 || !power || !power->owned || !power->ready ||
       !owner || !c || !c->referenced || !c->bound || c->owner!=owner || c->power!=power)return;
    if(guard_aborted(guard))return;
    s->mc_enable=get(io,0x200);
    if(guard_aborted(guard) || s->mc_enable==UINT32_MAX || !(s->mc_enable&0x100))return;
    s->flags|=1;
    s->engine0=get(io,0x2640);
    if(guard_aborted(guard))return;
    s->engine1=get(io,0x2648);
    if(guard_aborted(guard))return;
    s->pbdma_enable=get(io,0x204);
    if(guard_aborted(guard))return;
    if(s->pbdma_enable!=UINT32_MAX && (s->pbdma_enable&1)) {
        s->pbdma_before=get(io,0x3080);
        if(guard_aborted(guard))return;
        if(tg_gpu_pbdma_owns_slot(s->pbdma_before)) {
            /* NVIDIA GM20B hw_pbdma_gm20b.h, pinned Switchroot 1ae0167d.
             * PB GET and PB FETCH are distinct; retain raw high/low words. */
            s->gp_get=get(io,0x40014); if(guard_aborted(guard))return;
            s->gp_put=get(io,0x40000); if(guard_aborted(guard))return;
            s->gp_fetch=get(io,0x40050); if(guard_aborted(guard))return;
            s->pb_get_lo=get(io,0x40018); if(guard_aborted(guard))return;
            s->pb_get_hi=get(io,0x4001c); if(guard_aborted(guard))return;
            s->pb_fetch_lo=get(io,0x40054); if(guard_aborted(guard))return;
            s->pb_fetch_hi=get(io,0x40058); if(guard_aborted(guard))return;
            s->pb_header=get(io,0x40084); if(guard_aborted(guard))return;
            s->method0=get(io,0x400c0); if(guard_aborted(guard))return;
            s->data0=get(io,0x400c4); if(guard_aborted(guard))return;
        }
        s->pbdma_after=get(io,0x3080);
        if(guard_aborted(guard))return;
        if(s->pbdma_before==s->pbdma_after && tg_gpu_pbdma_owns_slot(s->pbdma_after))s->flags|=2;
    }
    if(c->submitted && userd_page(c->userd_offset)) {
        uint32_t bar1=get(io,0x1704);
        if(guard_aborted(guard))return;
        uint32_t base=get(io,0x2254);
        if(guard_aborted(guard))return;
        if(bar1!=UINT32_MAX && (bar1&0x80000000) && base!=UINT32_MAX &&
           (base&0x10000000) && (base&0xfffff)==(c->userd_offset>>12)) {
            const uint32_t alias=0x58000000u+c->userd_offset;
            s->userd_get=io->read32(io->context,alias+34*4); if(guard_aborted(guard))return;
            s->userd_put=io->read32(io->context,alias+35*4); if(guard_aborted(guard))return;
            s->userd_reference=io->read32(io->context,alias+0x48); if(guard_aborted(guard))return;
            s->flags|=4;
        }
    }
    if(s->mc_enable&0x1000) {
        s->gr_interrupt=get(io,0x400100); if(guard_aborted(guard))return;
        s->gr_current=get(io,0x409b00); if(guard_aborted(guard))return;
        s->flags|=8;
    }
}
void tg_gpu_channel_snapshot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,const void *owner,const struct tg_gpu_channel *c,
    struct tg_gpu_channel_snapshot *s)
{
    tg_gpu_channel_snapshot_checked(io,power,owner,c,s,NULL);
}
enum tg_gpu_channel_result tg_gpu_channel_bind_checked(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    const void *owner,struct tg_gpu_channel *c,const struct tg_gpu_commit_guard *guard)
{
    uint32_t value,inst;
    enum tg_gpu_channel_result wr;
    if(!c || !owner || !physical_page(root) || !physical_page(instance) || !physical_page(runlist))
        return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power)) return TG_GPU_CHANNEL_NOT_READY;
    if(c->referenced) return TG_GPU_CHANNEL_BUSY;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    c->stage=1;
    /* Do not read channel SRAM while PFIFO is reset. */
    value=get(io,0x200);
    if(value==UINT32_MAX || !(value&0x100)) return TG_GPU_CHANNEL_NOT_READY;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x204);
    if(value==UINT32_MAX || !(value&1)) return TG_GPU_CHANNEL_NOT_READY;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x2390);
    if(value==UINT32_MAX || !(value&1)) return TG_GPU_CHANNEL_NOT_READY;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x800000);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    if(value&0x80000000) return TG_GPU_CHANNEL_BUSY;
    if(fault_checked(io,c,guard)!=TG_GPU_CHANNEL_OK) return TG_GPU_CHANNEL_FAULT;
    c->owner=owner;c->power=power;c->root=root;c->referenced=true;
    c->bound=c->submitted=false;c->userd_offset=c->last_put=0;
    c->stage=2;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x2630,value|1,guard);
    wr=wait_bits_checked(io,c,0x2630,1,1,TG_GPU_CHANNEL_IO,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    wr=wait_bits_checked(io,c,0x3080,0xe000,0,TG_GPU_CHANNEL_TIMEOUT,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    {
        enum tg_gpu_channel_result idle=wait_engine_idle(io,c,0x2640,guard);
        if(idle!=TG_GPU_CHANNEL_OK) return idle;
        idle=wait_engine_idle(io,c,0x2648,guard);
        if(idle!=TG_GPU_CHANNEL_OK) return idle;
    }
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    {
        enum tg_gpu_tlb_result tlb=tg_gpu_tlb_invalidate_checked(io,power,root,guard);
        if(tlb==TG_GPU_TLB_FAULT) return TG_GPU_CHANNEL_FAULT;
        if(tlb!=TG_GPU_TLB_OK) return TG_GPU_CHANNEL_TLB;
    }
    c->stage=3;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    /* GM20B maps SoC DRAM through VIDMEM (0), including CCSR/runlists. */
    inst=(uint32_t)(instance>>12)|0x80000000;
    put_checked(io,0x800000,inst,guard);
    wr=wait_bits_checked(io,c,0x800000,0xbfffffff,inst,TG_GPU_CHANNEL_IO,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x800004);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x800004,(value&~0xc00u)|0x400,guard);
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x2270,(uint32_t)(runlist>>12),guard);
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x2274,TG_GPU_RUNLIST_ENTRIES,guard);
    wr=wait_bits_checked(io,c,0x2284,0x100000,0,TG_GPU_CHANNEL_TIMEOUT,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    /* Arbitrate software permission at DIRQL with zero MMIO (V5-01), then write
     * SCHED_DISABLE=0 at caller IRQL and gate post-write reads on abort (B04). */
    if(!guard_acquire(guard) || guard_aborted(guard))
        return TG_GPU_CHANNEL_FAULT;
    guard_write32(guard,io,0x57002630u,value&~1u,true);
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    (void)get(io,0x2630);
    wr=wait_bits_checked(io,c,0x2630,1,0,TG_GPU_CHANNEL_IO,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    if(fault_checked(io,c,guard)!=TG_GPU_CHANNEL_OK) return TG_GPU_CHANNEL_FAULT;
    c->stage=4;c->bound=true;
    return TG_GPU_CHANNEL_OK; /* bound, still no published workload */
}
enum tg_gpu_channel_result tg_gpu_channel_bind(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    const void *owner,struct tg_gpu_channel *c)
{
    return tg_gpu_channel_bind_checked(io,power,root,instance,runlist,owner,c,NULL);
}
enum tg_gpu_channel_result tg_gpu_channel_publish_checked(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint32_t offset,uint32_t gp_put,
    const void *owner,struct tg_gpu_channel *c,const struct tg_gpu_commit_guard *guard)
{
    uint32_t base,bar1,address;
    if(!c || !owner || !userd_page(offset) || !gp_put || gp_put>=512)
        return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power))return TG_GPU_CHANNEL_NOT_READY;
    if(!c->referenced || !c->bound)return TG_GPU_CHANNEL_NOT_READY;
    if(c->owner!=owner || c->power!=power)return TG_GPU_CHANNEL_BUSY;
    if(gp_put<=c->last_put || (c->last_put && offset!=c->userd_offset))
        return TG_GPU_CHANNEL_ARGUMENT;
    if(guard_aborted(guard))return TG_GPU_CHANNEL_FAULT;
    bar1=get(io,0x1704);
    if(guard_aborted(guard))return TG_GPU_CHANNEL_FAULT;
    base=get(io,0x2254);
    if(bar1==UINT32_MAX || base==UINT32_MAX)return TG_GPU_CHANNEL_IO;
    if(!(bar1&0x80000000) || !(base&0x10000000) || (base&0xfffffu)!=(offset>>12))
        return TG_GPU_CHANNEL_NOT_READY;
    if(fault_checked(io,c,guard)!=TG_GPU_CHANNEL_OK)return TG_GPU_CHANNEL_FAULT;
    address=0x58000000u+offset+35*4;
    /* V5-01 + B04:
     * 1) Arbitrate software commit permission at DIRQL with zero MMIO; if a
     *    fault latched first, GP_PUT is never written and submitted stays false.
     * 2) Record in-flight ownership BEFORE writing GP_PUT at caller IRQL so
     *    backing stays pinned even if a fault races the doorbell.
     * 3) If a fault latches right after GP_PUT is written, exit BEFORE issuing
     *    any BAR1 readback and hand in-flight backing to recovery/isolation. */
    if(!guard_acquire(guard) || guard_aborted(guard))
        return TG_GPU_CHANNEL_FAULT;
    c->submitted=true;c->userd_offset=offset;c->last_put=gp_put;
    guard_write32(guard,io,address,gp_put,true);
    if(guard_aborted(guard))return TG_GPU_CHANNEL_FAULT;
    c->last_register=address;c->last_value=io->read32(io->context,address);
    if(guard_aborted(guard))return TG_GPU_CHANNEL_FAULT;
    if(c->last_value!=gp_put)return TG_GPU_CHANNEL_IO;
    return fault_checked(io,c,guard);
}
enum tg_gpu_channel_result tg_gpu_channel_publish(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint32_t offset,uint32_t gp_put,
    const void *owner,struct tg_gpu_channel *c)
{
    return tg_gpu_channel_publish_checked(io,power,offset,gp_put,owner,c,NULL);
}
enum tg_gpu_channel_result tg_gpu_channel_submit_entries_checked(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    uint32_t userd_bar1_offset,uint32_t gp_put,const void *owner,struct tg_gpu_channel *c,
    const struct tg_gpu_commit_guard *guard)
{
    enum tg_gpu_channel_result result;
    if(!userd_page(userd_bar1_offset) || gp_put<2 || gp_put>4)return TG_GPU_CHANNEL_ARGUMENT;
    result=tg_gpu_channel_bind_checked(io,power,root,instance,runlist,owner,c,guard);
    if(result!=TG_GPU_CHANNEL_OK)return result;
    return tg_gpu_channel_publish_checked(io,power,userd_bar1_offset,gp_put,owner,c,guard);
}
enum tg_gpu_channel_result tg_gpu_channel_submit_entries(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    uint32_t userd_bar1_offset,uint32_t gp_put,const void *owner,struct tg_gpu_channel *c)
{
    return tg_gpu_channel_submit_entries_checked(io,power,root,instance,runlist,userd_bar1_offset,gp_put,owner,c,NULL);
}
enum tg_gpu_channel_result tg_gpu_channel_submit(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,uint64_t root,uint64_t instance,uint64_t runlist,
    uint32_t userd_bar1_offset,const void *owner,struct tg_gpu_channel *c)
{
    return tg_gpu_channel_submit_entries(io,power,root,instance,runlist,userd_bar1_offset,2,owner,c);
}
enum tg_gpu_channel_result tg_gpu_channel_retire_checked(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,const void *owner,struct tg_gpu_channel *c,
    const struct tg_gpu_commit_guard *guard)
{
    uint32_t value;
    enum tg_gpu_channel_result wr;
    if(!c || !owner) return TG_GPU_CHANNEL_ARGUMENT;
    if(!c->referenced) return TG_GPU_CHANNEL_OK;
    if(c->owner!=owner) return TG_GPU_CHANNEL_BUSY;
    if(c->power!=power) return TG_GPU_CHANNEL_ARGUMENT;
    if(!ready(io,power)) return TG_GPU_CHANNEL_NOT_READY;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    c->stage=5;
    value=get(io,0x2630);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x2630,value|1,guard);
    wr=wait_bits_checked(io,c,0x2630,1,1,TG_GPU_CHANNEL_IO,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x800004);
    if(value==UINT32_MAX) return TG_GPU_CHANNEL_IO;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x800004,(value&~0xc00u)|0x800,guard);
    wr=wait_bits_checked(io,c,0x800004,1,0,TG_GPU_CHANNEL_IO,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x2634,TG_GPU_PREEMPT_TSG0,guard); /* preempt the group, not a bare CHID */
    wr=wait_bits_checked(io,c,0x2634,0x100000,0,TG_GPU_CHANNEL_TIMEOUT,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    value=get(io,0x200);
    if(value==UINT32_MAX)return TG_GPU_CHANNEL_IO;
    /* A GR context can still reference RAMIN/image backing after its FIFO
     * reader becomes idle. nvgpu gk20a_is_channel_ctx_resident explicitly
     * tests FECS CURRENT_CTX.VALID: the instance bits may remain after unload.
     * Never clear that bit ourselves to manufacture retirement evidence.
     */
    if(value&0x1000) {
        wr=wait_bits_checked(io,c,0x409b00,0x80000000,0,TG_GPU_CHANNEL_TIMEOUT,guard);
        if(wr!=TG_GPU_CHANNEL_OK) return wr;
    }
    {
        enum tg_gpu_channel_result idle=wait_engine_idle(io,c,0x2640,guard);
        if(idle!=TG_GPU_CHANNEL_OK) return idle;
        idle=wait_engine_idle(io,c,0x2648,guard);
        if(idle!=TG_GPU_CHANNEL_OK) return idle;
    }
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x2274,0,guard);
    wr=wait_bits_checked(io,c,0x2284,0x100000,0,TG_GPU_CHANNEL_TIMEOUT,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    /* With no other channels in this engine, PBDMA must become invalid/idle.
     * An inactive reader's other SRAM fields are not completion evidence.
     */
    wr=wait_bits_checked(io,c,0x3080,0xe000,0,TG_GPU_CHANNEL_TIMEOUT,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    put_checked(io,0x800000,0,guard);
    wr=wait_bits_checked(io,c,0x800000,0x80000000,0,TG_GPU_CHANNEL_IO,guard);
    if(wr!=TG_GPU_CHANNEL_OK) return wr;
    c->stage=6;
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    {
        enum tg_gpu_frontend_result fe=tg_gpu_frontend_flush_checked(io,power,&c->frontend,guard);
        if(fe==TG_GPU_FE_FAULT) return TG_GPU_CHANNEL_FAULT;
        if(fe!=TG_GPU_FE_OK) return TG_GPU_CHANNEL_FLUSH;
    }
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    {
        enum tg_gpu_tlb_result tlb=tg_gpu_tlb_invalidate_checked(io,power,c->root,guard);
        if(tlb==TG_GPU_TLB_FAULT) return TG_GPU_CHANNEL_FAULT;
        if(tlb!=TG_GPU_TLB_OK) return TG_GPU_CHANNEL_TLB;
    }
    if(guard_aborted(guard)) return TG_GPU_CHANNEL_FAULT;
    c->owner=NULL;c->referenced=c->bound=c->submitted=false;c->stage=7;
    return TG_GPU_CHANNEL_OK;
}
enum tg_gpu_channel_result tg_gpu_channel_retire(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,const void *owner,struct tg_gpu_channel *c)
{
    return tg_gpu_channel_retire_checked(io,power,owner,c,NULL);
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
