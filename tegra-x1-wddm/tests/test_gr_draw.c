/* SPDX-License-Identifier: MIT */
/* Packet and upload model only; no physical shader execution is simulated. */
#include "../core/gr_draw.h"
#include "../core/command_stream.h"
#include "../core/gm20b_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    uint32_t p[TG_GPU_DRAW_MAX_WORDS+1],words=0;
    for(unsigned i=0;i<TG_GPU_DRAW_MAX_WORDS+1;++i)p[i]=0xa5a5a5a5;
    assert(tg_gpu_build_draw_packet(p,TG_GPU_DRAW_MAX_WORDS,0x2a0000,0x12004,0x76543210,&words));
    assert(words<TG_GPU_DRAW_MAX_WORDS && p[words]==0xa5a5a5a5);
    assert(tg_gpu_validate_command_stream(p,words*4));
    uint32_t regs[0x3000/4]={0};unsigned begins=0,draws=0,ends=0,reports=0;
    for(unsigned i=0;i<words;) {
        uint32_t h=p[i++],address=(h&0x1fff)*4,op=h>>29;
        unsigned count=op==4?1:(h>>16)&0x1fff;
        assert((op==1 || op==4) && ((h>>13)&7)==0 && count);
        assert(address+4*count<=sizeof(regs) && (op==4 || i+count<=words));
        for(unsigned j=0;j<count;++j) {
            uint32_t reg=address+j*4,v=op==4?(h>>16)&0x1fff:p[i++];regs[reg/4]=v;
            assert(reg!=0x19d0 && reg!=0xfb8 && reg!=0x1528); /* No clear, alias-raster or Horizon software method. */
            if(reg==0x1618){assert(v==4);++begins;}
            if(reg==0xd78){assert(begins==1 && v==3);++draws;}
            if(reg==0x1614){assert(draws==1 && !v);++ends;}
            if(reg==0x1b0c){assert(ends==1 && v==0x1000f010);++reports;}
        }
    }
    assert(begins==1&&draws==1&&ends==1&&reports==1);
    assert(regs[0x1608/4]==0&&regs[0x160c/4]==0x2a0000);
    assert(regs[0x2040/4]==0x11&&regs[0x2140/4]==0x51);
    assert(regs[0x2044/4]==0x30&&regs[0x2144/4]==0x1030);
    assert(regs[0x204c/4]==24&&regs[0x214c/4]==24);
    assert(regs[0x2050/4]==0&&regs[0x2150/4]==4);
    assert(regs[0xa00/4]==0x41000000&&regs[0xa0c/4]==0x41000000);
    assert(regs[0xa18/4]==0x6420);
    assert(regs[0xe00/4]==1&&regs[0xe04/4]==0xc0004&&regs[0xe08/4]==0xc0004);
    assert(regs[0x1a00/4]==0x1111&&regs[0x1970/4]==4);
    assert(regs[0x1b04/4]==0x12004&&regs[0x1b08/4]==0x76543210);
    memset(p,0xa5,sizeof(p));words=0x76543210;
    assert(!tg_gpu_build_draw_packet(p,511,0x2a0000,0x12000,1,&words));
    assert(!tg_gpu_build_draw_packet(p,512,0x2a0004,0x12000,1,&words));
    assert(!tg_gpu_build_draw_packet(p,512,TG_GPU_VA_LIMIT-4096,0x12000,1,&words));
    assert(!tg_gpu_build_draw_packet(p,512,0x2a0000,0x2a1000,1,&words));
    assert(!tg_gpu_build_draw_packet(p,512,0x2a0000,0x12001,1,&words));
    assert(words==0x76543210);
    for(unsigned i=0;i<TG_GPU_DRAW_MAX_WORDS+1;++i)assert(p[i]==0xa5a5a5a5);

    uint32_t *backing=aligned_alloc(4096,16384);assert(backing);
    memset(backing,0xa5,16384);
    uint32_t *code=backing+1024;struct tg_gpu_draw_status s={0};
    assert(tg_gpu_prepare_draw_code(code,8192,&s));
    assert(s.requested==1&&!s.checked&&s.sm==53&&s.vs_bytes==160&&s.fs_bytes==64);
    assert(s.vs_gprs==24&&s.fs_gprs==24&&s.code_hash==0xfb041dd2);
    assert(code[0x30/4]==0x02020461&&code[(4096+0x30)/4]==0x00025462);
    assert(!code[0]&&!code[11]&&!code[1024]&&!code[1035]);
    for(unsigned i=0;i<1024;++i)assert(backing[i]==0xa5a5a5a5&&backing[3072+i]==0xa5a5a5a5);
    assert(tg_gpu_check_draw_code(code,8192));
    code[0x80/4]^=1;assert(!tg_gpu_check_draw_code(code,8192));code[0x80/4]^=1;
    code[(4096+0x30)/4]^=1;assert(!tg_gpu_check_draw_code(code,8192));
    assert(!tg_gpu_prepare_draw_code(code,8191,&s));
    assert(!tg_gpu_prepare_draw_code(code+1,8192,&s));
    free(backing);
    puts("GM20B NAK shader upload, scissored triangle packet and corruption checks: PASS (model only)");
    return 0;
}
