/* SPDX-License-Identifier: MIT */
#include "../core/gr_image.h"
#include "../core/gm20b_mmu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t word(const uint8_t *p,unsigned offset)
{ return p[offset]|(uint32_t)p[offset+1]<<8|(uint32_t)p[offset+2]<<16|(uint32_t)p[offset+3]<<24; }
static void store(uint8_t *p,unsigned offset,uint32_t value)
{ for(unsigned i=0;i<4;++i) p[offset+i]=(uint8_t)(value>>(8*i)); }
static void untouched(const uint8_t *p,size_t bytes)
{ for(size_t i=0;i<bytes;++i) assert(p[i]==0xa5); }
int main(void)
{
    uint8_t golden[513],original[513],out[520];
    /* Unaligned source/output exercise the explicit little-endian format. */
    uint8_t *src=golden+1,*dst=out+1;
    memset(golden,0xff,sizeof(golden));store(src,0xfc,0x600dc0de);
    memcpy(original,golden,sizeof(golden));memset(out,0xa5,sizeof(out));
    struct tg_gpu_gr_image_config c={0x123400000,0x1ffe00000,4096,2,17};
    assert(tg_gpu_gr_image_build(dst,512,src,512,&c)==TG_GPU_GR_IMAGE_OK);
    assert(word(dst,0xfc)==0x600dc0de&&word(dst,0xf0)==17);
    assert(!word(dst,0xf4)&&!word(dst,0xf8));
    assert(word(dst,0x10)==2&&word(dst,0x14)==0xffe00000&&word(dst,0x18)==1);
    assert(word(dst,0xa0)==2&&word(dst,0xa4)==0x23400000&&word(dst,0xa8)==1);
    assert(word(dst,0x0c)==0xfffffbff&&word(dst,0x3c)==0xfffffff7);
    assert(word(dst,0x1c)==1&&!word(dst,0x20)&&word(dst,0x28)==0xffffff80&&!word(dst,0x2c));
    assert(!word(dst,0x60)&&word(dst,0x80)==UINT32_MAX);
    assert(!word(dst,0xac)&&!word(dst,0xb0)&&!word(dst,0xb4));
    assert(!memcmp(dst+256,src+256,256)&&!memcmp(golden,original,sizeof(golden)));
    assert(out[0]==0xa5);untouched(out+513,sizeof(out)-513);
    /* An invalid late field must leave the destination unchanged. */
    for(unsigned test=0;test<10;++test) {
        struct tg_gpu_gr_image_config bad=c;memset(out,0xa5,sizeof(out));
        if(test==0)bad.access_map_va=0;
        if(test==1)bad.access_map_va++;
        if(test==2)bad.access_map_va=TG_GPU_VA_LIMIT-4096;
        if(test==3)bad.patch_va=TG_GPU_VA_LIMIT;
        if(test==4)bad.patch_va++;
        if(test==5)bad.patch_count=513;
        if(test==6)bad.patch_va=bad.access_map_va;
        if(test==7)bad.patch_bytes=0;
        if(test==8)bad.patch_va=0;
        if(test==9){bad.patch_count=0;bad.patch_va=0;}
        assert(tg_gpu_gr_image_build(dst,512,src,512,&bad)==TG_GPU_GR_IMAGE_ADDRESS);
        untouched(out,sizeof(out));
    }
    assert(tg_gpu_gr_image_build(dst,511,src,512,&c)==TG_GPU_GR_IMAGE_ARGUMENT);
    assert(tg_gpu_gr_image_build(dst,512,src,252,&c)==TG_GPU_GR_IMAGE_ARGUMENT);
    assert(tg_gpu_gr_image_build(dst,512,src,511,&c)==TG_GPU_GR_IMAGE_ARGUMENT);
    assert(tg_gpu_gr_image_build(src,512,src,512,&c)==TG_GPU_GR_IMAGE_ARGUMENT);
    assert(tg_gpu_gr_image_build(src+4,508,src,256,&c)==TG_GPU_GR_IMAGE_ARGUMENT);
    store(src,0xfc,0);
    assert(tg_gpu_gr_image_build(dst,512,src,512,&c)==TG_GPU_GR_IMAGE_FORMAT);
    untouched(out,sizeof(out));store(src,0xfc,0x600dc0de);
    /* Empty patch data may retain a mapped buffer, as FECS consumes count. */
    c.patch_count=0;
    assert(tg_gpu_gr_image_build(dst,512,src,512,&c)==TG_GPU_GR_IMAGE_OK);
    assert(!word(dst,0x10)&&word(dst,0x14)==0xffe00000);
    c.patch_va=c.patch_bytes=0;
    assert(tg_gpu_gr_image_build(dst,512,src,512,&c)==TG_GPU_GR_IMAGE_OK);
    assert(!word(dst,0x14)&&!word(dst,0x18));
    c.access_map_va=TG_GPU_VA_LIMIT-TG_GPU_GR_ACCESS_MAP_BYTES;
    assert(tg_gpu_gr_image_build(dst,512,src,512,&c)==TG_GPU_GR_IMAGE_OK);
    assert(word(dst,0xa8)==0xff);

    struct tg_gpu_gr_patch p[]={{0x408004,0x12345678},{0x419e10,0x87654321}};
    memset(out,0xa5,sizeof(out));
    assert(tg_gpu_gr_patch_build(dst,16,p,2));
    assert(word(dst,0)==0x408004&&word(dst,4)==0x12345678);
    assert(word(dst,8)==0x419e10&&word(dst,12)==0x87654321);
    untouched(dst+16,sizeof(out)-17);
    for(unsigned test=0;test<5;++test) {
        memset(out,0xa5,sizeof(out));p[1].address=test==0?0x200:test==1?0x600000:0x419e11;
        assert(!tg_gpu_gr_patch_build(dst,test==3?15:512,p,test==4?UINT32_MAX:2));
        untouched(out,sizeof(out));
    }
    uint8_t *map=malloc(TG_GPU_GR_ACCESS_MAP_BYTES+8);assert(map);
    memset(map,0xa5,TG_GPU_GR_ACCESS_MAP_BYTES+8);
    assert(!tg_gpu_gr_access_map_build(map,TG_GPU_GR_ACCESS_MAP_BYTES-1));
    untouched(map,TG_GPU_GR_ACCESS_MAP_BYTES+8);
    assert(tg_gpu_gr_access_map_build(map,TG_GPU_GR_ACCESS_MAP_BYTES));
    unsigned bits=0;
    for(size_t i=0;i<TG_GPU_GR_ACCESS_MAP_BYTES;++i)
        for(unsigned bit=0;bit<8;++bit)bits+=(map[i]>>bit)&1;
    assert(bits==30);
    assert(map[0x404468/32]&(1u<<((0x404468/4)&7)));
    assert(map[0x419f78/32]&(1u<<((0x419f78/4)&7)));
    assert(!map[0x200/32]&&!map[0x409000/32]);
    assert(!(map[0x418e54/32]&(1u<<((0x418e54/4)&7))));
    untouched(map+TG_GPU_GR_ACCESS_MAP_BYTES,8);free(map);
    puts("GM20B golden clone, per-context headers/patches and restricted access map: PASS (CPU format only)");
    return 0;
}
