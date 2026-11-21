/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2014-2018 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * RAMFC, BAR1, MMU and FIFO sequences follow Switchroot nvgpu 1ae0167d:
 * gk20a/fifo_gk20a.c, gm20b/fifo_gm20b.c, common/{bus,fb,mc} and hw headers.
 * Packet definitions are from Mesa's NVIDIA class headers. See NOTICE.
 */
#include "execute.h"
#include "gm20b_mmu.h"
#include "gpfifo.h"
#include "fault.h"
#include "runlist.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"

#define GPU 0x57000000u
#define BAR1 0x58000000u
#define INST 0x40000u
#define USERD 0x41000u
#define RING 0x42000u
#define PUSH 0x43000u
#define FENCE 0x44000u
#define RUNLIST 0x45000u
#define FLUSH 0x46000u
#define DEBUG_RD 0x47000u
#define DEBUG_WR 0x48000u
#define RING_VA 0x10000u
#define PUSH_VA 0x11000u
#define FENCE_VA 0x12000u
#define USERD_VA TG_GPU_BOOT_USERD_BAR1_OFFSET

static void release_packet(uint32_t *p, uint64_t va, uint32_t value)
{
    p[0] = NVDEF(NVB06F, DMA_INCR, OPCODE, VALUE) |
           NVVAL(NVB06F, DMA_INCR, COUNT, 4) |
           NVVAL(NVB06F, DMA_INCR, ADDRESS, NVB06F_SEMAPHOREA >> 2);
    p[1] = (uint32_t)(va >> 32);
    p[2] = (uint32_t)va;
    p[3] = value;
    p[4] = NVDEF(NVB06F, SEMAPHORED, OPERATION, RELEASE) |
           NVDEF(NVB06F, SEMAPHORED, RELEASE_WFI, DIS) |
           NVDEF(NVB06F, SEMAPHORED, RELEASE_SIZE, 4BYTE);
}

bool tg_gpu_prepare_arena(void *cpu, uint64_t physical, size_t bytes,
                          const uint32_t expected[2])
{
    uint64_t *pd = (uint64_t *)cpu, *pt;
    uint32_t *instance, *userd, *push, *fence, *runlist;
    struct tg_gpu_gpfifo_entry *ring;
    uint32_t i;
    uint64_t entry;
    if (!cpu || !expected || ((uintptr_t)cpu & 0xfff) || (physical & 0xfff) ||
        bytes < TG_GPU_ARENA_BYTES || physical < 0x80000000 ||
        physical > UINT64_C(0x100000000) - TG_GPU_ARENA_BYTES ||
        expected[0] == 0xdeadbeef || expected[1] == 0xdeadbeef || expected[0] == expected[1])
        return false;
    /* All backing pages are owned by the caller, not arbitrary OS memory. */
    for (i = 0; i < TG_GPU_ARENA_BYTES / 4; ++i) ((uint32_t *)cpu)[i] = 0;
    pt = (uint64_t *)((uint8_t *)cpu + 0x20000);
    instance = (uint32_t *)((uint8_t *)cpu + INST);
    userd = (uint32_t *)((uint8_t *)cpu + USERD);
    push = (uint32_t *)((uint8_t *)cpu + PUSH);
    fence = (uint32_t *)((uint8_t *)cpu + FENCE);
    ring = (struct tg_gpu_gpfifo_entry *)((uint8_t *)cpu + RING);
    runlist = (uint32_t *)((uint8_t *)cpu + RUNLIST);
    if (!tg_gpu_pde_small(physical + 0x20000, &pd[0]) ||
        !tg_gpu_instance_pdb(physical, &entry)) return false;
    instance[128] = (uint32_t)entry; /* volatile PDB, SoC DRAM target */
    instance[129] = (uint32_t)(entry >> 32);
    instance[130] = 0x00fff000; /* BAR1 and channel VA limited to 16 MiB */
    if (!tg_gpu_pte_4k(physical + RING, true, true, &pt[RING_VA >> 12]) ||
        !tg_gpu_pte_4k(physical + PUSH, true, true, &pt[PUSH_VA >> 12]) ||
        !tg_gpu_pte_4k(physical + FENCE, false, true, &pt[FENCE_VA >> 12]) ||
        !tg_gpu_pte_4k(physical + USERD, false, true, &pt[USERD_VA >> 12])) return false;

    /* Start empty, then publish both entries through BAR1 after binding, as
     * gk20a_fifo_userd_gp_put does. Saved GP_PUT alone is not a notification.
     */
    instance[2] = (uint32_t)(physical + USERD); /* VIDMEM denotes SoC DRAM */
    instance[3] = 0;
    instance[4] = 0xface; /* hardware signature */
    instance[12] = 0x102; /* acquire retry mantissa/exp 2; no acquire emitted */
    instance[18] = RING_VA;
    instance[19] = 9u << 16; /* 512 GPFIFO entries */
    instance[33] = 0x20400000; /* main/user/first/incrementing pushbuffer */
    instance[37] = 0x30000001; /* subdevice 1 active, channel DMA */
    instance[39] = 0x100; /* PB Fermi1, GP/MP Fermi0 */
    instance[43] = 0x1f; /* software engine; methods below are FIFO methods */
    instance[62] = 0x10003080; /* runlist timeslice */
    instance[63] = 0x10000010; /* pushbuffer timeslice */
    userd[0] = 0x55aa3377; /* BAR1 visibility check only */
    fence[0] = fence[1] = 0xdeadbeef;
    release_packet(push, FENCE_VA, expected[0]);
    release_packet(push + 5, FENCE_VA + 4, expected[1]);
    if (!tg_gpu_build_gpfifo(PUSH_VA, 5, false, &ring[0]) ||
        !tg_gpu_build_gpfifo(PUSH_VA + 20, 5, true, &ring[1])) return false;
    tg_gpu_runlist_single(runlist);
    return true;
}

static bool prepare_irq_packets(void *cpu,const uint32_t expected[2])
{
    uint32_t *push=(uint32_t *)((uint8_t *)cpu+PUSH);
    struct tg_gpu_gpfifo_entry *ring=(void *)((uint8_t *)cpu+RING);
    for(unsigned i=0;i<2;++i) {
        release_packet(push+i*7,FENCE_VA+i*4,expected[i]);
        push[i*7+5]=NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE)|
            NVVAL(NVB06F,DMA_INCR,COUNT,1)|NVVAL(NVB06F,DMA_INCR,SUBCHANNEL,6)|
            NVVAL(NVB06F,DMA_INCR,ADDRESS,NVB06F_NON_STALL_INTERRUPT>>2);
        push[i*7+6]=0; /* Nouveau nvc0_fence_emit32's notification handle. */
        if(!tg_gpu_build_gpfifo(PUSH_VA+i*28,7,i!=0,&ring[i])) return false;
    }
    return true;
}
bool tg_gpu_prepare_3d_arena(void *cpu,uint64_t physical,size_t bytes,const uint32_t expected[2])
{
    if(!tg_gpu_prepare_arena(cpu,physical,bytes,expected)) return false;
    uint32_t *push=(uint32_t *)((uint8_t *)cpu+PUSH);
    struct tg_gpu_gpfifo_entry *ring=(void *)((uint8_t *)cpu+RING);
    if(!tg_gpu_build_gr_engine_packet(push,TG_GPU_GR_ENGINE_FIRST_WORDS,FENCE_VA,expected[0],true) ||
       !tg_gpu_build_gr_engine_packet(push+TG_GPU_GR_ENGINE_FIRST_WORDS,
           TG_GPU_GR_ENGINE_NEXT_WORDS,FENCE_VA+4,expected[1],false)) return false;
    return tg_gpu_build_gpfifo(PUSH_VA,TG_GPU_GR_ENGINE_FIRST_WORDS,false,&ring[0]) &&
        tg_gpu_build_gpfifo(PUSH_VA+4*TG_GPU_GR_ENGINE_FIRST_WORDS,
                            TG_GPU_GR_ENGINE_NEXT_WORDS,true,&ring[1]);
}
static bool prepare_clear_packets(void *cpu,const struct tg_gpu_gr_work *work,
    const uint32_t expected[2],bool draw,struct tg_gpu_draw_status *status)
{
    uint32_t *push=(uint32_t *)((uint8_t *)cpu+PUSH);
    struct tg_gpu_gpfifo_entry *ring=(void *)((uint8_t *)cpu+RING);
    for(unsigned i=0;i<2;++i) {
        uint32_t words=0;
        unsigned stride=draw?TG_GPU_DRAW_MAX_WORDS:TG_GPU_CLEAR_MAX_WORDS;
        bool ok=draw&&i?
            tg_gpu_build_draw_packet(push+i*stride,stride,TG_GPU_GR_WORK_VA+TG_GPU_DRAW_OFFSET,
                FENCE_VA+i*4,expected[i],&words):
            tg_gpu_build_clear_packet(push+i*stride,stride,TG_GPU_GR_WORK_VA+TG_GPU_CLEAR_OFFSET,
                FENCE_VA+i*4,expected[i],i==0,&words);
        if(!ok || !tg_gpu_build_gpfifo(PUSH_VA+i*4*stride,words,i!=0,&ring[i]))return false;
    }
    uint32_t *pixels=(uint32_t *)((uint8_t *)work->cpu+TG_GPU_CLEAR_OFFSET);
    /* CPU only initializes the sentinel; ROP must produce the final pixels. */
    for(unsigned i=0;i<TG_GPU_CLEAR_BYTES/4;++i) pixels[i]=TG_GPU_CLEAR_SENTINEL;
    return !draw || tg_gpu_prepare_draw_code((uint8_t *)work->cpu+TG_GPU_DRAW_OFFSET,
                                              TG_GPU_DRAW_BYTES,status);
}

bool tg_gpu_prepare_gr_arena(void *cpu, uint64_t physical, size_t bytes,
                            const struct tg_gpu_gr_payload *firmware)
{
    uint64_t *pt, pdb;
    uint32_t *inst;
    uint8_t *surface;
    uint32_t words[17], count, pages;
    if (!cpu || ((uintptr_t)cpu & 0xfff) || (physical & 0xfff) ||
        bytes < TG_GPU_ARENA_BYTES || physical < 0x80000000 ||
        physical > UINT64_C(0x100000000)-TG_GPU_ARENA_BYTES ||
        !firmware || !firmware->surface || !firmware->image.size ||
        firmware->image.size > TG_GPU_GR_SURFACE_BYTES) return false;
    for (unsigned i=0;i<2;++i)
        if (tg_gpu_gr_boot_header(&firmware->image,i,TG_GPU_GR_SURFACE_VA,words,&count)
            != TG_GPU_GR_FW_OK) return false;
    if (!tg_gpu_instance_pdb(physical,&pdb)) return false;
    inst = (uint32_t *)((uint8_t *)cpu+TG_GPU_GR_INSTANCE_OFFSET);
    inst[128]=(uint32_t)pdb; inst[129]=(uint32_t)(pdb>>32);
    inst[130]=0x00fff000;
    pt = (uint64_t *)((uint8_t *)cpu+0x20000);
    pages = (firmware->image.size+4095)/4096;
    for (uint32_t i=0;i<pages;++i)
        if (!tg_gpu_pte_4k(physical+TG_GPU_GR_SURFACE_OFFSET+i*4096,true,true,
                          &pt[(TG_GPU_GR_SURFACE_VA>>12)+i])) return false;
    surface=(uint8_t *)cpu+TG_GPU_GR_SURFACE_OFFSET;
    for (uint32_t i=0;i<firmware->image.size;++i) surface[i]=firmware->surface[i];
    return true;
}

/* Install only driver-owned pages before the root becomes visible. The signed
 * ACR image is packed later, once hardware supplies the protected WPR range.
 * WPR itself is never mapped into the CPU or this GPU address space.
 */
bool tg_gpu_prepare_acr_arena(void *cpu, uint64_t physical, size_t bytes)
{
    uint64_t *pt, pdb;
    uint32_t *inst;
    if (!cpu || ((uintptr_t)cpu&4095) || (physical&4095) ||
        bytes<TG_GPU_ARENA_BYTES || physical<0x80000000 ||
        physical>UINT64_C(0x100000000)-TG_GPU_ARENA_BYTES ||
        !tg_gpu_instance_pdb(physical,&pdb)) return false;
    pt=(uint64_t *)((uint8_t *)cpu+0x20000);
    inst=(uint32_t *)((uint8_t *)cpu+TG_GPU_ACR_INSTANCE_OFFSET);
    inst[128]=(uint32_t)pdb; inst[129]=(uint32_t)(pdb>>32);
    inst[130]=0x00fff000;
    for (unsigned i=0;i<TG_GPU_ACR_SURFACE_BYTES/4096;++i)
        if (!tg_gpu_pte_4k(physical+TG_GPU_ACR_SURFACE_OFFSET+i*4096,true,true,
                          &pt[(TG_GPU_ACR_SURFACE_VA>>12)+i])) return false;
    return true;
}

static uint32_t ex_read(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context, GPU + reg); }
static void ex_write(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t val)
{
    io->write32(io->context, GPU + reg, val);
    (void)ex_read(io, reg);
}
static bool ex_wait(const struct tg_gpu_power_io *io, uint32_t reg,
                    uint32_t mask, uint32_t value)
{
    uint32_t i;
    for (i = 0; i < 1000; ++i) {
        if ((ex_read(io, reg) & mask) == value) return true;
        io->delay_us(io->context, 5);
    }
    return false;
}
void tg_gpu_execute_snapshot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_execution *state, struct tg_gpu_status *s)
{
    s->fifo_interrupt = ex_read(io, 0x2100);
    s->mmu_fault = ex_read(io, 0x259c);
    s->pbdma_interrupt = ex_read(io, 0x40108);
    s->channel_status = ex_read(io, 0x800004);
    s->scheduler_after = ex_read(io, 0x2630);
    s->scheduler_error = ex_read(io, 0x254c); /* FIFO_INTR_SCHED_ERROR, not RUNLIST_FAULT */
    s->bind_error = ex_read(io, 0x252c);
    s->chsw_error = ex_read(io, 0x256c);
    s->engine_status[0] = ex_read(io, 0x2640);
    s->engine_status[1] = ex_read(io, 0x2648);
    s->runlist_base = ex_read(io, 0x2280);
    s->runlist_status = ex_read(io, 0x2284);
    if (s->pbdma_enable_after != UINT32_MAX && (s->pbdma_enable_after & 1)) {
        s->pbdma_context=(struct tg_gpu_pbdma_context){0};
        s->pbdma_context.intr_enable=ex_read(io,0x4010c);
        s->pbdma_context.intr_stall=ex_read(io,0x4013c);
        s->pbdma_context.fifo_pending=ex_read(io,0x25a0);
        s->pbdma_status = ex_read(io, 0x3080);
        s->pbdma_gp_base = s->pbdma_gp_fetch =
            s->pbdma_gp_get = s->pbdma_gp_put = UINT32_MAX;
        if (tg_gpu_pbdma_owns_slot(s->pbdma_status)) {
            s->pbdma_gp_base = ex_read(io, 0x40048);
            s->pbdma_gp_fetch = ex_read(io, 0x40050);
            s->pbdma_gp_get = ex_read(io, 0x40014);
            s->pbdma_gp_put = ex_read(io, 0x40000);
            s->pbdma_context.target=ex_read(io,0x400ac);
            s->pbdma_context.signature=ex_read(io,0x40010);
            s->pbdma_context.method0=ex_read(io,0x400c0);
            s->pbdma_context.data0=ex_read(io,0x400c4);
            s->pbdma_context.pb_header=ex_read(io,0x40084);
            s->pbdma_context.header_shadow=ex_read(io,0x40118);
            s->pbdma_context.subdevice=ex_read(io,0x40094);
        }
        s->pbdma_interrupt_1 = ex_read(io, 0x40148);
    }
    if (state->channel_bound) {
        s->userd_gp_get = io->read32(io->context, BAR1 + USERD_VA + 34 * 4);
        s->userd_gp_put = io->read32(io->context, BAR1 + USERD_VA + 35 * 4);
    }
    if (state->gr_owned && s->gr.stage >= 8) {
        tg_gpu_gr_snapshot(io, &s->gr);
        if(s->gr_channel.class_id==TG_GPU_GR_ENGINE_CLASS)
            s->gr_channel.current_context=s->gr.current_context;
    }
    tg_gpu_frontend_snapshot(io, &s->frontend);
}

static bool ex_fault(const struct tg_gpu_power_io *io)
{
    struct tg_gpu_fault_status status;
    return tg_gpu_fault_poll(io, &status);
}
static bool gr_fault(const struct tg_gpu_power_io *io,struct tg_gpu_status *s)
{
    tg_gpu_gr_snapshot(io,&s->gr);
    bool fault=s->gr.gr_interrupt || s->gr.fecs_error_mailbox || s->gr.fecs_host_interrupt;
    if(fault && !s->method_error.trapped_address && !s->method_error.status) {
        /* Capture before reset/cleanup. Never clear or acknowledge an error
         * here; a rejected method must not become a successful report.
         */
        s->method_error.status=ex_read(io,0x400110);
        s->method_error.trapped_address=ex_read(io,0x400704);
        s->method_error.data_lo=ex_read(io,0x400708);
        s->method_error.data_hi=ex_read(io,0x40070c);
        s->method_error.exception=ex_read(io,0x400108);
        s->method_error.fe_esr=ex_read(io,0x404000);
        s->method_error.mme_esr=ex_read(io,0x404490);
        s->method_error.scc_esr=ex_read(io,0x408030);
    }
    return fault;
}

static enum tg_gpu_execute_result boot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, void *cpu, uint64_t physical,
    const struct tg_gpu_gr_payload *firmware, const struct tg_gpu_gr_work *work,
    const struct tg_gpu_execution_notify *notify,bool three_d,bool clear,bool draw,
    struct tg_gpu_execution *state,
    struct tg_gpu_status *s)
{
    volatile uint32_t *fence, *userd;
    uint32_t i, intr_stall;
    if (!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
        !power->owned || !power->ready || !cpu || !state || !s || state->owned ||
        (notify && (!notify->arm || !notify->wait)) ||
        (three_d && (!firmware || !work || work->bytes<TG_GPU_GR_CHANNEL_WORK_BYTES || notify)) ||
        !(three_d?tg_gpu_prepare_3d_arena(cpu,physical,TG_GPU_ARENA_BYTES,s->expected):
                   tg_gpu_prepare_arena(cpu,physical,TG_GPU_ARENA_BYTES,s->expected)))
        return TG_GPU_EXEC_ARGUMENT;
    if (firmware && !tg_gpu_prepare_gr_arena(cpu,physical,TG_GPU_ARENA_BYTES,firmware))
        return TG_GPU_EXEC_ARGUMENT;
    if (firmware && firmware->acr &&
        !tg_gpu_prepare_acr_arena(cpu,physical,TG_GPU_ARENA_BYTES))
        return TG_GPU_EXEC_ARGUMENT;
    if(work && (!firmware || !(three_d?
        tg_gpu_gr_context_prepare_channel(cpu,physical,TG_GPU_ARENA_BYTES,work):
        tg_gpu_gr_context_prepare(cpu,physical,TG_GPU_ARENA_BYTES,work))))
        return TG_GPU_EXEC_ARGUMENT;
    if(notify && !prepare_irq_packets(cpu,s->expected)) return TG_GPU_EXEC_ARGUMENT;
    if(clear && !prepare_clear_packets(cpu,work,s->expected,draw,&s->draw)) return TG_GPU_EXEC_ARGUMENT;
    if(notify) s->irq_expected=2;
    if(three_d) {
        s->gr_channel.class_id=TG_GPU_GR_ENGINE_CLASS;
        s->gr_channel.image_result=UINT32_MAX;
    }
    fence = (volatile uint32_t *)((uint8_t *)cpu + FENCE);
    userd = (volatile uint32_t *)((uint8_t *)cpu + USERD);
    s->observed[0] = fence[0]; s->observed[1] = fence[1];
    state->stage = 1;
    state->fifo_ready = false;
    s->mc_enable_before = ex_read(io, 0x200);
    s->channel_instance_before = UINT32_MAX; /* not readable while held reset */
    if (s->mc_enable_before == UINT32_MAX) return TG_GPU_EXEC_FIFO_INIT;
    if (s->mc_enable_before & 0x100)
        s->channel_instance_before = ex_read(io, 0x800000);
    state->owned = true;
    /* power_up proved exclusive ownership by requiring reset + clamp before
     * releasing the GPU. A cold CCSR slot is not an ownership test. Follow
     * Switchroot: reset/enable PFIFO, then initialize our unbound slot.
     */
    ex_write(io, 0x140, 0); ex_write(io, 0x144, 0);
    ex_write(io, 0x200, s->mc_enable_before & ~0x100u);
    io->delay_us(io->context, 20);
    ex_write(io, 0x200, s->mc_enable_before | 0x100);
    io->delay_us(io->context, 20);
    s->mc_enable_after = ex_read(io, 0x200);
    if (s->mc_enable_after == UINT32_MAX || !(s->mc_enable_after & 0x100))
        return TG_GPU_EXEC_FIFO_INIT;
    if (tg_gpu_frontend_fifo(io, power, &s->frontend) != TG_GPU_FE_OK)
        return TG_GPU_EXEC_FRONTEND;
    s->channel_instance_after = ex_read(io, 0x800000);
    if (s->channel_instance_after == UINT32_MAX) return TG_GPU_EXEC_FIFO_INIT;
    ex_write(io, 0x800004, 0x800); /* disable our channel before unbinding */
    ex_write(io, 0x800000, 0);
    s->channel_instance_after = ex_read(io, 0x800000);
    if (s->channel_instance_after & 0x80000000) return TG_GPU_EXEC_BUSY;
    state->fifo_ready = true;
    /* Channel enable and runlist scheduling are separate controls. Own only
     * runlist zero: keep it blocked while binding, then explicitly allow it
     * after its list is committed (Switchroot fifo_sched_disable_rw).
     */
    s->scheduler_before = ex_read(io, 0x2630);
    if (s->scheduler_before == UINT32_MAX) return TG_GPU_EXEC_SCHEDULER;
    ex_write(io, 0x2630, s->scheduler_before | 1);
    s->scheduler_after = ex_read(io, 0x2630);
    if (s->scheduler_after == UINT32_MAX || !(s->scheduler_after & 1))
        return TG_GPU_EXEC_SCHEDULER;
    /* PFIFO reset/enable does not enable its pushbuffer readers. Nouveau's
     * gk104_fifo_init_pbdmas explicitly programs MC_ENABLE_PB (0x204).
     * Own only PBDMA0, and require that it serves our single runlist (0).
     */
    s->pbdma_enable_before = ex_read(io, 0x204);
    s->pbdma_runlist_map = ex_read(io, 0x2390);
    if (s->pbdma_enable_before == UINT32_MAX || s->pbdma_runlist_map == UINT32_MAX ||
        !(s->pbdma_runlist_map & 1)) return TG_GPU_EXEC_PBDMA_INIT;
    state->old_pbdma_enable = s->pbdma_enable_before;
    state->pbdma_owned = true;
    ex_write(io, 0x204, state->old_pbdma_enable | 1);
    s->pbdma_enable_after = ex_read(io, 0x204);
    if (s->pbdma_enable_after == UINT32_MAX || !(s->pbdma_enable_after & 1))
        return TG_GPU_EXEC_PBDMA_INIT;
    /* GM20B selects Tegra SMMU translation with GPU address bit 34:
     * Switchroot gk20a_mm_get_iommu_bit + nvgpu_mem_iommu_translate.
     * prepare_arena accepts only physical backing below 4 GiB, so every
     * address programmed here leaves that tag clear. Do not rewrite global
     * MC_SMMU_GPU/GPUB_ASID or require those registers to read as zero.
     * The GPU's own MMU still limits this channel to the private arena.
     */
    /* Match Switchroot's local fault enables. MC interrupt outputs remain
     * disabled above, so polling does not require an ISR. In particular,
     * bind/scheduler/channel-switch faults must not become generic timeouts.
     */
    intr_stall = ex_read(io, 0x4013c) & ~0x100u; /* LB request is not fatal */
    ex_write(io, 0x4013c, intr_stall);
    ex_write(io, 0x2100, UINT32_MAX);
    ex_write(io, 0x40108, UINT32_MAX); ex_write(io, 0x40148, UINT32_MAX);
    ex_write(io, 0x4010c, intr_stall);
    ex_write(io, 0x4014c, ex_read(io, 0x40140) & ~1u); /* unused HCE illegal-op */
    ex_write(io, 0x2140, TG_GPU_FIFO_ERROR_MASK | 0x40000000u);
    ex_write(io, 0x2528, 0x80000000);
    ex_write(io, 0x2a00, UINT32_MAX);

    state->stage = 2;
    if (tg_gpu_frontend_memory(io, power, &s->frontend) != TG_GPU_FE_OK)
        return TG_GPU_EXEC_FRONTEND;
    ex_write(io, 0x100c80, ex_read(io, 0x100c80) | 0x800);
    ex_write(io, 0x100c10, (uint32_t)((physical + FLUSH) >> 8));
    /* nvgpu_aperture_mask resolves to VIDMEM (0) throughout GM20B, not
     * only in leaf PTEs: debug buffers, TLB, BAR1, RAMFC and runlist too.
     */
    ex_write(io, 0x100cc8, (uint32_t)((physical + DEBUG_WR) >> 8));
    ex_write(io, 0x100ccc, (uint32_t)((physical + DEBUG_RD) >> 8));
    /* Switchroot gk20a_finalize_poweron -> gm20b_fb_set_mmu_debug_mode(false):
     * explicitly clear FB_MMU_DEBUG_CTRL.DEBUG (bit 16) rather than inheriting
     * reset state, while preserving all non-debug fields.
     */
    {
        uint32_t debug_ctrl = ex_read(io, 0x100cc4);
        if (debug_ctrl == UINT32_MAX) return TG_GPU_EXEC_MMU_TIMEOUT;
        ex_write(io, 0x100cc4, debug_ctrl & ~0x10000u);
    }
    for (i = 0; i < 1000 && !(ex_read(io, 0x100c80) & 0x00ff0000); ++i)
        io->delay_us(io->context, 2);
    if (i == 1000) return TG_GPU_EXEC_MMU_TIMEOUT;
    ex_write(io, 0x100cb8, (uint32_t)(physical >> 8));
    ex_write(io, 0x100cbc, 0x80000001);
    if (!ex_wait(io, 0x100c80, 0x8000, 0x8000)) return TG_GPU_EXEC_MMU_TIMEOUT;
    ex_write(io, 0x1704, (uint32_t)((physical + INST) >> 12) | 0x80000000);
    if (!ex_wait(io, 0x1710, 3, 0)) return TG_GPU_EXEC_BAR1;
    if (tg_gpu_frontend_flush(io, power, &s->frontend) != TG_GPU_FE_OK)
        return TG_GPU_EXEC_FRONTEND;
    s->bar1_phase = 1; s->bar1_expected = 0x55aa3377;
    s->bar1_read = io->read32(io->context, BAR1 + USERD_VA); s->bar1_cpu = userd[0];
    if (s->bar1_read != s->bar1_expected)
        return TG_GPU_EXEC_BAR1;
    /* Complete the upstream two-way BAR1 test before enabling snoop. Leave
     * USERD reserved words and GP_GET/GP_PUT clean; this is CPU MMIO, never
     * used as evidence of GPU command execution or as a fence value.
     */
    s->bar1_phase = 2; s->bar1_expected = 0x337755aa;
    io->write32(io->context, BAR1 + USERD_VA, s->bar1_expected);
    s->bar1_read = io->read32(io->context, BAR1 + USERD_VA); s->bar1_cpu = userd[0];
    if (s->bar1_read != s->bar1_expected)
        return TG_GPU_EXEC_BAR1;
    s->bar1_phase = 3;
    if (tg_gpu_frontend_sync_memory(io, power, &s->frontend) != TG_GPU_FE_OK)
        return TG_GPU_EXEC_FRONTEND;
    s->bar1_cpu = userd[0];
    if (s->bar1_cpu != s->bar1_expected) return TG_GPU_EXEC_BAR1;
    s->bar1_phase = 4; s->bar1_expected = 0;
    io->write32(io->context, BAR1 + USERD_VA, 0);
    if (tg_gpu_frontend_sync_memory(io, power, &s->frontend) != TG_GPU_FE_OK)
        return TG_GPU_EXEC_FRONTEND;
    s->bar1_read = io->read32(io->context, BAR1 + USERD_VA); s->bar1_cpu = userd[0];
    if (s->bar1_read || s->bar1_cpu) return TG_GPU_EXEC_BAR1;
    s->bar1_phase = 5;
    ex_write(io, 0x2254, (USERD_VA >> 12) | 0x10000000); /* USERD BAR1 snoop */

    state->stage = 7; /* distinguish GR preamble failure from BAR1 failure */
    state->gr_owned = true;
    if (tg_gpu_frontend_gr_prepare(io, power, &s->frontend) != TG_GPU_FE_OK)
        return TG_GPU_EXEC_FRONTEND;
    if (firmware) {
        state->stage = 8;
        s->gr.priv_security=ex_read(io,0x21434);
        if (s->gr.priv_security && s->gr.priv_security!=UINT32_MAX && firmware->acr) {
            struct tg_gpu_acr_image image;
            struct tg_gpu_acr_addresses addresses;
            uint32_t descriptor[19];
            state->stage=9; state->acr_owned=true;
            if (tg_gpu_acr_hw_prepare(io,power,&s->acr)!=TG_GPU_ACR_HW_OK)
                return TG_GPU_EXEC_ACR_BOOT;
            s->acr_firmware_result=tg_gpu_acr_prepare(firmware->acr,
                (s->acr.scp_status&(1u<<20))!=0,&image);
            if (s->acr_firmware_result) return TG_GPU_EXEC_ACR_BOOT;
            /* Switchroot APP_VERSION_GM20B uses the 32-byte cmdline_args_v3. */
            if (image.pmu_version!=20490253u || image.size>TG_GPU_ACR_SURFACE_BYTES ||
                (physical<s->acr.wpr_base+s->acr.wpr_size &&
                 physical+TG_GPU_ARENA_BYTES>s->acr.wpr_base) ||
                (work && work->physical<s->acr.wpr_base+s->acr.wpr_size &&
                 work->physical+(three_d?TG_GPU_GR_CHANNEL_WORK_BYTES:TG_GPU_GR_WORK_BYTES)>s->acr.wpr_base)) {
                s->acr_firmware_result=TG_GPU_ACR_ADDRESS;
                return TG_GPU_EXEC_ACR_BOOT;
            }
            addresses=(struct tg_gpu_acr_addresses){TG_GPU_ACR_SURFACE_VA,
                physical+TG_GPU_ACR_SURFACE_OFFSET,s->acr.wpr_base,s->acr.wpr_size,
                s->acr.dmem_size,32};
            s->acr_firmware_result=tg_gpu_acr_pack(&image,&addresses,
                (uint8_t *)cpu+TG_GPU_ACR_SURFACE_OFFSET,TG_GPU_ACR_SURFACE_BYTES,descriptor);
            if (s->acr_firmware_result) return TG_GPU_EXEC_ACR_BOOT;
            /* Page tables were installed before BAR1, but the staged firmware
             * was written afterward. Publish it before any PMU DMA can start.
             */
            if (tg_gpu_frontend_sync_memory(io,power,&s->frontend)!=TG_GPU_FE_OK)
                return TG_GPU_EXEC_FRONTEND;
            if (tg_gpu_acr_hw_boot(io,power,&image,descriptor,
                                  physical+TG_GPU_ACR_INSTANCE_OFFSET,&s->acr)!=TG_GPU_ACR_HW_OK)
                return TG_GPU_EXEC_ACR_BOOT;
            state->stage=10;
            if (tg_gpu_pmu_start(io,power,&s->acr,&s->pmu)!=TG_GPU_PMU_OK)
                return TG_GPU_EXEC_PMU_BOOT;
            state->stage=8;
            if (tg_gpu_gr_boot_secure(io,power,&s->frontend,&firmware->netlist,&firmware->image,
                    TG_GPU_GR_SURFACE_VA,physical+TG_GPU_GR_INSTANCE_OFFSET,&s->acr,&s->pmu,&s->gr)
                !=TG_GPU_GR_OK) return TG_GPU_EXEC_GR_BOOT;
        } else if (tg_gpu_gr_boot(io,power,&s->frontend,&firmware->netlist,&firmware->image,
                          TG_GPU_GR_SURFACE_VA,physical+TG_GPU_GR_INSTANCE_OFFSET,&s->gr)
            != TG_GPU_GR_OK) return TG_GPU_EXEC_GR_BOOT;
        if (tg_gpu_gr_gpc_mmu(io,power,&s->frontend,&s->gr)!=TG_GPU_GR_OK)
            return TG_GPU_EXEC_GR_BOOT;
        state->stage=11;
        if (tg_gpu_frontend_ce_prepare(io,power,&s->frontend,&state->ce_reset_mask)!=TG_GPU_FE_OK)
            return TG_GPU_EXEC_FRONTEND;
        if(work) {
            state->stage=12;
            if(tg_gpu_gr_context_init(io,power,&s->frontend,&firmware->netlist,&s->gr,
                (uint32_t *)((uint8_t *)cpu+INST),physical+INST,work,&s->graphics)!=TG_GPU_GR_CTX_OK)
                return TG_GPU_EXEC_GR_CONTEXT;
            if(three_d) {
                state->stage=14;
                s->gr_channel.image_result=tg_gpu_gr_context_load_image(io,power,
                    &s->frontend,(uint32_t *)((uint8_t *)cpu+INST),work,&s->graphics,1);
                if(s->gr_channel.image_result) return TG_GPU_EXEC_GR_ENGINE;
                s->gr_channel.image_ready=1;
            }
        }
    }
    if(notify) {
        state->stage=13;
        if(!notify->arm(notify->context)) return TG_GPU_EXEC_IRQ;
    }
    state->boot_ready=true;
    return TG_GPU_EXEC_OK;
}

enum tg_gpu_execute_result tg_gpu_boot_graphics(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,void *cpu,uint64_t physical,
    const struct tg_gpu_gr_payload *firmware,const struct tg_gpu_gr_work *work,
    struct tg_gpu_execution *state,struct tg_gpu_status *s)
{
    if(!firmware || !work || !s)return TG_GPU_EXEC_ARGUMENT;
    return boot(io,power,cpu,physical,firmware,work,NULL,true,false,false,state,s);
}

enum tg_gpu_execute_result tg_gpu_boot_map_userd(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,void *cpu,uint64_t physical,uint64_t userd,
    const struct tg_gpu_execution *state,struct tg_gpu_status *s)
{
    uint64_t pte;uint32_t value;
    if(!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
       !power->owned || !power->ready || !state || !state->boot_ready || state->channel_bound ||
       !cpu || ((uintptr_t)cpu&4095) || physical<0x80000000 || (physical&4095) ||
       physical>UINT64_C(0x100000000)-TG_GPU_ARENA_BYTES || !s ||
       userd<0x80000000 || userd>UINT64_C(0xfffff000) || (userd&4095) ||
       (userd>=physical && userd<physical+TG_GPU_ARENA_BYTES) ||
       !tg_gpu_pte_4k(userd,false,true,&pte))return TG_GPU_EXEC_ARGUMENT;
    value=ex_read(io,0x800000);
    if(value==UINT32_MAX || (value&0x80000000))return TG_GPU_EXEC_BUSY;
    value=ex_read(io,0x2630);
    if(value==UINT32_MAX || !(value&1))return TG_GPU_EXEC_BUSY;
    if(ex_read(io,0x1704)!=((uint32_t)((physical+INST)>>12)|0x80000000))return TG_GPU_EXEC_BAR1;
    s->bar1_phase=6;
    ex_write(io,0x2254,0); /* No snoop while replacing its translation. */
    ((uint64_t *)((uint8_t *)cpu+0x20000))[USERD_VA>>12]=pte;
    /* Retain a consistent prototype RAMFC; application RAMIN is independent. */
    ((uint32_t *)((uint8_t *)cpu+INST))[2]=(uint32_t)userd;
    if(tg_gpu_frontend_sync_memory(io,power,&s->frontend)!=TG_GPU_FE_OK)return TG_GPU_EXEC_FRONTEND;
    if(tg_gpu_tlb_invalidate(io,power,physical)!=TG_GPU_TLB_OK)return TG_GPU_EXEC_MMU_TIMEOUT;
    ex_write(io,0x2254,(USERD_VA>>12)|0x10000000);
    s->userd_gp_get=io->read32(io->context,BAR1+USERD_VA+34*4);
    s->userd_gp_put=io->read32(io->context,BAR1+USERD_VA+35*4);
    if(s->userd_gp_get || s->userd_gp_put)return TG_GPU_EXEC_BAR1;
    s->bar1_phase=7;return TG_GPU_EXEC_OK;
}

static enum tg_gpu_execute_result channel_result(enum tg_gpu_channel_result r)
{
    switch(r) {
    case TG_GPU_CHANNEL_OK:return TG_GPU_EXEC_OK;
    case TG_GPU_CHANNEL_ARGUMENT:return TG_GPU_EXEC_ARGUMENT;
    case TG_GPU_CHANNEL_BUSY:return TG_GPU_EXEC_BUSY;
    case TG_GPU_CHANNEL_TLB:return TG_GPU_EXEC_MMU_TIMEOUT;
    case TG_GPU_CHANNEL_TIMEOUT:return TG_GPU_EXEC_RUNLIST;
    case TG_GPU_CHANNEL_FAULT:return TG_GPU_EXEC_FAULT;
    case TG_GPU_CHANNEL_FLUSH:return TG_GPU_EXEC_FRONTEND;
    default:return TG_GPU_EXEC_FIFO_INIT;
    }
}

static enum tg_gpu_execute_result execute(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, void *cpu, uint64_t physical,
    const struct tg_gpu_gr_payload *firmware, const struct tg_gpu_gr_work *work,
    const struct tg_gpu_execution_notify *notify,bool three_d,bool clear,bool draw,
    struct tg_gpu_execution *state,struct tg_gpu_status *s)
{
    enum tg_gpu_execute_result result=boot(io,power,cpu,physical,firmware,work,
                                           notify,three_d,clear,draw,state,s);
    if(result!=TG_GPU_EXEC_OK)return result;
    volatile uint32_t *fence=(volatile uint32_t *)((uint8_t *)cpu+FENCE);
    uint32_t i,gpu_first,gpu_second=0xdeadbeef;
    if(draw) {
        /* Exercise the shared BAR1 remap with a different owned USERD page.
         * This final work page is outside the shader/pixel/context buffers.
         */
        state->stage=16;
        result=tg_gpu_boot_map_userd(io,power,cpu,physical,
            work->physical+TG_GPU_GR_CHANNEL_WORK_BYTES-4096,state,s);
        if(result!=TG_GPU_EXEC_OK)return result;
    }
    state->stage = 3;
    if(draw) {
        enum tg_gpu_channel_result r=tg_gpu_channel_bind(io,power,physical,physical+INST,
                                                       physical+RUNLIST,state,&state->channel);
        state->channel_bound=state->channel.referenced;
        s->channel_instance_after=ex_read(io,0x800000);
        if(r!=TG_GPU_CHANNEL_OK)return channel_result(r);
    } else {
        state->channel_bound=true;
        ex_write(io, 0x800000, (uint32_t)((physical + INST) >> 12) | 0x80000000);
        s->channel_instance_after = ex_read(io, 0x800000);
        if (s->channel_instance_after != ((uint32_t)((physical + INST) >> 12) | 0x80000000))
            return TG_GPU_EXEC_FIFO_INIT;
        ex_write(io, 0x800004, (ex_read(io, 0x800004) & ~0x400u) | 0x400);
        ex_write(io, 0x2270, (uint32_t)((physical + RUNLIST) >> 12));
        ex_write(io, 0x2274, TG_GPU_RUNLIST_ENTRIES); /* TSG zero + its one channel */
        if (!ex_wait(io, 0x2284, 0x100000, 0)) return TG_GPU_EXEC_RUNLIST;
        ex_write(io, 0x2630, ex_read(io, 0x2630) & ~1u);
    }
    s->scheduler_after = ex_read(io, 0x2630);
    if (s->scheduler_after == UINT32_MAX || (s->scheduler_after & 1))
        return TG_GPU_EXEC_SCHEDULER;

    state->stage = 4;
    /* Both notifications use the vendor's BAR1 path after the runlist is
     * runnable. A CPU store to saved RAMFC/USERD is not a replacement.
     */
    if (ex_fault(io)) { tg_gpu_execute_snapshot(io, state, s); return TG_GPU_EXEC_FAULT; }
    if(three_d && gr_fault(io,s)) return TG_GPU_EXEC_GR_ENGINE;
    if(draw) {
        enum tg_gpu_channel_result r=tg_gpu_channel_publish(io,power,USERD_VA,1,state,&state->channel);
        if(r!=TG_GPU_CHANNEL_OK)return channel_result(r);
    } else io->write32(io->context, BAR1 + USERD_VA + 35 * 4, 1);
    gpu_first = io->read32(io->context, BAR1 + FENCE_VA);
    for (i = 0; i < 10000 && gpu_first != s->expected[0]; ++i) {
        if (ex_fault(io) || (three_d && gr_fault(io,s))) break;
        io->delay_us(io->context, 10);
        gpu_first = io->read32(io->context, BAR1 + FENCE_VA);
    }
    s->observed[0] = fence[0];
    if (gpu_first == s->expected[0]) {
        if(notify && !notify->wait(notify->context,1)) return TG_GPU_EXEC_IRQ;
        state->stage = 5;
        if(draw) {
            enum tg_gpu_channel_result r=tg_gpu_channel_publish(io,power,USERD_VA,2,state,&state->channel);
            if(r!=TG_GPU_CHANNEL_OK)return channel_result(r);
        } else io->write32(io->context, BAR1 + USERD_VA + 35 * 4, 2);
        gpu_second = io->read32(io->context, BAR1 + FENCE_VA + 4);
        for (i = 0; i < 10000 && gpu_second != s->expected[1]; ++i) {
            if (ex_fault(io) || (three_d && gr_fault(io,s))) break;
            io->delay_us(io->context, 10);
            gpu_second = io->read32(io->context, BAR1 + FENCE_VA + 4);
        }
        if(gpu_second==s->expected[1] && notify && !notify->wait(notify->context,2))
            return TG_GPU_EXEC_IRQ;
    }
    /* Observe the GPU alias while polling, but require matching CPU/DRAM
     * readback after real L2 writeback before reporting a completed test.
     * Neither alias is ever assigned the expected value by the probe.
     */
    if (gpu_first == s->expected[0] && gpu_second == s->expected[1] &&
        tg_gpu_frontend_sync_memory(io, power, &s->frontend) != TG_GPU_FE_OK)
        return TG_GPU_EXEC_FRONTEND;
    s->observed[0] = fence[0];
    s->observed[1] = fence[1];
    if(three_d && gpu_first==s->expected[0] && gpu_second==s->expected[1]) {
        const volatile uint32_t *image=work->cpu;
        s->gr_channel.save_ops=image[0xf4/4];
        s->gr_channel.restore_ops=image[0xf8/4];
    }
    tg_gpu_execute_snapshot(io, state, s);
    if (s->frontend.result != TG_GPU_FE_OK) return TG_GPU_EXEC_FRONTEND;
    if(three_d && gr_fault(io,s))
        return TG_GPU_EXEC_GR_ENGINE;
    const struct tg_gpu_fault_status faults = {
        s->fifo_interrupt, s->mmu_fault, s->pbdma_interrupt, s->pbdma_interrupt_1
    };
    if (tg_gpu_fault_pending(&faults))
        return TG_GPU_EXEC_FAULT;
    if (s->observed[0] != s->expected[0] || s->observed[1] != s->expected[1])
        return TG_GPU_EXEC_TIMEOUT;
    if(clear && !tg_gpu_check_clear_pixels(
        (volatile uint32_t *)((uint8_t *)work->cpu+TG_GPU_CLEAR_OFFSET),TG_GPU_CLEAR_BYTES,&s->clear))
        return TG_GPU_EXEC_PIXELS;
    if(draw) {
        if(!tg_gpu_check_draw_code((volatile uint32_t *)((uint8_t *)work->cpu+TG_GPU_DRAW_OFFSET),
                                  TG_GPU_DRAW_BYTES))return TG_GPU_EXEC_PIXELS;
        s->draw.checked=1;
        state->stage=15;
        enum tg_gpu_channel_result r=tg_gpu_channel_retire(io,power,state,&state->channel);
        if(r!=TG_GPU_CHANNEL_OK)return channel_result(r);
        state->channel_bound=false;
        tg_gpu_gr_snapshot(io,&s->gr);
        s->gr_channel.current_context=s->gr.current_context;
    }
    state->stage = 6;
    return TG_GPU_EXEC_OK;
}

enum tg_gpu_execute_result tg_gpu_execute(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, void *cpu, uint64_t physical,
    const struct tg_gpu_gr_payload *firmware, struct tg_gpu_execution *state,
    struct tg_gpu_status *s)
{ return execute(io,power,cpu,physical,firmware,NULL,NULL,false,false,false,state,s); }

enum tg_gpu_execute_result tg_gpu_execute_gr(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, void *cpu, uint64_t physical,
    const struct tg_gpu_gr_payload *firmware, const struct tg_gpu_gr_work *work,
    struct tg_gpu_execution *state, struct tg_gpu_status *s)
{
    if(!work) return TG_GPU_EXEC_ARGUMENT;
    return execute(io,power,cpu,physical,firmware,work,NULL,false,false,false,state,s);
}

enum tg_gpu_execute_result tg_gpu_execute_irq(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,void *cpu,uint64_t physical,
    const struct tg_gpu_gr_payload *firmware,const struct tg_gpu_execution_notify *notify,
    struct tg_gpu_execution *state,struct tg_gpu_status *s)
{
    if(!notify) return TG_GPU_EXEC_ARGUMENT;
    return execute(io,power,cpu,physical,firmware,NULL,notify,false,false,false,state,s);
}
enum tg_gpu_execute_result tg_gpu_execute_3d(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,void *cpu,uint64_t physical,
    const struct tg_gpu_gr_payload *firmware,const struct tg_gpu_gr_work *work,
    struct tg_gpu_execution *state,struct tg_gpu_status *s)
{
    if(!work || !firmware) return TG_GPU_EXEC_ARGUMENT;
    return execute(io,power,cpu,physical,firmware,work,NULL,true,false,false,state,s);
}
enum tg_gpu_execute_result tg_gpu_execute_clear(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,void *cpu,uint64_t physical,
    const struct tg_gpu_gr_payload *firmware,const struct tg_gpu_gr_work *work,
    struct tg_gpu_execution *state,struct tg_gpu_status *s)
{
    if(!work || !firmware) return TG_GPU_EXEC_ARGUMENT;
    return execute(io,power,cpu,physical,firmware,work,NULL,true,true,false,state,s);
}
enum tg_gpu_execute_result tg_gpu_execute_draw(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,void *cpu,uint64_t physical,
    const struct tg_gpu_gr_payload *firmware,const struct tg_gpu_gr_work *work,
    struct tg_gpu_execution *state,struct tg_gpu_status *s)
{
    if(!work || !firmware)return TG_GPU_EXEC_ARGUMENT;
    return execute(io,power,cpu,physical,firmware,work,NULL,true,true,true,state,s);
}

void tg_gpu_execute_stop(const struct tg_gpu_power_io *io, struct tg_gpu_execution *s)
{
    if (!s->owned) return;
    s->boot_ready=false;
    if (s->fifo_ready) {
        ex_write(io, 0x2630, ex_read(io, 0x2630) | 1);
        ex_write(io, 0x800004, ex_read(io, 0x800004) | 0x800);
        ex_write(io, 0x2274, 0);
        (void)ex_wait(io, 0x2284, 0x100000, 0);
        ex_write(io, 0x800000, 0);
        ex_write(io, 0x1704, 0);
    }
    if (s->pbdma_owned) {
        ex_write(io, 0x204, s->old_pbdma_enable & ~1u);
        s->pbdma_owned = false;
    }
    /* Reset the test FIFO even if runlist removal times out; MC drain and
     * full GPU reset still must succeed before the arena is freed.
     */
    /* Stop FECS/GPCCS DMA too before the caller drains MC or frees the arena.
     * Their boot surface remains mapped until full GPU power retirement.
     * Match Switchroot gk20a_gr_suspend before asserting MC PGRAPH reset:
     * disable GR GPFIFO/semaphore access and mask local GR interrupt/exception
     * enables only while GR is currently out of reset. Full stop/suspend/TDR
     * still relies on MC reset, MC drain, CAR/PMC power-down and cold rebuild.
     */
    uint32_t mc = ex_read(io, 0x200);
    if (s->gr_owned && mc != UINT32_MAX && (mc & 0x18001000u) == 0x18001000u) {
        ex_write(io, 0x400500, 0);
        ex_write(io, 0x40013c, 0);
        ex_write(io, 0x400138, 0);
        ex_write(io, 0x400130, 0);
        ex_write(io, 0x400134, 0);
    }
    uint32_t reset=(s->gr_owned ? 0x18001100u : 0x100u) | (s->acr_owned ? 0x2000u : 0) | s->ce_reset_mask;
    if (mc != UINT32_MAX) ex_write(io, 0x200, mc & ~reset);
    io->delay_us(io->context, 20);
    s->gr_owned = false;
    s->acr_owned = false;
    s->ce_reset_mask = 0;
    s->fifo_ready = false;
    s->channel_bound = false;
}
