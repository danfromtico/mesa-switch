/* SPDX-License-Identifier: MIT */
#include "../core/execute.h"
#include "../core/gm20b_mmu.h"
#include <assert.h>
#include <string.h>

static _Alignas(4096) uint8_t arena[TG_GPU_ARENA_BYTES];
struct model { uint32_t base,instance,sched;unsigned writes,tlbs,syncs;bool fail_tlb,bad_alias; };
static uint32_t read32(void *context,uint32_t address)
{
    struct model *m=context;
    switch(address) {
    case 0x57800000:return m->instance;
    case 0x57002630:return m->sched;
    case 0x57001704:return 0x800c0040;
    case 0x57002254:return m->base;
    case 0x57100c80:return m->fail_tlb?0x10000:0x18000;
    case 0x58013088:case 0x5801308c:
        assert(m->tlbs && m->base==0x10000013);
        return m->bad_alias?UINT32_MAX:0;
    case 0x57070000:case 0x57070010:case 0x57070004:
    case 0x57001100:case 0x57120058:case 0x5712005c:return 0;
    default:assert(!"unexpected BAR1 model read");return UINT32_MAX;
    }
}
static void write32(void *context,uint32_t address,uint32_t value)
{
    struct model *m=context;uint64_t pte;++m->writes;
    switch(address) {
    case 0x57002254:
        assert(!m->instance && m->sched==1);
        if(value)assert(m->tlbs && value==0x10000013);
        m->base=value;break;
    case 0x57070000:case 0x57070010:case 0x57070004:assert(value==1);++m->syncs;break;
    case 0x57100cb8:assert(value==0xc00000);break;
    case 0x57100cbc:
        assert(value==0x80000001 && !m->base && m->syncs);
        assert(tg_gpu_pte_4k(0xd0002000,false,true,&pte));
        assert(((uint64_t *)(arena+0x20000))[19]==pte);
        ++m->tlbs;break;
    default:assert(!"unexpected BAR1 model write");
    }
}
static void delay(void *context,uint32_t us) { (void)context;assert(us==2 || us==5); }
void test_boot_bar1(void)
{
    struct model m={.sched=1,.base=0x10000013};
    struct tg_gpu_power power={.owned=true,.ready=true};
    struct tg_gpu_execution state={.owned=true,.boot_ready=true};
    struct tg_gpu_status status={.frontend={.stage=11}};
    struct tg_gpu_power_io io={.context=&m,.read32=read32,.write32=write32,.delay_us=delay};
    uint32_t expected[2]={0x12345678,0x87654321};
    assert(tg_gpu_prepare_arena(arena,0xc0000000,sizeof(arena),expected));
    uint64_t original=((uint64_t *)(arena+0x20000))[19];
    assert(tg_gpu_boot_map_userd(&io,&power,arena,0xc0000000,0xc0041000,&state,&status)==TG_GPU_EXEC_ARGUMENT);
    assert(!m.writes);
    m.instance=0x800c0040;
    assert(tg_gpu_boot_map_userd(&io,&power,arena,0xc0000000,0xd0002000,&state,&status)==TG_GPU_EXEC_BUSY);
    assert(!m.writes && ((uint64_t *)(arena+0x20000))[19]==original);m.instance=0;
    assert(tg_gpu_boot_map_userd(&io,&power,arena,0xc0000000,0xd0002000,&state,&status)==TG_GPU_EXEC_OK);
    assert(status.bar1_phase==7 && !status.userd_gp_get && !status.userd_gp_put && m.tlbs==1);
    assert(((uint32_t *)(arena+0x40000))[2]==0xd0002000);
    assert(((uint32_t *)(arena+0x44000))[0]==0xdeadbeef && ((uint32_t *)(arena+0x44000))[1]==0xdeadbeef);
    m.tlbs=m.writes=m.syncs=0;m.fail_tlb=true;
    assert(tg_gpu_boot_map_userd(&io,&power,arena,0xc0000000,0xd0002000,&state,&status)==TG_GPU_EXEC_MMU_TIMEOUT);
    assert(!m.base && m.tlbs==1 && status.bar1_phase==6);
    m.fail_tlb=false;m.bad_alias=true;
    assert(tg_gpu_boot_map_userd(&io,&power,arena,0xc0000000,0xd0002000,&state,&status)==TG_GPU_EXEC_BAR1);
    assert(state.owned && state.boot_ready && !state.channel_bound);
}
