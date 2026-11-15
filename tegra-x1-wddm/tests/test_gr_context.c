/* SPDX-License-Identifier: MIT */
/* Explicit MMIO/GPU-write substitutes. No physical GPU is exercised here. */
#include "../platform/t214_gr_context.h"
#include "../core/gr_draw.h"
#include "../core/gm20b_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GPU 0x57000000u
struct model {
    uint32_t *regs,*inst,*work;
    unsigned writes,binds,saves,power,reset,flushes;
    bool no_power,no_bind,no_save,bind_fail,save_fail,no_image,bad_magic,pri;
    uint32_t busy_reg;
    unsigned helper_writes;
    uint32_t saved_sm_disp_ctrl;
    bool ignore_helper_write;
    unsigned output_exception_writes;
    uint32_t saved_warp_report_mask;
    bool ignore_output_exception_write;
};
/* gr_pri_gpcs_tpcs_sm_disp_ctrl; bit 3 disables helper-invocation loads. */
#define SM_DISP_CTRL 0x419f78u
/* The supplied 32.7.6 NETB context entry, independently checked by hash. */
#define SM_DISP_CTRL_DEFAULT 0x00010bebu
/* The same NETB enables OUT_OF_RANGE_ADDR; NVK shader objects require it off. */
#define SM_WARP_REPORT_MASK 0x419e44u
#define SM_WARP_REPORT_DEFAULT 0x00d3eff2u
#define SM_OUT_OF_RANGE_ADDR (1u<<14)
static uint32_t read32(void *context,uint32_t address)
{
    struct model *m=context;uint32_t r=address-GPU;
    assert(address>=GPU && r<0x600000 && !(r&3));
    assert(r!=0x404488 && r!=0x40448c && r!=0x400200 && r!=0x400204);
    return m->regs[r/4];
}
static void write32(void *context,uint32_t address,uint32_t value)
{
    struct model *m=context;uint32_t r=address-GPU;
    assert(address>=GPU && r<0x600000 && !(r&3));
    /* No global IRQ, PFIFO, security fuse, SMMU or power-control writes. */
    assert(r>=0x400000 || r==0x70000 || r==0x70010 || r==0x70004);
    ++m->writes;
    if(r==0x70000 || r==0x70010 || r==0x70004) {
        assert(value==1);
        if(r==0x70010) {
            ++m->flushes;
            if(m->saves && !m->no_image && !m->save_fail)
                m->work[(TG_GPU_GR_GOLD_OFFSET+256)/4]=0x12348765; /* GPU substitute */
        }
        m->regs[r/4]=r==m->busy_reg?3:0;return;
    }
    if(r==0x404170) {
        assert(value==0x12 || value==0x10);++m->power;
        m->regs[r/4]=m->no_power?value:value&~0x10u;return;
    }
    if(r==0x409614) {
        assert(value==(m->reset?0x770u:0x70u));++m->reset;
    }
    if(r==SM_DISP_CTRL && !(value&8)) {
        /* Once, on the bound context after its bundle and method streams and
         * before the golden save. Only the helper-load disable may change. */
        assert(m->binds==1 && !m->saves && !m->helper_writes);
        assert(!m->regs[0x400208/4] && m->regs[0x404154/4]==0x800);
        assert((m->regs[r/4]&8) && value==(m->regs[r/4]&~8u));
        ++m->helper_writes;
        if(m->ignore_helper_write)return;
    }
    if(r==SM_WARP_REPORT_MASK && !(value&SM_OUT_OF_RANGE_ADDR)) {
        assert(m->binds==1 && !m->saves && !m->output_exception_writes);
        assert(!m->regs[0x400208/4] && m->regs[0x404154/4]==0x800);
        assert((m->regs[r/4]&SM_OUT_OF_RANGE_ADDR) &&
               value==(m->regs[r/4]&~SM_OUT_OF_RANGE_ADDR));
        ++m->output_exception_writes;
        if(m->ignore_output_exception_write)return;
    }
    if(r==0x409840) {m->regs[0x409800/4]&=~value;return;}
    if(r==0x409504) {
        assert(m->regs[0x409500/4]==0x80080040);
        if(value==3) {
            assert(m->power==2 && m->reset==2 && !m->binds);
            assert(m->inst[132]==(TG_GPU_GR_WORK_VA|4) && m->flushes);
            ++m->binds;
            if(!m->no_bind) m->regs[0x409800/4]|=m->bind_fail?0x30:0x10;
            m->work[0xfc/4]=m->bad_magic?0:0x600dc0de; /* FECS header substitute */
        } else {
            assert(value==9 && m->binds==1 && !m->saves);
            assert(m->inst[132]==((TG_GPU_GR_WORK_VA+TG_GPU_GR_GOLD_OFFSET)|4));
            assert(m->work[(TG_GPU_GR_GOLD_OFFSET+0x1c)/4]==1);
            assert(!m->work[(TG_GPU_GR_GOLD_OFFSET+0x20)/4]);
            assert(m->work[(TG_GPU_GR_GOLD_OFFSET+256)/4]==TG_GPU_GR_GOLD_SENTINEL);
            /* The image FECS saves must already allow helper-lane loads. */
            assert(!(m->regs[SM_DISP_CTRL/4]&8));
            m->saved_sm_disp_ctrl=m->regs[SM_DISP_CTRL/4];
            assert(!(m->regs[SM_WARP_REPORT_MASK/4]&SM_OUT_OF_RANGE_ADDR));
            m->saved_warp_report_mask=m->regs[SM_WARP_REPORT_MASK/4];
            ++m->saves;
            if(!m->no_save) m->regs[0x409800/4]|=m->save_fail?3:1;
            if(m->pri) m->regs[0x120058/4]=0x100;
        }
    }
    m->regs[r/4]=value;
}
static void delay(void *context,uint32_t us)
{(void)context;assert(us==5 || us==10);}
static void reset(struct model *m)
{
    uint32_t *regs=m->regs,*inst=m->inst,*work=m->work;
    memset(regs,0,0x600000);memset(inst,0,4096);memset(work,0,TG_GPU_GR_WORK_BYTES);
    *m=(struct model){.regs=regs,.inst=inst,.work=work};
    regs[0x120078/4]=1;regs[0x22430/4]=1;regs[0x22434/4]=2;
    regs[0x502608/4]=0x40002;regs[0x500c30/4]=3;
    regs[0x409800/4]=0x31; /* An old success/failure reply must be cleared. */
    regs[SM_DISP_CTRL/4]=SM_DISP_CTRL_DEFAULT;
    regs[SM_WARP_REPORT_MASK/4]=SM_WARP_REPORT_DEFAULT;
    for(unsigned i=0;i<TG_GPU_GR_CTX_CAPACITY/4;++i)
        work[TG_GPU_GR_GOLD_OFFSET/4+i]=TG_GPU_GR_GOLD_SENTINEL;
}
static enum tg_gpu_gr_context_result run(struct model *m,struct tg_gpu_gr_netlist *net,
    struct tg_gpu_gr_context_status *s)
{
    struct tg_gpu_power_io io={m,read32,write32,NULL,NULL,delay, NULL};
    struct tg_gpu_power power={.owned=true,.ready=true};
    struct tg_gpu_frontend_status fe={.stage=11,.ltc_count=2};
    struct tg_gpu_gr_status gr={.stage=15,.context_bytes=0x13800};
    struct tg_gpu_gr_work work={m->work,0x90000000,TG_GPU_GR_WORK_BYTES};
    return tg_gpu_gr_context_init(&io,&power,&fe,net,&gr,m->inst,0x80040000,&work,s);
}
int main(int argc,char **argv)
{
    struct model m={.regs=calloc(1,0x600000),.inst=calloc(1,4096),
        .work=aligned_alloc(4096,TG_GPU_GR_CHANNEL_WORK_BYTES)};
    struct tg_gpu_gr_context_status s;
    uint32_t ctx[]={0x400080,0,0x1234},bundle[]={0xe100,0xabc},method[]={0x100,0x55};
    struct tg_gpu_gr_netlist net={0};
    assert(m.regs && m.inst && m.work);
    net.region[TG_GPU_GR_CONTEXT]=(struct tg_gpu_gr_blob){(const uint8_t *)ctx,sizeof(ctx)};
    net.region[TG_GPU_GR_BUNDLE]=(struct tg_gpu_gr_blob){(const uint8_t *)bundle,sizeof(bundle)};
    net.region[TG_GPU_GR_METHOD]=(struct tg_gpu_gr_blob){(const uint8_t *)method,sizeof(method)};
    reset(&m);assert(run(&m,&net,&s)==TG_GPU_GR_CTX_OK && s.stage==9);
    assert(s.gpcs==1 && s.tpcs==2 && s.zculls==4);
    assert(m.regs[0x500910/4]==0x40002 && m.regs[0x500914/4]==0x102);
    assert(s.image_magic==0x600dc0de && s.image_changed_words==1 && m.binds==1 && m.saves==1);
    assert(m.inst[132]==(TG_GPU_GR_WORK_VA|4) && !m.regs[0x409b00/4]);
    /* Independent vendor values: CB backing limits/valid selectors, state
     * limit, TPC routing and original INDEX/VALUE interpretation. */
    assert(m.regs[0x419848/4]==0x10000150 && m.regs[0x419c2c/4]==0x10000150);
    assert(m.regs[0x4064c8/4]==0x00c001c0 && m.regs[0x5030f8/4]==0xc00);
    assert(m.regs[0x405b60/4]==0x100 && m.regs[0x405ba0/4]==0x100);
    assert(m.regs[0x400080/4]==0x1234 && m.regs[0x404488/4]==0x80000100);
    assert(!m.regs[0x400208/4] && m.regs[0x404154/4]==0x800);
    /* Every other bit of the vendor default survives in the saved state. */
    assert(m.helper_writes==1 && m.regs[SM_DISP_CTRL/4]==(SM_DISP_CTRL_DEFAULT&~8u));
    assert(m.saved_sm_disp_ctrl==(SM_DISP_CTRL_DEFAULT&~8u));
    assert(m.output_exception_writes==1 &&
           m.saved_warp_report_mask==(SM_WARP_REPORT_DEFAULT&~SM_OUT_OF_RANGE_ADDR));
    {
        struct tg_gpu_power_io io={&m,read32,write32,NULL,NULL,delay, NULL};
        struct tg_gpu_power power={.owned=true,.ready=true};
        struct tg_gpu_frontend_status fe={.stage=11,.ltc_count=2};
        struct tg_gpu_gr_work work={m.work,0x90000000,TG_GPU_GR_CHANNEL_WORK_BYTES};
        void *gold=malloc(s.context_bytes);assert(gold);
        memcpy(gold,(uint8_t *)m.work+TG_GPU_GR_GOLD_OFFSET,s.context_bytes);
        assert(tg_gpu_gr_context_load_image(&io,&power,&fe,m.inst,&work,&s,42)==TG_GPU_GR_CTX_OK);
        assert(!memcmp(gold,(uint8_t *)m.work+TG_GPU_GR_GOLD_OFFSET,s.context_bytes));
        assert(m.work[0xfc/4]==0x600dc0de && m.work[0xf0/4]==42);
        assert(!m.work[0xf4/4]&&!m.work[0xf8/4]&&m.work[0x10/4]==21);
        assert(m.work[0x14/4]==0x280000&&!m.work[0x18/4]);
        assert(m.work[0xa0/4]==2&&m.work[0xa4/4]==0x200000&&!m.work[0xa8/4]);
        assert(!memcmp((uint8_t *)m.work+256,(uint8_t *)gold+256,s.context_bytes-256));
        uint32_t *patch=(uint32_t *)((uint8_t *)m.work+TG_GPU_GR_PATCH_OFFSET);
        assert(patch[0]==0x40800c&&patch[1]==0x1420);
        assert(m.binds==1&&m.saves==1); /* no second FECS bind or fabricated GPU write */
        unsigned writes=m.writes;
        m.regs[0x409b00/4]=0x80080040;
        assert(tg_gpu_gr_context_load_image(&io,&power,&fe,m.inst,&work,&s,43)==TG_GPU_GR_CTX_ARGUMENT);
        assert(m.writes==writes&&m.work[0xf0/4]==42);
        m.regs[0x409b00/4]=0;work.bytes=TG_GPU_GR_WORK_BYTES;
        assert(tg_gpu_gr_context_load_image(&io,&power,&fe,m.inst,&work,&s,43)==TG_GPU_GR_CTX_ARGUMENT);
        assert(m.writes==writes);
        work.bytes=TG_GPU_GR_CHANNEL_WORK_BYTES;m.busy_reg=0x70010;
        assert(tg_gpu_gr_context_load_image(&io,&power,&fe,m.inst,&work,&s,43)==TG_GPU_GR_CTX_VISIBILITY);
        free(gold);
    }
    {
        struct tg_gpu_gr_patch patches[TG_GPU_GR_GLOBAL_PATCH_COUNT],before[TG_GPU_GR_GLOBAL_PATCH_COUNT];
        const struct tg_gpu_gr_global_addresses high={0x123400000,0x123410000,0x123420000};
        assert(tg_gpu_gr_context_global_patches(&high,patches,TG_GPU_GR_GLOBAL_PATCH_COUNT));
        assert(patches[0].address==0x40800c&&patches[0].value==0x1234100);
        assert(patches[6].address==0x408004&&patches[6].value==0x1234000);
        assert(patches[11].address==0x418810&&patches[11].value==0x80123420);
        assert(patches[12].value==0x10123420&&patches[13].value==0x10123420);
        memcpy(before,patches,sizeof(before));
        struct tg_gpu_gr_global_addresses bad=high;bad.attribute=TG_GPU_VA_LIMIT-0x1000;
        assert(!tg_gpu_gr_context_global_patches(&bad,patches,TG_GPU_GR_GLOBAL_PATCH_COUNT));
        assert(!memcmp(before,patches,sizeof(before)));
        bad=high;bad.bundle++;
        assert(!tg_gpu_gr_context_global_patches(&bad,patches,TG_GPU_GR_GLOBAL_PATCH_COUNT));
        assert(!memcmp(before,patches,sizeof(before)));
    }
    for(unsigned test=0;test<9;++test) {
        reset(&m);
        if(test==0)m.no_power=true;if(test==1)m.no_bind=true;
        if(test==2)m.bind_fail=true;if(test==3)m.no_save=true;
        if(test==4)m.save_fail=true;if(test==5)m.no_image=true;
        if(test==6)m.bad_magic=true;if(test==7)m.busy_reg=0x70010;
        if(test==8)m.pri=true;
        const unsigned expected[]={TG_GPU_GR_CTX_TIMEOUT,TG_GPU_GR_CTX_TIMEOUT,
            TG_GPU_GR_CTX_FECS_REJECTED,TG_GPU_GR_CTX_TIMEOUT,TG_GPU_GR_CTX_FECS_REJECTED,
            TG_GPU_GR_CTX_IMAGE,TG_GPU_GR_CTX_IMAGE,TG_GPU_GR_CTX_VISIBILITY,TG_GPU_GR_CTX_VISIBILITY};
        assert(run(&m,&net,&s)==expected[test] && s.stage<9);
        if(test<3 || test==7)assert(!m.saves);
        assert(!m.regs[0x400208/4] && m.regs[0x404154/4]==0x800);
    }
    {
        /* Already enabled: no write. A failed PRI access is never written
         * back as register data and no golden image is saved from it. */
        reset(&m);m.regs[SM_DISP_CTRL/4]=SM_DISP_CTRL_DEFAULT&~8u;
        assert(run(&m,&net,&s)==TG_GPU_GR_CTX_OK && s.stage==9 && !m.helper_writes);
        assert(m.regs[SM_DISP_CTRL/4]==(SM_DISP_CTRL_DEFAULT&~8u) && m.saves==1);
        reset(&m);m.ignore_helper_write=true;
        assert(run(&m,&net,&s)==TG_GPU_GR_CTX_REGISTER && s.stage==5);
        assert(m.helper_writes==1 && !m.saves && s.last_register==SM_DISP_CTRL &&
               s.last_value==SM_DISP_CTRL_DEFAULT);
        const uint32_t unreadable[]={UINT32_MAX,0xbadf1100,0xbadf5040,0xbad00100};
        for(unsigned i=0;i<sizeof(unreadable)/sizeof(unreadable[0]);++i) {
            reset(&m);m.regs[SM_DISP_CTRL/4]=unreadable[i];
            assert(run(&m,&net,&s)==TG_GPU_GR_CTX_REGISTER && s.stage==5);
            assert(s.last_register==SM_DISP_CTRL && s.last_value==unreadable[i]);
            assert(!m.helper_writes && !m.saves && m.regs[SM_DISP_CTRL/4]==unreadable[i]);
            assert(!m.regs[0x400208/4] && m.regs[0x404154/4]==0x800);
        }
    }
    {
        reset(&m);m.regs[SM_WARP_REPORT_MASK/4]=SM_WARP_REPORT_DEFAULT&~SM_OUT_OF_RANGE_ADDR;
        assert(run(&m,&net,&s)==TG_GPU_GR_CTX_OK && !m.output_exception_writes && m.saves==1);
        reset(&m);m.ignore_output_exception_write=true;
        assert(run(&m,&net,&s)==TG_GPU_GR_CTX_REGISTER && s.stage==5);
        assert(m.output_exception_writes==1 && !m.saves && s.last_register==SM_WARP_REPORT_MASK);
        const uint32_t unreadable[]={UINT32_MAX,0xbadf1100,0xbadf5040,0xbad00100};
        for(unsigned i=0;i<sizeof(unreadable)/sizeof(unreadable[0]);++i) {
            reset(&m);m.regs[SM_WARP_REPORT_MASK/4]=unreadable[i];
            assert(run(&m,&net,&s)==TG_GPU_GR_CTX_REGISTER && s.stage==5);
            assert(s.last_register==SM_WARP_REPORT_MASK && s.last_value==unreadable[i]);
            assert(!m.output_exception_writes && !m.saves);
        }
    }
    {
        const uint32_t unsupported[]={0x20001,0x20002,0x40001,2,0x30002,0x50002,UINT32_MAX};
        for(unsigned i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
            reset(&m);m.regs[0x502608/4]=unsupported[i];
            assert(run(&m,&net,&s)==TG_GPU_GR_CTX_TOPOLOGY && !m.writes);
        }
    }
    reset(&m);ctx[0]=0x70000;
    assert(run(&m,&net,&s)==TG_GPU_GR_CTX_FORMAT && !m.writes);ctx[0]=0x400080;
    {
        void *arena=aligned_alloc(4096,0x80000);assert(arena);memset(arena,0,0x80000);
        struct tg_gpu_gr_work work={m.work,0x90000000,TG_GPU_GR_WORK_BYTES};
        uint64_t *pt=(uint64_t *)((uint8_t *)arena+0x20000),pte;
        assert(tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work));
        assert(tg_gpu_pte_4k(0x90000000,false,true,&pte) && pt[TG_GPU_GR_WORK_VA>>12]==pte);
        assert(m.work[TG_GPU_GR_GOLD_OFFSET/4]==TG_GPU_GR_GOLD_SENTINEL && !m.work[0]);
        assert(!tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work));
        memset(arena,0,0x80000);work.physical=0x80040000;
        assert(!tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work) && !pt[TG_GPU_GR_WORK_VA>>12]);
        work.physical=0xfffff000;
        assert(!tg_gpu_gr_context_prepare(arena,0x80000000,0x80000,&work));
        work.physical=0x90000000;
        assert(!tg_gpu_gr_context_prepare_channel(arena,0x80000000,0x80000,&work));
        work.bytes=TG_GPU_GR_CHANNEL_WORK_BYTES;
        assert(tg_gpu_gr_context_prepare_channel(arena,0x80000000,0x80000,&work));
        assert(tg_gpu_pte_4k(0x90100000,true,true,&pte));
        assert(pt[(TG_GPU_GR_WORK_VA+TG_GPU_GR_ACCESS_MAP_OFFSET)>>12]==pte);
        assert(tg_gpu_pte_4k(0x90180000,true,true,&pte));
        assert(pt[(TG_GPU_GR_WORK_VA+TG_GPU_GR_PATCH_OFFSET)>>12]==pte);
        assert(tg_gpu_pte_4k(0x901a0000,true,true,&pte));
        assert(pt[(TG_GPU_GR_WORK_VA+TG_GPU_DRAW_OFFSET)>>12]==pte);
        assert(tg_gpu_pte_4k(0x901a1000,true,true,&pte));
        assert(pt[(TG_GPU_GR_WORK_VA+TG_GPU_DRAW_OFFSET+4096)>>12]==pte);
        assert(tg_gpu_pte_4k(0x90000000,false,true,&pte)&&pt[TG_GPU_GR_WORK_VA>>12]==pte);
        assert(!pt[(TG_GPU_GR_WORK_VA+TG_GPU_GR_CHANNEL_WORK_BYTES)>>12]);
        free(arena);
    }
    if(argc==2) {
        FILE *f=fopen(argv[1],"rb");assert(f && !fseek(f,0,SEEK_END));long bytes=ftell(f);
        assert(bytes>0 && !fseek(f,0,SEEK_SET));uint8_t *data=malloc((size_t)bytes);assert(data);
        assert(fread(data,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
        assert(tg_gpu_gr_netlist_parse((struct tg_gpu_gr_blob){data,(size_t)bytes},&net)==TG_GPU_GR_FW_OK);
        reset(&m);assert(run(&m,&net,&s)==TG_GPU_GR_CTX_OK);free(data);
        puts("External NETB graphics initialization streams: PASS (model only)");
    }
    free(m.regs);free(m.inst);free(m.work);
    puts("PASS: GR golden-save order, helper loads, shader-output exception policy, mappings and failure rejection (model only)");
    return 0;
}
