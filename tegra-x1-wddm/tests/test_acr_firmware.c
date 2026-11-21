/* SPDX-License-Identifier: MIT */
#include "../core/acr_firmware.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void w32(uint8_t *p, uint32_t v)
{ for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(i*8)); }
static uint32_t r32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void bin(uint8_t *p, uint32_t n)
{
    w32(p,0x10de); w32(p+4,1); w32(p+8,n);
    w32(p+12,0x100); w32(p+16,0x200); w32(p+20,n-0x200);
}
static const struct tg_gpu_acr_addresses addresses={
    0x30000,0x90000000,0xffc00000,0x100000,0x4000,0x40
};
static void model(void)
{
    uint8_t hs[0x1200]={0},boot[0x500]={0},pmu[0x600]={0},pdesc[152]={0};
    uint8_t fsig[108]={0},psig[76]={0},surface[0x600]={0},out[0x5000];
    struct tg_gpu_gr_payload gr={0};
    struct tg_gpu_acr_files f;
    struct tg_gpu_acr_image v, bad;
    struct tg_gpu_acr_addresses a;
    uint32_t bld[19];
    bin(hs,sizeof(hs)); bin(boot,sizeof(boot));
    w32(hs+0x100,0x120); w32(hs+0x104,16);
    w32(hs+0x108,0x130); w32(hs+0x10c,16);
    w32(hs+0x110,0x140); w32(hs+0x114,0x144);
    w32(hs+0x118,0x148); w32(hs+0x11c,36);
    for (unsigned i=0;i<32;++i) hs[0x120+i]=(uint8_t)(i+1);
    w32(hs+0x140,0x880); w32(hs+0x14c,0x100);
    w32(hs+0x150,0x500); w32(hs+0x154,0x400); w32(hs+0x158,1);
    w32(hs+0x15c,0x100); w32(hs+0x160,0x400);
    w32(boot+0x100,0x100); w32(boot+0x10c,0x200);
    for (unsigned i=0x200;i<0x400;++i) boot[i]=(uint8_t)i;
    w32(pdesc,sizeof(pdesc)); w32(pdesc+4,sizeof(pmu));
    w32(pdesc+84,128); w32(pdesc+88,0x1000); w32(pdesc+92,0x1000);
    w32(pdesc+96,0x100); w32(pdesc+100,0x500);
    w32(pdesc+120,0x100); w32(pdesc+124,0x300); w32(pdesc+128,0x100);
    w32(fsig+64,1); w32(fsig+68,1); w32(fsig+72,2);
    w32(psig+64,1); w32(psig+68,1);
    gr.surface=surface; gr.image.size=sizeof(surface);
    gr.image.falcon[0]=(struct tg_gpu_gr_falcon_image){
        {0,0x200},{0x200,0x104},{0x400,0x104},0x5c00,0x5c00,0
    };
    f=(struct tg_gpu_acr_files){
        {hs,sizeof(hs)},{boot,sizeof(boot)},{pmu,sizeof(pmu)},{pdesc,sizeof(pdesc)},
        {psig,sizeof(psig)},{fsig,sizeof(fsig)},&gr
    };
    assert(tg_gpu_acr_prepare(&f,false,&v)==TG_GPU_ACR_OK);
    /* Fixed format oracle: 20-byte WPR records, 124-byte LSB headers,
     * 4 KiB image alignment and one 256-byte loader slot per Falcon.
     */
    assert(v.wpr_size==0x2700 && v.wpr_offset==0x2000 && v.size==0x4700);
    assert(v.lsf[0].lsb_offset==0x100 && v.lsf[0].image_offset==0x1000);
    assert(v.lsf[1].lsb_offset==0x1700 && v.lsf[1].image_offset==0x2000);
    assert(tg_gpu_acr_pack(&v,&addresses,out,sizeof(out),bld)==TG_GPU_ACR_OK);
    assert(!memcmp(out+0x880,hs+0x130,16));
    assert(!memcmp(out+v.boot_offset,boot+0x200,0x200));
    assert(r32(out+0x500+576)==0x2700 && r32(out+0x500+584)==0x90002000);
    assert(r32(out+0x2000)==2 && r32(out+0x2014)==0 && r32(out+0x2028)==UINT32_MAX);
    assert(r32(out+0x2010)==1 && r32(out+0x2024)==1); /* COPY, not authenticated */
    assert(!memcmp(out+0x2100,fsig,76));
    assert(r32(out+0x3600+36)==0xffc012 && r32(out+0x3600+60)==0xffc014);
    assert(r32(out+0x4600+4)==0xffc021 && r32(out+0x4600+20)==0xffc024);
    assert(r32(out+0x4600+36)==0x3fc0 && r32(out+0x3700+120)==4);
    assert(bld[8]==1 && bld[9]==0x300 && bld[15]==0x305 && bld[16]==0x400);
    memset(out,0xa5,sizeof(out));
    assert(tg_gpu_acr_pack(&v,&addresses,out,v.size-1,bld)==TG_GPU_ACR_SIZE);
    a=addresses; a.physical=a.wpr_base;
    assert(tg_gpu_acr_pack(&v,&a,out,sizeof(out),bld)==TG_GPU_ACR_ADDRESS);
    a=addresses; a.gpu_va=(UINT64_C(1)<<40)-256;
    assert(tg_gpu_acr_pack(&v,&a,out,sizeof(out),bld)==TG_GPU_ACR_ADDRESS);
    a=addresses; a.pmu_dmem_size=0x80;
    assert(tg_gpu_acr_pack(&v,&a,out,sizeof(out),bld)==TG_GPU_ACR_ADDRESS);
    for (unsigned i=0;i<sizeof(out);++i) assert(out[i]==0xa5);
    w32(hs+0x140,0xfffffff0);
    assert(tg_gpu_acr_prepare(&f,false,&bad)==TG_GPU_ACR_FORMAT && !bad.size);
    w32(hs+0x140,0x880); w32(hs+0x144,0xffffffff);
    assert(tg_gpu_acr_prepare(&f,false,&bad)==TG_GPU_ACR_SIGNATURE_PROFILE);
    w32(hs+0x144,0); w32(psig+64,0);
    assert(tg_gpu_acr_prepare(&f,false,&bad)==TG_GPU_ACR_SIGNATURE_PROFILE);
    assert(tg_gpu_acr_prepare(&f,true,&bad)==TG_GPU_ACR_OK);
    assert(tg_gpu_acr_pack(&bad,&addresses,out,sizeof(out),bld)==TG_GPU_ACR_OK);
    assert(!memcmp(out+0x880,hs+0x120,16));
    w32(psig+64,1); w32(pdesc+124,0xfffffff0);
    assert(tg_gpu_acr_prepare(&f,false,&bad)==TG_GPU_ACR_FORMAT);
    w32(pdesc+124,0x300); f.hs.size--;
    assert(tg_gpu_acr_prepare(&f,false,&bad)==TG_GPU_ACR_FORMAT);
    puts("ACR format, WPR layout, address patching and failure-before-write: PASS (model)");
}
static struct tg_gpu_gr_blob load(const char *root, const char *name)
{
    char path[2048]; FILE *file; long size; uint8_t *p;
    assert(snprintf(path,sizeof(path),"%s/%s",root,name)>0);
    file=fopen(path,"rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); size=ftell(file); assert(size>0 && size<0x400000);
    rewind(file); p=malloc((size_t)size); assert(p);
    assert(fread(p,1,(size_t)size,file)==(size_t)size); fclose(file);
    return (struct tg_gpu_gr_blob){p,(size_t)size};
}
static void files(const char *dir)
{
    struct tg_gpu_gr_blob n=load(dir,"NETB_img.bin"), boots[2]={load(dir,"fecs.bin"),load(dir,"gpccs.bin")};
    struct tg_gpu_gr_payload gr={0};
    struct tg_gpu_acr_files f={load(dir,"nv_acr_ucode_prod.bin"),load(dir,"pmu_bl.bin"),
        load(dir,"gpmu_ucode_image.bin"),load(dir,"gpmu_ucode_desc.bin"),
        load(dir,"pmu_sig.bin"),load(dir,"fecs_sig.bin"),&gr};
    struct tg_gpu_acr_image v;
    uint8_t *out; uint32_t bld[19];
    assert(tg_gpu_gr_image_prepare(n,boots,NULL,0,&gr.image)==TG_GPU_GR_FW_OK);
    gr.surface=malloc(gr.image.size); assert(gr.surface);
    assert(tg_gpu_gr_image_prepare(n,boots,(void *)gr.surface,gr.image.size,&gr.image)==TG_GPU_GR_FW_OK);
    assert(tg_gpu_acr_prepare(&f,false,&v)==TG_GPU_ACR_OK);
    /* Both signed GM20B images use the same PMU and FECS package. */
    const int noble = f.hs.size == 18944;
    assert(v.hs.size==(noble ? 0x4800u : 0x4900u) && v.boot.size==0x200);
    assert(v.signature_offset==(noble ? 0x2690u : 0x2790u));
    assert(v.secure_offset==256 && v.secure_size==(noble ? 6656u : 6912u));
    assert(v.dmem_offset==(noble ? 6912u : 7168u) && v.dmem_size==3072);
    assert(v.pmu_version==20490253u);
    assert(v.wpr_size==0x12c00 && v.wpr_offset==0x5000 && v.size==0x17c00);
    /* FECS file padding is not boot code: its descriptor declares 256 bytes. */
    assert(v.lsf[0].image.size==0x5600 && v.lsf[1].image.size==0xbb00);
    out=malloc(v.size); assert(out);
    assert(tg_gpu_acr_pack(&v,&addresses,out,v.size,bld)==TG_GPU_ACR_OK);
    assert(!memcmp(out+v.signature_offset,f.hs.data+0x130,16));
    assert(!memcmp(out+v.wpr_offset+v.lsf[0].lsb_offset,f.fecs_sig.data,76));
    assert(!memcmp(out+v.wpr_offset+v.lsf[1].image_offset,f.pmu.data,f.pmu.size));
    assert(r32(out+v.dmem_offset+584)==addresses.physical+v.wpr_offset);
    printf("NVIDIA GM20B ACR packing: PASS (host only); WPR=%u total=%u PMU=%08x\n",
        v.wpr_size,v.size,v.pmu_version);
    free(out); free((void *)gr.surface); free((void *)n.data);
    for (unsigned i=0;i<2;++i) free((void *)boots[i].data);
    free((void *)f.hs.data); free((void *)f.hs_boot.data); free((void *)f.pmu.data);
    free((void *)f.pmu_desc.data); free((void *)f.pmu_sig.data); free((void *)f.fecs_sig.data);
}
int main(int argc, char **argv)
{
    assert(argc==1 || argc==2); model(); if (argc==2) files(argv[1]); return 0;
}
