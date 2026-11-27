/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 * Adapted from Switchroot nvgpu 1ae0167d: gm20b/pmu_gm20b.c,
 * gk20a/flcn_gk20a.c, common/pmu/pmu.c and common/fb/fb_gm20b.c.
 * See ../NOTICE. Authentication is performed by vendor firmware, not this code.
 */
#include "t214_acr.h"
#define GPU 0x57000000u
#define PMU 0x10a000u
#define PWR 0x2000u
static uint32_t get(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context,GPU+reg); }
static void stream(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t v)
{ io->write32(io->context,GPU+reg,v); }
static void put(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t v)
{ stream(io,reg,v); (void)get(io,reg); }
static bool owned(const struct tg_gpu_power_io *io, const struct tg_gpu_power *p)
{ return io && io->read32 && io->write32 && io->delay_us && p && p->owned && p->ready; }
static bool wait_reg(const struct tg_gpu_power_io *io, struct tg_gpu_acr_hw_status *s,
    uint32_t reg, uint32_t mask, uint32_t value, uint32_t us)
{
    s->last_register=reg;
    for (uint32_t t=0;t<us;t+=10) {
        s->last_value=get(io,reg);
        if (s->last_value==UINT32_MAX) return false;
        if ((s->last_value&mask)==value) return true;
        io->delay_us(io->context,10);
    }
    return false;
}
bool tg_gpu_acr_hw_stop(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *p, struct tg_gpu_acr_hw_status *s)
{
    uint32_t enable;
    if (!owned(io,p) || !s) return false;
    s->reset_confirmed=0;
    enable=get(io,0x200);
    if (enable==UINT32_MAX) return false;
    put(io,PMU+0x014,UINT32_MAX); /* local IRQ mask clear, never CPU IRQ enable */
    put(io,0x200,enable&~PWR);
    io->delay_us(io->context,20);
    s->reset_confirmed=get(io,0x200)==(enable&~PWR);
    return s->reset_confirmed!=0;
}
static enum tg_gpu_acr_hw_result fail(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *p, struct tg_gpu_acr_hw_status *s,
    enum tg_gpu_acr_hw_result result)
{
    s->result=result;
    /* Preserve the original fault and its last register. Full GPU reset/drain
     * remains mandatory even when this local PMU reset is confirmed.
     */
    (void)tg_gpu_acr_hw_stop(io,p,s);
    return result;
}
enum tg_gpu_acr_hw_result tg_gpu_acr_hw_prepare(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *p, struct tg_gpu_acr_hw_status *s)
{
    uint32_t saved, low, high, enable;
    uint64_t end;
    if (!s) return TG_GPU_ACR_HW_ARGUMENT;
    *s=(struct tg_gpu_acr_hw_status){0};
    if (!owned(io,p)) return s->result=TG_GPU_ACR_HW_ARGUMENT;
    s->stage=1; s->security=get(io,0x21434);
    if (s->security==UINT32_MAX || get(io,0x140)!=0 || get(io,0x144)!=0)
        return s->result=TG_GPU_ACR_HW_REGISTER;
    if (!s->security) return s->result=TG_GPU_ACR_HW_SECURITY;
    /* Fetch protection information before consulting WPR. The read/write
     * selector advances on read: it must not use put()'s posted readback.
     */
    if (!wait_reg(io,s,0x100cd0,4,0,100000)) return s->result=TG_GPU_ACR_HW_WPR;
    stream(io,0x100cd0,s->last_value|4);
    if (!wait_reg(io,s,0x100cd0,4,0,100000)) return s->result=TG_GPU_ACR_HW_WPR;
    saved=get(io,0x100cd4);
    if (saved==UINT32_MAX) return s->result=TG_GPU_ACR_HW_REGISTER;
    stream(io,0x100cd4,(saved&~15u)|2); low=get(io,0x100cd4);
    stream(io,0x100cd4,(saved&~15u)|3); high=get(io,0x100cd4);
    stream(io,0x100cd4,saved); /* restore only the selector; address fields are read-only */
    if (low==UINT32_MAX || high==UINT32_MAX) return s->result=TG_GPU_ACR_HW_REGISTER;
    s->wpr_base=(uint64_t)(low>>4)<<12;
    end=(uint64_t)(high>>4)<<12;
    /* Use Switchroot's conservative end-start span, not nouveau's additional
     * final 128 KiB block. Never extend or CPU-map the hardware carveout.
     */
    if (!s->wpr_base || end<=s->wpr_base || end>UINT64_C(0x100000000) ||
        ((s->wpr_base|end)&0x1ffff)) return s->result=TG_GPU_ACR_HW_WPR;
    s->wpr_size=end-s->wpr_base;
    s->stage=2; enable=get(io,0x200);
    if (enable==UINT32_MAX) return s->result=TG_GPU_ACR_HW_REGISTER;
    if ((enable&PWR) && !wait_reg(io,s,PMU+0x04c,0xffff,0,200000))
        return fail(io,p,s,TG_GPU_ACR_HW_IDLE_TIMEOUT);
    if (!tg_gpu_acr_hw_stop(io,p,s)) return s->result=TG_GPU_ACR_HW_RESET;
    put(io,0x200,enable|PWR);
    io->delay_us(io->context,20);
    s->reset_confirmed=0; s->mc_enable=get(io,0x200);
    if (s->mc_enable!=(enable|PWR)) return fail(io,p,s,TG_GPU_ACR_HW_RESET);
    /* No PMU governor yet: use upstream's PMU SLCG/BLCG disable profiles. */
    put(io,PMU+0x17c,0x3fffe); put(io,PMU+0xa74,0x7ffe);
    put(io,PMU+0xe74,0xf); put(io,PMU+0xa70,0);
    s->stage=3;
    if (!wait_reg(io,s,PMU+0x10c,6,0,1000)) return fail(io,p,s,TG_GPU_ACR_HW_SCRUB_TIMEOUT);
    if (!wait_reg(io,s,PMU+0x04c,0xffff,0,200000)) return fail(io,p,s,TG_GPU_ACR_HW_IDLE_TIMEOUT);
    s->hwcfg=get(io,PMU+0x108);
    s->scp_status=get(io,PMU+0xc08);
    s->imem_size=(s->hwcfg&0x1ff)*256;
    s->dmem_size=((s->hwcfg>>9)&0x1ff)*256;
    if (s->hwcfg==UINT32_MAX || s->scp_status==UINT32_MAX || !s->imem_size || s->dmem_size<76)
        return fail(io,p,s,TG_GPU_ACR_HW_MEMORY);
    put(io,PMU+0x014,UINT32_MAX);
    s->stage=4;
    return TG_GPU_ACR_HW_OK;
}
enum tg_gpu_acr_hw_result tg_gpu_acr_hw_boot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *p, const struct tg_gpu_acr_image *v,
    const uint32_t desc[19], uint64_t instance, struct tg_gpu_acr_hw_status *s)
{
    static const uint32_t aperture[5]={4,0,4,5,6};
    uint32_t bytes, tag, itfen;
    if (!s) return TG_GPU_ACR_HW_ARGUMENT;
    if (!owned(io,p) || !v || !desc || s->result || s->stage!=4)
        return s->result=TG_GPU_ACR_HW_ARGUMENT;
    if (!v->boot.data || !v->boot.size || v->boot.size>UINT32_MAX-255 ||
        !v->wpr_size || v->wpr_size>s->wpr_size || v->boot_dmem ||
        instance<0x80000000 || instance>0xfffff000 || (instance&0xfff) ||
        (instance>=s->wpr_base && instance<s->wpr_base+s->wpr_size) ||
        desc[8]!=1 || !desc[9] || !desc[15] || desc[16]!=v->dmem_size ||
        desc[10]!=v->nonsec_offset || desc[11]!=v->nonsec_size ||
        desc[12]!=v->secure_offset || desc[13]!=v->secure_size)
        return fail(io,p,s,TG_GPU_ACR_HW_ARGUMENT);
    bytes=((uint32_t)v->boot.size+255)&~255u; tag=v->boot_tag;
    if (bytes>s->imem_size || v->dmem_size>s->dmem_size ||
        tag>(UINT32_MAX>>8)-bytes/256)
        return fail(io,p,s,TG_GPU_ACR_HW_MEMORY);
    s->stage=5;
    for (unsigned i=0;i<5;++i) put(io,PMU+0xe00+i*4,aperture[i]);
    put(io,PMU+0x044,0);
    itfen=get(io,PMU+0x048);
    s->last_register=PMU+0x048; s->last_value=itfen;
    if (itfen==UINT32_MAX) return fail(io,p,s,TG_GPU_ACR_HW_REGISTER);
    put(io,PMU+0x048,itfen|1);
    /* PMU's dedicated instance register uses bit 30 VALID and the explicit
     * SYS_NCOH selector from gm20b_bl_bootstrap, unlike CCSR/BAR1 encodings.
     * The private page tables still use the established SoC DRAM PTE policy.
     */
    put(io,PMU+0x480,(uint32_t)(instance>>12)|0x70000000);
    s->last_register=PMU+0x480; s->last_value=get(io,PMU+0x480);
    /* NEW_INSTBLK defines PTR[27:0], TARGET[29:28] and VALID[30]. This Lite
     * additionally reads bit 31 as one. Check every programmed field without
     * treating that unprogrammed bit as an address/target mismatch.
     */
    if ((s->last_value&0x7fffffffu)!=((uint32_t)(instance>>12)|0x70000000))
        return fail(io,p,s,TG_GPU_ACR_HW_REGISTER);
    s->stage=6;
    put(io,PMU+0x1c0,0x01000000);
    for (unsigned i=0;i<19;++i) stream(io,PMU+0x1c4,desc[i]);
    s->last_register=PMU+0x1c0; s->last_value=get(io,PMU+0x1c0);
    if ((s->last_value&0xfffc)!=76) return fail(io,p,s,TG_GPU_ACR_HW_MEMORY);
    put(io,PMU+0x180,(s->imem_size-bytes)|0x01000000);
    for (uint32_t i=0;i<bytes;i+=4) {
        uint32_t word=0;
        if (!(i&255)) stream(io,PMU+0x188,tag++);
        for (unsigned j=0;j<4 && i+j<v->boot.size;++j)
            word|=(uint32_t)v->boot.data[i+j]<<(j*8);
        stream(io,PMU+0x184,word);
    }
    s->stage=7;
    put(io,PMU+0x040,0xdeada5a5); /* success must overwrite this firmware mailbox */
    put(io,PMU+0x10c,0); put(io,PMU+0x104,v->boot_tag<<8);
    put(io,PMU+0x100,2);
    if (!wait_reg(io,s,PMU+0x100,0x10,0x10,10000000))
        return fail(io,p,s,TG_GPU_ACR_HW_HALT_TIMEOUT);
    s->stage=8;
    s->cpuctl=get(io,PMU+0x100); s->sctl=get(io,PMU+0x240);
    s->mailbox0=get(io,PMU+0x040); s->mailbox1=get(io,PMU+0x044);
    if (s->cpuctl==UINT32_MAX || s->sctl==UINT32_MAX || s->mailbox1==UINT32_MAX)
        return fail(io,p,s,TG_GPU_ACR_HW_REGISTER);
    if (s->mailbox0!=0) return fail(io,p,s,TG_GPU_ACR_HW_AUTHENTICATION);
    s->stage=9;
    return TG_GPU_ACR_HW_OK;
}
