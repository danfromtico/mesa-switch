/* SPDX-License-Identifier: MIT */
/* Packet decoding and explicit CPU pixel substitutes; no physical GPU pass. */
#include "../core/gr_clear.h"
#include "../core/command_stream.h"
#include "../core/gm20b_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct decoded {
    uint32_t regs[0x2000/4];
    unsigned clears,reports;
};
static void parse(struct decoded *d,const uint32_t *p,unsigned words)
{
    unsigned i=0;
    while(i<words) {
        uint32_t header=p[i++],op=header>>29,method=(header&0x1fff)*4;
        assert(((header>>13)&7)==0);
        unsigned count=op==4?1:(header>>16)&0x1fff;
        assert(op==4 || op==1);
        assert(count && method+4*count<=0x2000 && (op==4 || i+count<=words));
        for(unsigned n=0;n<count;++n) {
            uint32_t value=op==4?(header>>16)&0x1fff:p[i++];
            unsigned reg=method+4*n;assert(reg!=0xfb8);d->regs[reg/4]=value;
            if(reg==0x19d0){assert(value==0x3c);++d->clears;}
            if(reg==0x1b0c){assert(value==0x1000f010);++d->reports;}
        }
    }
    assert(i==words);
}
int main(void)
{
    uint32_t commands[TG_GPU_CLEAR_MAX_WORDS+1],words=0;
    for(unsigned i=0;i<TG_GPU_CLEAR_MAX_WORDS+1;++i)commands[i]=0xa5a5a5a5;
    assert(tg_gpu_build_clear_packet(commands,TG_GPU_CLEAR_MAX_WORDS,
        UINT64_C(0x123400000),0x12000,0x12345678,true,&words));
    assert(words<TG_GPU_CLEAR_MAX_WORDS&&commands[words]==0xa5a5a5a5);
    assert(tg_gpu_validate_command_stream(commands,words*4));
    struct decoded d={0};parse(&d,commands,words);
    assert(d.regs[0]==0xb197&&d.clears==1&&d.reports==1);
    assert(d.regs[0x800/4]==1&&d.regs[0x804/4]==0x23400000);
    assert(d.regs[0x808/4]==128&&d.regs[0x80c/4]==16&&d.regs[0x810/4]==0xd5);
    assert(d.regs[0x814/4]==0x1000&&d.regs[0x818/4]==1&&!d.regs[0x81c/4]);
    assert(d.regs[0x10f8/4]==0x10&&d.regs[0x121c/4]==1);
    assert(d.regs[0xd6c/4]==0x100000&&d.regs[0xd70/4]==0x100000);
    assert(!d.regs[0xd80/4]&&!d.regs[0xd84/4]);
    assert(d.regs[0xd88/4]==0x3f800000&&d.regs[0xd8c/4]==0x3f800000);
    assert(d.regs[0x1b04/4]==0x12000&&d.regs[0x1b08/4]==0x12345678);
    assert(tg_gpu_build_clear_packet(commands,TG_GPU_CLEAR_MAX_WORDS,
        0x290000,0x12004,0x87654321,false,&words));
    parse(&d,commands,words);
    assert(d.clears==2&&d.reports==2);
    assert(d.regs[0xd6c/4]==0xc0004&&d.regs[0xd70/4]==0xc0004);
    assert(d.regs[0xd80/4]==0x3f800000&&!d.regs[0xd88/4]);
    assert(d.regs[0x1b08/4]==0x87654321);
    memset(commands,0xa5,sizeof(commands));words=0x76543210;
    assert(!tg_gpu_build_clear_packet(commands,127,0x290000,0x12000,1,true,&words));
    assert(!tg_gpu_build_clear_packet(commands,128,0x290004,0x12000,1,true,&words));
    assert(!tg_gpu_build_clear_packet(commands,128,TG_GPU_VA_LIMIT,0x12000,1,true,&words));
    assert(!tg_gpu_build_clear_packet(commands,128,0x290000,0x290000,1,true,&words));
    assert(!tg_gpu_build_clear_packet(commands,128,0x290000,0x12001,1,true,&words));
    assert(words==0x76543210);
    for(unsigned i=0;i<TG_GPU_CLEAR_MAX_WORDS+1;++i)assert(commands[i]==0xa5a5a5a5);

    uint32_t image[TG_GPU_CLEAR_BYTES/4];struct tg_gpu_clear_status s;
    for(unsigned i=0;i<TG_GPU_CLEAR_BYTES/4;++i)image[i]=TG_GPU_CLEAR_SENTINEL;
    assert(!tg_gpu_check_clear_pixels(image,sizeof(image),&s));
    assert(s.errors==256&&!s.guard_errors);
    /* Explicit GPU-pixel substitute: exact expected logical pixels plus
     * untouched row padding and allocation tail. It is model evidence only.
     */
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        image[y*32+x]=(x>=4&&x<12&&y>=4&&y<12)?0xff0000ff:0xffff0000;
    assert(tg_gpu_check_clear_pixels(image,sizeof(image),&s));
    assert(s.checked==1&&s.pixels==256&&s.background==0xffff0000&&s.foreground==0xff0000ff);
    const uint32_t hash=s.image_hash;image[31]=0;
    assert(!tg_gpu_check_clear_pixels(image,sizeof(image),&s)&&s.guard_errors==1&&!s.errors&&s.image_hash==hash);
    image[31]=TG_GPU_CLEAR_SENTINEL;image[9*32+9]=0xffff0000;
    assert(!tg_gpu_check_clear_pixels(image,sizeof(image),&s)&&s.errors==1&&!s.guard_errors);
    assert(s.background==0xffff0000&&s.foreground==0xff0000ff); /* Two sampled pixels are not enough. */
    image[9*32+9]=0xff0000ff;image[1023]=0;
    assert(!tg_gpu_check_clear_pixels(image,sizeof(image),&s)&&s.guard_errors==1);
    assert(!tg_gpu_check_clear_pixels(image,sizeof(image)-4,&s));
    puts("GM20B pitch clear packets, complete pixel readback and guards: PASS (model only)");
    return 0;
}
