/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2019 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * MC/FIFO routing follows Switchroot nvgpu 1ae0167d, common/mc/mc_gm20b.c
 * and gk20a/fifo_gk20a.c. LIC offsets follow Linux v6.12 irq-tegra.c.
 * The GIC itself belongs to Windows and is never programmed here.
 */
#include "t214_interrupt.h"
#include "t214_resources.h"
#include <stddef.h>

#define MC_PENDING (T214_GPU_REG_BASE+0x104u)
#define MC_ENABLE (T214_GPU_REG_BASE+0x144u)
#define MC_MASK (T214_GPU_REG_BASE+0x644u)
#define FIFO_PENDING (T214_GPU_REG_BASE+0x2100u)
#define FIFO_ENABLE (T214_GPU_REG_BASE+0x2140u)
#define MC_FIFO 0x100u
#define FIFO_CHANNEL 0x80000000u
#define LIC_FIR T214_GPU_LIC_BASE
#define LIC_FIR_CLEAR (T214_GPU_LIC_BASE+8u)
#define LIC_ENABLE (T214_GPU_LIC_BASE+12u)
#define LIC_ENABLE_SET (T214_GPU_LIC_BASE+16u)
#define LIC_ENABLE_CLEAR (T214_GPU_LIC_BASE+20u)
#define LIC_CLASS (T214_GPU_LIC_BASE+24u)

static void stage(struct tg_gpu_irq_status *s,uint32_t value)
{ if(s && !s->result && !s->restore_result) s->stage=value; }
static bool valid(uint32_t r,uint32_t value)
{
    /* These are complete bitmaps, not identity/status registers. All sources
     * enabled is a valid snapshot (physical MC_MASK1 reset value is ~0u).
     * A later masked write still requires its exact owned-bit readback.
     */
    return value!=UINT32_MAX || r==MC_MASK || r==LIC_ENABLE || r==LIC_CLASS;
}
static uint32_t rd(const struct tg_gpu_power_io *io,struct tg_gpu_irq_status *s,uint32_t r)
{
    uint32_t value=io->read32(io->context,r);
    if(s && !s->result && !s->restore_result) {
        s->last_register=r;s->last_value=value;
    }
    return value;
}
static void wr(const struct tg_gpu_power_io *io,uint32_t r,uint32_t v)
{ io->write32(io->context,r,v); }
static bool set(const struct tg_gpu_power_io *io,struct tg_gpu_irq_status *s,
    uint32_t r,uint32_t v,uint32_t mask)
{
    wr(io,r,v);
    uint32_t value=rd(io,s,r);
    return valid(r,value) && !((value^v)&mask);
}
enum tg_gpu_irq_result tg_gpu_irq_arm(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,struct tg_gpu_irq_state *q,struct tg_gpu_irq_status *s)
{
    if(!io || !io->read32 || !io->write32 || !power || !power->ready ||
       !q || !s || q->saved) return TG_GPU_IRQ_ARGUMENT;
    *s=(struct tg_gpu_irq_status){0};
    stage(s,TG_GPU_IRQ_SNAPSHOT);
#define SNAP(member,reg) do { \
    q->member=rd(io,s,reg); \
    if(!valid(reg,q->member)) return s->result=TG_GPU_IRQ_REGISTER; \
} while(0)
    SNAP(mc_enable,MC_ENABLE);SNAP(mc_mask,MC_MASK);
    SNAP(fifo_enable,FIFO_ENABLE);SNAP(lic_enable,LIC_ENABLE);
    SNAP(lic_class,LIC_CLASS);
#undef SNAP
    uint32_t force=rd(io,s,LIC_FIR);
    if(force==UINT32_MAX) return s->result=TG_GPU_IRQ_REGISTER;
    if(q->mc_enable || (force&T214_GPU_LIC_NONSTALL))
        return s->result=TG_GPU_IRQ_FOREIGN;
    q->saved=true;
    stage(s,TG_GPU_IRQ_DISABLE_MC);
    if(!set(io,s,MC_ENABLE,0,3)) goto fail;
    stage(s,TG_GPU_IRQ_ROUTE_LIC);
    if(!set(io,s,LIC_CLASS,q->lic_class&~T214_GPU_LIC_NONSTALL,T214_GPU_LIC_NONSTALL)) goto fail;
    stage(s,TG_GPU_IRQ_ENABLE_LIC);
    wr(io,LIC_ENABLE_SET,T214_GPU_LIC_NONSTALL);
    uint32_t enabled=rd(io,s,LIC_ENABLE);
    if(!(enabled&T214_GPU_LIC_NONSTALL)) goto fail;
    /* Drop only an inherited channel event; leave real FIFO faults intact. */
    wr(io,FIFO_PENDING,FIFO_CHANNEL);
    stage(s,TG_GPU_IRQ_ENABLE_FIFO);
    if(!set(io,s,FIFO_ENABLE,q->fifo_enable|FIFO_CHANNEL,FIFO_CHANNEL)) goto fail;
    stage(s,TG_GPU_IRQ_MASK_MC);
    if(!set(io,s,MC_MASK,MC_FIFO,UINT32_MAX)) goto fail;
    stage(s,TG_GPU_IRQ_ENABLE_MC);
    if(!set(io,s,MC_ENABLE,1,3)) goto fail;
    q->armed=true;
    stage(s,TG_GPU_IRQ_ARMED);
    return TG_GPU_IRQ_OK;
fail:
    wr(io,MC_ENABLE,0);
    return s->result=TG_GPU_IRQ_REGISTER; /* retain the snapshot for disarm */
}
static bool restore_lic(const struct tg_gpu_power_io *io,struct tg_gpu_irq_state *q,
    struct tg_gpu_irq_status *s)
{
    uint32_t cls=rd(io,s,LIC_CLASS);
    wr(io,LIC_ENABLE_CLEAR,T214_GPU_LIC_NONSTALL);
    if(!set(io,s,LIC_CLASS,(cls&~T214_GPU_LIC_NONSTALL)|
        (q->lic_class&T214_GPU_LIC_NONSTALL),T214_GPU_LIC_NONSTALL)) return false;
    if(q->lic_enable&T214_GPU_LIC_NONSTALL) wr(io,LIC_ENABLE_SET,T214_GPU_LIC_NONSTALL);
    uint32_t enabled=rd(io,s,LIC_ENABLE);
    return !((enabled^q->lic_enable)&T214_GPU_LIC_NONSTALL);
}
enum tg_gpu_irq_result tg_gpu_irq_disarm(const struct tg_gpu_power_io *io,
    struct tg_gpu_irq_state *q,struct tg_gpu_irq_status *s)
{
    if(!io || !io->read32 || !io->write32 || !q || !s) return TG_GPU_IRQ_ARGUMENT;
    if(!q->saved) return TG_GPU_IRQ_OK;
    q->armed=false;
    stage(s,TG_GPU_IRQ_RESTORE);
    if(!set(io,s,MC_ENABLE,0,3) || !restore_lic(io,q,s)) goto fail;
    uint32_t fifo=rd(io,s,FIFO_ENABLE);
    if(fifo==UINT32_MAX) goto fail;
    if(!set(io,s,FIFO_ENABLE,(fifo&~FIFO_CHANNEL)|(q->fifo_enable&FIFO_CHANNEL),FIFO_CHANNEL) ||
       !set(io,s,MC_MASK,q->mc_mask,UINT32_MAX) || !set(io,s,MC_ENABLE,q->mc_enable,3)) goto fail;
    q->saved=false;s->restore_result=0;
    return TG_GPU_IRQ_OK;
fail:
    return s->restore_result=TG_GPU_IRQ_REGISTER;
}
enum tg_gpu_irq_result tg_gpu_irq_retire(const struct tg_gpu_power_io *io,
    struct tg_gpu_irq_state *q)
{
    if(!io || !io->read32 || !io->write32 || !q) return TG_GPU_IRQ_ARGUMENT;
    if(!q->saved) return TG_GPU_IRQ_OK;
    if(!restore_lic(io,q,NULL)) return TG_GPU_IRQ_REGISTER;
    q->saved=q->armed=false;
    return TG_GPU_IRQ_OK;
}
bool tg_gpu_irq_handle(const struct tg_gpu_power_io *io,struct tg_gpu_irq_state *q,
    struct tg_gpu_irq_status *s)
{
    if(!q->armed) return false;
    stage(s,TG_GPU_IRQ_HANDLE);
    s->mc_pending=rd(io,s,MC_PENDING);
    if(s->mc_pending==UINT32_MAX) {
        s->result=TG_GPU_IRQ_REGISTER;wr(io,MC_ENABLE,0);return true;
    }
    if(!(s->mc_pending&MC_FIFO)) return false;
    s->fifo_pending=rd(io,s,FIFO_PENDING);
    if(s->fifo_pending==UINT32_MAX || !(s->fifo_pending&FIFO_CHANNEL)) {
        s->result=s->fifo_pending==UINT32_MAX ? TG_GPU_IRQ_REGISTER : TG_GPU_IRQ_UNEXPECTED;
        wr(io,MC_ENABLE,0);return true;
    }
    /* Only W1C the nonstall channel bit; never suppress a PBDMA/MMU fault. */
    wr(io,FIFO_PENDING,FIFO_CHANNEL);wr(io,LIC_FIR_CLEAR,T214_GPU_LIC_NONSTALL);
    uint32_t pending=rd(io,s,FIFO_PENDING);
    if(pending==UINT32_MAX || (pending&FIFO_CHANNEL)) {
        s->result=pending==UINT32_MAX ? TG_GPU_IRQ_REGISTER : TG_GPU_IRQ_STICKY;
        wr(io,MC_ENABLE,0);return true;
    }
    ++s->interrupts;
    return true;
}
