/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018, NVIDIA CORPORATION. All rights reserved.
 * GM20B RTOS ABI and ordering: Switchroot nvgpu 1ae0167d common/pmu,
 * gm20b/pmu_gm20b.c and Linux nouveau v6.12 subdev/pmu/gm20b.c. See ../NOTICE.
 */
#include "t214_pmu.h"
#define GPU 0x57000000u
#define PMU 0x10a000u
#define MSG_HEAD 0x4c8u
#define MSG_TAIL 0x4ccu
#define POLLS 2000u
static uint32_t get(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context,GPU+PMU+reg); }
static void put(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t value)
{
    io->write32(io->context,GPU+PMU+reg,value);
    (void)get(io,reg);
}
static uint32_t le16(const uint8_t *b) { return b[0]|(uint32_t)b[1]<<8; }
static uint32_t le32(const uint8_t *b)
{ return le16(b)|le16(b+2)<<16; }
static enum tg_gpu_pmu_result finish(const struct tg_gpu_power_io *io,
    struct tg_gpu_pmu_status *s, enum tg_gpu_pmu_result result)
{
    s->cpuctl=get(io,0x100); s->irqstat=get(io,8);
    s->msg_head=get(io,MSG_HEAD); s->msg_tail=get(io,MSG_TAIL);
    s->result=result; return result;
}
static bool transfer(const struct tg_gpu_power_io *io, uint32_t limit,
    uint32_t offset, uint8_t *bytes, uint32_t count, bool write)
{
    uint32_t rounded=(count+3)&~3u;
    if (!count || (offset&3) || offset>limit || rounded>limit-offset) return false;
    put(io,0x1c0,offset|(write?0x01000000u:0x02000000u));
    for (uint32_t i=0;i<rounded;i+=4) {
        uint32_t value=0;
        if (write) {
            for (uint32_t j=0;j<4 && i+j<count;++j) value|=(uint32_t)bytes[i+j]<<(8*j);
            /* No posted read of the streaming port: it advances DMEM. */
            io->write32(io->context,GPU+PMU+0x1c4,value);
        } else {
            value=get(io,0x1c4);
            for (uint32_t j=0;j<4 && i+j<count;++j) bytes[i+j]=(uint8_t)(value>>(8*j));
        }
    }
    return (get(io,0x1c0)&0xffffu)==offset+rounded;
}
static bool region(uint32_t base, uint32_t size, uint32_t limit)
{ return !(base&3) && !(size&3) && size>=32 && base<limit && size<=limit-base; }
static bool position(uint32_t pos, uint32_t base, uint32_t size)
{ return !(pos&3) && pos>=base && pos<base+size; }

static enum tg_gpu_pmu_result init_message(const struct tg_gpu_power_io *io,
    uint32_t limit, struct tg_gpu_pmu_status *s)
{
    uint8_t msg[42];
    uint32_t head=0,tail=0;
    for (unsigned i=0;i<POLLS;++i) {
        head=get(io,MSG_HEAD); tail=get(io,MSG_TAIL);
        s->last_register=PMU+MSG_HEAD; s->last_value=head;
        if (head==UINT32_MAX || tail==UINT32_MAX) return TG_GPU_PMU_REGISTER;
        if (head!=tail) break;
        io->delay_us(io->context,1000);
    }
    if (head==tail) return TG_GPU_PMU_INIT_TIMEOUT;
    if ((head&3) || (tail&3) || tail>=limit || head>limit || head<tail || head-tail<44)
        return TG_GPU_PMU_INIT_FORMAT;
    if (!transfer(io,limit,tail,msg,sizeof(msg),false)) return TG_GPU_PMU_PIO;
    s->init_header=le32(msg);
    if (msg[0]!=7 || msg[1]!=sizeof(msg) || msg[4]!=0) return TG_GPU_PMU_INIT_FORMAT;
    /* pmu_init_msg_pmu_v1: header(4), prefix(4), five 6-byte queue records.
     * Only HPQ 0 and MSGQ 4 are used. Validate before acknowledging or writing.
     */
    s->hpq_size=le16(msg+8); s->hpq_base=le16(msg+10); s->hpq_index=msg[12];
    s->msg_size=le16(msg+32); s->msg_base=le16(msg+34);
    if (!region(s->hpq_base,s->hpq_size,limit) || s->hpq_index>=4 ||
        !region(s->msg_base,s->msg_size,limit) ||
        (s->hpq_base<s->msg_base+s->msg_size && s->msg_base<s->hpq_base+s->hpq_size) ||
        !position(tail,s->msg_base,s->msg_size) ||
        !position(head,s->msg_base,s->msg_size) || tail+44>s->msg_base+s->msg_size)
        return TG_GPU_PMU_INIT_FORMAT;
    put(io,MSG_TAIL,tail+44);
    s->messages=1;
    return TG_GPU_PMU_OK;
}

static enum tg_gpu_pmu_result post_wpr(const struct tg_gpu_power_io *io,
    uint32_t limit, struct tg_gpu_pmu_status *s)
{
    /* INIT_WPR_REGION, region 1, offset 0, sequence 1, status+interrupt reply. */
    uint8_t command[16]={0x0a,16,3,1, 0,0,0,0, 1,0,0,0, 0,0,0,0};
    uint8_t rewind[4]={0,4,0,0};
    uint32_t reg=0x4a0+s->hpq_index*4;
    uint32_t head=get(io,reg),tail=get(io,0x4b0+s->hpq_index*4);
    uint32_t end=s->hpq_base+s->hpq_size;
    s->last_register=PMU+reg; s->last_value=head;
    if (!position(head,s->hpq_base,s->hpq_size) ||
        !position(tail,s->hpq_base,s->hpq_size)) return TG_GPU_PMU_QUEUE;
    if (head>=tail && end-head<sizeof(command)+4) {
        if (tail<=s->hpq_base+sizeof(command)) return TG_GPU_PMU_QUEUE;
        if (!transfer(io,limit,head,rewind,sizeof(rewind),true)) return TG_GPU_PMU_PIO;
        head=s->hpq_base;
    }
    if (head<tail && tail-head<=sizeof(command)) return TG_GPU_PMU_QUEUE;
    if (!transfer(io,limit,head,command,sizeof(command),true)) return TG_GPU_PMU_PIO;
    /* MMIO operations are ordered by the platform IO implementation. */
    s->cmd_head=head+sizeof(command); put(io,reg,s->cmd_head);
    return TG_GPU_PMU_OK;
}

static enum tg_gpu_pmu_result wpr_reply(const struct tg_gpu_power_io *io,
    uint32_t limit, struct tg_gpu_pmu_status *s)
{
    uint8_t msg[256];
    for (unsigned i=0;i<POLLS;++i) {
        uint32_t head=get(io,MSG_HEAD),tail=get(io,MSG_TAIL),available,bytes;
        s->last_register=PMU+MSG_HEAD; s->last_value=head;
        if (!position(head,s->msg_base,s->msg_size) ||
            !position(tail,s->msg_base,s->msg_size)) return TG_GPU_PMU_QUEUE;
        if (head==tail) { io->delay_us(io->context,1000); continue; }
        available=head>tail?head-tail:s->msg_base+s->msg_size-tail;
        if (!transfer(io,limit,tail,msg,4,false)) return TG_GPU_PMU_PIO;
        s->last_header=le32(msg);
        bytes=(msg[1]+3u)&~3u;
        if (msg[1]<4 || bytes>available || ++s->messages>64) return TG_GPU_PMU_QUEUE;
        if (msg[0]==0) {
            if (msg[1]!=4 || head>=tail || tail==s->msg_base) return TG_GPU_PMU_QUEUE;
            put(io,MSG_TAIL,s->msg_base); continue;
        }
        if (!transfer(io,limit,tail,msg,msg[1],false)) return TG_GPU_PMU_PIO;
        put(io,MSG_TAIL,tail+bytes);
        if (msg[0]==0x0a && msg[3]==1 && !(msg[2]&4)) {
            if (msg[1]!=12 || msg[4]!=0) return TG_GPU_PMU_QUEUE;
            s->reply_code=le32(msg+8);
            return s->reply_code?TG_GPU_PMU_WPR:TG_GPU_PMU_OK;
        }
    }
    return TG_GPU_PMU_REPLY_TIMEOUT;
}

enum tg_gpu_pmu_result tg_gpu_pmu_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, const struct tg_gpu_acr_hw_status *acr,
    struct tg_gpu_pmu_status *s)
{
    uint8_t args[32]={0};
    const uint32_t aperture[5]={4,0,4,5,6};
    enum tg_gpu_pmu_result result;
    uint32_t limit,irq;
    if (!s) return TG_GPU_PMU_ARGUMENT;
    *s=(struct tg_gpu_pmu_status){0};
    if (!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
        !power->owned || !power->ready || !acr || acr->result || acr->stage!=9 ||
        acr->dmem_size<256 || acr->dmem_size>0xffff)
        return s->result=TG_GPU_PMU_ARGUMENT;
    s->stage=1; s->reply_code=UINT32_MAX;
    s->cpuctl=get(io,0x100);
    if (s->cpuctl==UINT32_MAX || (s->cpuctl&0x50)!=0x50 || !acr->security ||
        io->read32(io->context,GPU+0x140) || io->read32(io->context,GPU+0x144))
        return finish(io,s,TG_GPU_PMU_SECURITY);
    put(io,4,0x10); /* Clear HS halt before the LS RTOS can service interrupts. */
    irq=get(io,8);
    if (irq==UINT32_MAX || (irq&0x10)) return finish(io,s,TG_GPU_PMU_HALT_IRQ);
    for (unsigned i=0;i<5;++i) put(io,0xe00+i*4,aperture[i]);
    /* cmdline_args_v3: 204 MHz PWRCLK, secure_mode=true. Trace and GC6 off. */
    for (unsigned i=0;i<4;++i) args[4+i]=(uint8_t)(204000000u>>(i*8));
    args[20]=1; limit=acr->dmem_size-sizeof(args);
    if (!transfer(io,acr->dmem_size,limit,args,sizeof(args),true))
        return finish(io,s,TG_GPU_PMU_PIO);
    s->stage=2; put(io,0x130,2); /* LS mode requires CPUCTL_ALIAS. */
    s->stage=3; result=init_message(io,limit,s);
    if (result) return finish(io,s,result);
    s->stage=4; result=post_wpr(io,limit,s);
    if (result) return finish(io,s,result);
    s->stage=5; result=wpr_reply(io,limit,s);
    if (result) return finish(io,s,result);
    s->stage=6;
    return finish(io,s,TG_GPU_PMU_OK);
}
