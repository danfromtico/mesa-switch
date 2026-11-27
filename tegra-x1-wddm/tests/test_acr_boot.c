/* SPDX-License-Identifier: MIT */
#include "../platform/t214_acr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define GPU 0x57000000u
#define PMU 0x10a000u
enum failure { NONE, VPR_BUSY, BAD_WPR, BUSY, RESET, SCRUB, BAD_HWCFG, BAD_INSTANCE, BAD_TARGET, BAD_VALID,
               DMEM_SHORT, NO_HALT, AUTH_ERROR, STALE_MAILBOX };
struct model {
    enum failure fail;
    uint32_t mc, selector, dmemc, imemc, instance, mailbox, vector;
    uint32_t desc[19], code[128], tags[2], apertures[5];
    unsigned writes, dwords, iwords, ntags, starts, resets;
    uint64_t delayed;
};
static uint32_t read32(void *ctx, uint32_t address)
{
    struct model *m=ctx;
    uint32_t r=address-GPU;
    assert(address>=GPU);
    switch (r) {
    case 0x21434:return 1;
    case 0x100cd0:return m->fail==VPR_BUSY ? 4 : 0;
    case 0x100cd4: {
        unsigned i=m->selector++&15;
        if (i==2) return 0x00ffc002;
        if (i==3) return m->fail==BAD_WPR ? 0x00ffc003 : 0x00ffe003;
        return i;
    }
    case 0x200:return m->mc;
    case PMU+0x04c:return m->fail==BUSY ? 1 : 0;
    case PMU+0x108:return m->fail==BAD_HWCFG ? UINT32_MAX : 32|(32<<9);
    case PMU+0x10c:return m->fail==SCRUB ? 6 : 0;
    case PMU+0x1c0:return m->dmemc;
    case PMU+0x180:return m->imemc;
    case PMU+0x480:return (m->instance|0x80000000u) ^
        (m->fail==BAD_INSTANCE ? 1 : m->fail==BAD_TARGET ? 0x10000000 : m->fail==BAD_VALID ? 0x40000000 : 0);
    case PMU+0x100:return m->starts ? (m->fail==NO_HALT ? 2 : 0x10) : 0;
    case PMU+0x040:
        if (!m->starts || m->fail==STALE_MAILBOX) return m->mailbox;
        return m->fail==AUTH_ERROR ? 42 : 0; /* explicit simulated firmware result */
    case PMU+0x1c4:case PMU+0x184:assert(!"stream port was read");return 0;
    default:return 0;
    }
}
static void write32(void *ctx, uint32_t address, uint32_t value)
{
    struct model *m=ctx;uint32_t r=address-GPU;
    assert(address>=GPU);++m->writes;
    /* No fuse/security-policy writes or memory accesses to WPR are allowed. */
    assert(r!=0x21434 && r!=0x100ce4);
    assert(r!=0x140 && r!=0x144);
    if (r>=PMU+0xe00 && r<PMU+0xe14){m->apertures[(r-PMU-0xe00)/4]=value;return;}
    switch (r) {
    case 0x100cd4:m->selector=value&15;break;
    case 0x200:
        if (m->fail!=RESET) m->mc=value;
        if (!(value&0x2000)) ++m->resets;
        break;
    case PMU+0x1c0:m->dmemc=value;break;
    case PMU+0x1c4:
        assert(m->dwords<19);m->desc[m->dwords++]=value;
        if (m->fail!=DMEM_SHORT) m->dmemc+=4;
        break;
    case PMU+0x180:m->imemc=value;break;
    case PMU+0x184:assert(m->iwords<128);m->code[m->iwords++]=value;break;
    case PMU+0x188:assert(m->ntags<2);m->tags[m->ntags++]=value;break;
    case PMU+0x480:m->instance=value;break;
    case PMU+0x040:m->mailbox=value;break;
    case PMU+0x104:m->vector=value;break;
    case PMU+0x100:
        assert(value==2 && m->dwords==19 && m->iwords==128);
        ++m->starts;break;
    default:break;
    }
}
static void delay(void *ctx,uint32_t us){((struct model*)ctx)->delayed+=us;}
static struct tg_gpu_power_io io_for(struct model *m)
{return (struct tg_gpu_power_io){.context=m,.read32=read32,.write32=write32,.delay_us=delay};}
static struct model fresh(enum failure f)
{return (struct model){.fail=f,.mc=0x18003100};}
static const struct tg_gpu_power power={.owned=true,.ready=true};
static void check_prepare_fail(enum failure f, enum tg_gpu_acr_hw_result expected)
{
    struct model m=fresh(f);struct tg_gpu_power_io io=io_for(&m);
    struct tg_gpu_acr_hw_status s;
    assert(tg_gpu_acr_hw_prepare(&io,&power,&s)==expected);
    assert(!m.starts && !m.dwords && !m.iwords);
    if (f==BUSY || f==SCRUB || f==BAD_HWCFG) assert(s.reset_confirmed && !(m.mc&0x2000));
    assert((m.mc&~0x2000u)==(0x18003100&~0x2000u));
}
int main(void)
{
    uint8_t boot[257];uint32_t desc[19]={0};
    struct tg_gpu_acr_image image={.boot={boot,sizeof(boot)},.boot_tag=0x100,
        .wpr_size=0x12c00,.dmem_size=0xc00,.nonsec_size=0x100,.secure_offset=0x100,.secure_size=0x100};
    for (unsigned i=0;i<sizeof(boot);++i) boot[i]=(uint8_t)i;
    desc[8]=1;desc[9]=0x2000;desc[11]=0x100;desc[12]=0x100;desc[13]=0x100;
    desc[15]=0x2100;desc[16]=0xc00;
    struct model m=fresh(NONE);struct tg_gpu_power_io io=io_for(&m);
    struct tg_gpu_acr_hw_status s;
    struct tg_gpu_power not_owned={0};
    assert(tg_gpu_acr_hw_prepare(&io,&not_owned,&s)==TG_GPU_ACR_HW_ARGUMENT && !m.writes);
    assert(tg_gpu_acr_hw_prepare(&io,&power,&s)==TG_GPU_ACR_HW_OK);
    assert(s.stage==4 && s.wpr_base==0xffc00000 && s.wpr_size==0x200000);
    assert(s.imem_size==8192 && s.dmem_size==8192 && m.selector==0 && !m.starts);
    assert(tg_gpu_acr_hw_boot(&io,&power,&image,desc,0x80000000,&s)==TG_GPU_ACR_HW_OK);
    assert(s.stage==9 && s.mailbox0==0 && m.starts==1);
    assert(m.instance==0x70080000 && m.vector==0x10000);
    assert(!memcmp(desc,m.desc,sizeof(desc)) && m.imemc==0x01001e00);
    assert(m.tags[0]==0x100 && m.tags[1]==0x101);
    assert(m.code[0]==0x03020100 && m.code[63]==0xfffefdfc && m.code[64]==0 && m.code[127]==0);
    const uint32_t aperture[]={4,0,4,5,6};assert(!memcmp(aperture,m.apertures,sizeof(aperture)));
    assert(tg_gpu_acr_hw_stop(&io,&power,&s) && s.reset_confirmed && m.mc==0x18001100);
    check_prepare_fail(VPR_BUSY,TG_GPU_ACR_HW_WPR);
    check_prepare_fail(BAD_WPR,TG_GPU_ACR_HW_WPR);
    check_prepare_fail(BUSY,TG_GPU_ACR_HW_IDLE_TIMEOUT);
    check_prepare_fail(RESET,TG_GPU_ACR_HW_RESET);
    check_prepare_fail(SCRUB,TG_GPU_ACR_HW_SCRUB_TIMEOUT);
    check_prepare_fail(BAD_HWCFG,TG_GPU_ACR_HW_MEMORY);
    const enum failure cases[]={DMEM_SHORT,NO_HALT,AUTH_ERROR,STALE_MAILBOX,BAD_INSTANCE,BAD_TARGET,BAD_VALID};
    const enum tg_gpu_acr_hw_result result[]={TG_GPU_ACR_HW_MEMORY,TG_GPU_ACR_HW_HALT_TIMEOUT,
        TG_GPU_ACR_HW_AUTHENTICATION,TG_GPU_ACR_HW_AUTHENTICATION,TG_GPU_ACR_HW_REGISTER,
        TG_GPU_ACR_HW_REGISTER,TG_GPU_ACR_HW_REGISTER};
    for (unsigned i=0;i<7;++i) {
        m=fresh(NONE);io=io_for(&m);
        assert(tg_gpu_acr_hw_prepare(&io,&power,&s)==TG_GPU_ACR_HW_OK);
        m.fail=cases[i];
        assert(tg_gpu_acr_hw_boot(&io,&power,&image,desc,0x80000000,&s)==result[i]);
        assert(s.reset_confirmed && !(m.mc&0x2000));
        if (cases[i]==DMEM_SHORT) assert(!m.starts && !m.iwords);
        if (i>=4) assert(!m.starts && !m.dwords && s.last_register==PMU+0x480 && s.last_value!=0xf0080000u);
        if (cases[i]==NO_HALT) assert(m.delayed>=10000000 && m.delayed<10001000);
    }
    /* Bad backing and oversized boot must fail before firmware starts. */
    for (unsigned i=0;i<2;++i) {
        m=fresh(NONE);io=io_for(&m);
        assert(tg_gpu_acr_hw_prepare(&io,&power,&s)==TG_GPU_ACR_HW_OK);
        if(i) image.boot.size=8193;
        assert(tg_gpu_acr_hw_boot(&io,&power,&image,desc,i?0x80000000:0xffc00000,&s)!=TG_GPU_ACR_HW_OK);
        assert(!m.dwords && !m.iwords && !m.starts && s.reset_confirmed);
    }
    puts("ACR PMU bootstrap model: ordering, selector, PIO bounds, authentication and timeout cleanup pass");
    return 0;
}
