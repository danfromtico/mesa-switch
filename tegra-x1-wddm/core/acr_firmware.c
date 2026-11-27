/* SPDX-License-Identifier: MIT
 * Copyright (c) 2015-2018, NVIDIA CORPORATION. All rights reserved.
 * GM20B ACR/LSF formats and layout follow Switchroot nvgpu 1ae0167d,
 * gm20b/acr_gm20b.c and the include/nvgpu/acr headers. See ../NOTICE for license.
 */
#include "acr_firmware.h"

#define SIGNATURE_BYTES 76u
#define LSB_BYTES (SIGNATURE_BYTES + 48u)
#define ADDRESS_LIMIT (UINT64_C(1) << 40)

static uint32_t get32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(i*8)); }
static void copy(uint8_t *to, const uint8_t *from, size_t n)
{ for (size_t i=0;i<n;++i) to[i]=from[i]; }
static bool span(size_t total, uint32_t offset, uint32_t size)
{ return size && offset<=total && size<=total-offset; }
static bool align32(uint32_t value, uint32_t alignment, uint32_t *out)
{
    if (value>UINT32_MAX-(alignment-1)) return false;
    *out=(value+alignment-1)&~(alignment-1); return true;
}
static bool append(uint32_t *cursor, uint32_t bytes, uint32_t alignment, uint32_t *offset)
{
    if (!align32(*cursor,alignment,offset) || bytes>UINT32_MAX-*offset) return false;
    *cursor=*offset+bytes; return true;
}
struct bin { uint32_t metadata, data, size; };
static bool binary(struct tg_gpu_gr_blob f, uint32_t metadata_size, struct bin *b)
{
    if (!f.data || f.size<24 || f.size>UINT32_MAX || get32(f.data)!=0x10de ||
        get32(f.data+4)!=1 || get32(f.data+8)!=f.size) return false;
    b->metadata=get32(f.data+12); b->data=get32(f.data+16); b->size=get32(f.data+20);
    return b->metadata>=24 && span(f.size,b->metadata,metadata_size) &&
        !(b->data&255) && b->data>=b->metadata+metadata_size && span(f.size,b->data,b->size);
}
static enum tg_gpu_acr_result hs_parse(const struct tg_gpu_acr_files *f,
    bool debug, struct tg_gpu_acr_image *v)
{
    struct bin hs, boot;
    const uint8_t *h, *e, *b;
    uint32_t sig, sig_size, loc, index, exec, exec_size, code_size;
    if (!binary(f->hs,32,&hs) || !binary(f->hs_boot,24,&boot)) return TG_GPU_ACR_FORMAT;
    h=f->hs.data+hs.metadata;
    sig=get32(h+(debug?0:8)); sig_size=get32(h+(debug?4:12));
    loc=get32(h+16); index=get32(h+20); exec=get32(h+24); exec_size=get32(h+28);
    if (!span(f->hs.size,loc,4) || !span(f->hs.size,index,4) || exec_size<28 ||
        !span(f->hs.size,exec,exec_size)) return TG_GPU_ACR_FORMAT;
    loc=get32(f->hs.data+loc); index=get32(f->hs.data+index);
    if (!sig_size || !span(f->hs.size,sig,sig_size) || index>UINT32_MAX/16 ||
        !span(sig_size,index*16,16)) return TG_GPU_ACR_SIGNATURE_PROFILE;
    if ((loc&3) || !span(hs.size,loc,16)) return TG_GPU_ACR_FORMAT;
    v->hs=(struct tg_gpu_gr_blob){f->hs.data+hs.data,hs.size};
    v->hs_signature=(struct tg_gpu_gr_blob){f->hs.data+sig+index*16,16};
    v->signature_offset=loc;
    e=f->hs.data+exec;
    v->nonsec_offset=get32(e); v->nonsec_size=get32(e+4);
    v->dmem_offset=get32(e+8); v->dmem_size=get32(e+12);
    v->secure_offset=get32(e+20); v->secure_size=get32(e+24);
    if (get32(e+16)!=1 || !span(hs.size,v->nonsec_offset,v->nonsec_size) ||
        !span(hs.size,v->secure_offset,v->secure_size) ||
        !span(hs.size,v->dmem_offset,v->dmem_size) || v->dmem_size<592 ||
        ((v->nonsec_offset|v->secure_offset|v->dmem_offset|v->nonsec_size|
          v->secure_size|v->dmem_size)&255)) return TG_GPU_ACR_FORMAT;
    /* The signed image contains its own ACR descriptor. Only documented
     * runtime address fields may be patched, not its signature location.
     */
    if (loc<v->dmem_offset+592 && loc+16>v->dmem_offset+512) return TG_GPU_ACR_FORMAT;
    b=f->hs_boot.data+boot.metadata;
    v->boot_tag=get32(b); v->boot_dmem=get32(b+4); code_size=get32(b+12);
    if (get32(b+8)!=0 || !code_size || !align32(code_size,256,&code_size) ||
        code_size>boot.size || (v->boot_dmem&3) || v->boot_tag>0xffff)
        return TG_GPU_ACR_FORMAT;
    v->boot=(struct tg_gpu_gr_blob){f->hs_boot.data+boot.data,code_size};
    return TG_GPU_ACR_OK;
}
static bool signature(struct tg_gpu_gr_blob b, uint32_t id, bool debug)
{
    return b.data && b.size>=SIGNATURE_BYTES && get32(b.data+72)==id &&
        get32(b.data+(debug?68:64))==1;
}
static bool lsf_sizes(struct tg_gpu_acr_lsf *l)
{
    uint32_t code, full;
    if (!l->image.data || l->image.size>UINT32_MAX || !l->boot_size ||
        !align32(l->boot_size,256,&l->boot_size) || (l->boot_imem&255) ||
        l->app_start!=l->boot_size || !span(l->image.size,l->app_start,l->app_size) ||
        !span(l->app_size,l->code_offset,l->code_size) ||
        !span(l->app_size,l->data_offset,l->data_size) ||
        ((l->code_offset|l->data_offset)&255) ||
        !align32(l->app_size,256,&full) || full>UINT32_MAX-l->boot_size ||
        full+l->boot_size!=l->image.size ||
        !align32(l->data_offset,256,&code) || code>full) return false;
    l->ucode_size=code+l->boot_size; l->full_data_size=full-code;
    return l->full_data_size!=0;
}
static enum tg_gpu_acr_result lsf_parse(const struct tg_gpu_acr_files *f,
    bool debug, struct tg_gpu_acr_image *v)
{
    struct tg_gpu_acr_lsf *fe=&v->lsf[0], *pm=&v->lsf[1];
    const uint8_t *d=f->pmu_desc.data;
    const struct tg_gpu_gr_falcon_image *g;
    uint32_t desc_size, n, padded_data, image_size;
    if (!signature(f->fecs_sig,2,debug) || !signature(f->pmu_sig,0,debug))
        return TG_GPU_ACR_SIGNATURE_PROFILE;
    if (!d || f->pmu_desc.size<136 || !f->pmu.data || !f->gr || !f->gr->surface)
        return TG_GPU_ACR_ARGUMENT;
    desc_size=get32(d); n=get32(d+132);
    if (desc_size>f->pmu_desc.size || desc_size<140 || n>(desc_size-140)/8 ||
        get32(d+80)!=0 || get32(d+4)!=f->pmu.size) return TG_GPU_ACR_FORMAT;
    pm->id=0; pm->image=f->pmu; pm->signature=f->pmu_sig;
    pm->boot_size=get32(d+84); pm->boot_imem=get32(d+88);
    if (get32(d+92)<pm->boot_imem || get32(d+92)-pm->boot_imem>=pm->boot_size)
        return TG_GPU_ACR_FORMAT;
    pm->app_start=get32(d+96); pm->app_size=get32(d+100); pm->entry=get32(d+108);
    pm->code_offset=get32(d+116); pm->code_size=get32(d+120);
    pm->data_offset=get32(d+124); pm->data_size=get32(d+128);
    v->pmu_version=get32(d+12);
    if (!lsf_sizes(pm)) return TG_GPU_ACR_FORMAT;
    g=&f->gr->image.falcon[0];
    if (g->boot.offset || !g->boot.size || !g->code.size || !g->data.size ||
        !align32(g->data.size,256,&padded_data) ||
        g->data.offset>UINT32_MAX-padded_data) return TG_GPU_ACR_FORMAT;
    image_size=g->data.offset+padded_data;
    if (image_size>f->gr->image.size || g->code.offset>g->data.offset ||
        g->code.size>g->data.offset-g->code.offset) return TG_GPU_ACR_FORMAT;
    fe->id=2; fe->image=(struct tg_gpu_gr_blob){f->gr->surface,image_size};
    fe->signature=f->fecs_sig; fe->boot_size=g->boot.size; fe->boot_imem=g->imem_offset;
    fe->app_start=g->code.offset; fe->app_size=image_size-g->code.offset;
    fe->code_size=g->code.size; fe->data_offset=g->data.offset-g->code.offset;
    fe->data_size=g->data.size;
    if (!lsf_sizes(fe)) return TG_GPU_ACR_FORMAT;
    return TG_GPU_ACR_OK;
}
enum tg_gpu_acr_result tg_gpu_acr_prepare(const struct tg_gpu_acr_files *f,
    bool debug, struct tg_gpu_acr_image *image)
{
    struct tg_gpu_acr_image v={0};
    enum tg_gpu_acr_result result;
    uint32_t cursor=3*20; /* two WPR entries and the invalid-ID terminator */
    if (!image) return TG_GPU_ACR_ARGUMENT;
    *image=v;
    if (!f) return TG_GPU_ACR_ARGUMENT;
    result=hs_parse(f,debug,&v); if (result) return result;
    result=lsf_parse(f,debug,&v); if (result) return result;
    for (unsigned i=0;i<2;++i) {
        struct tg_gpu_acr_lsf *l=&v.lsf[i];
        if (!append(&cursor,LSB_BYTES,256,&l->lsb_offset) ||
            !append(&cursor,(uint32_t)l->image.size,4096,&l->image_offset) ||
            !append(&cursor,256,256,&l->loader_offset)) return TG_GPU_ACR_SIZE;
    }
    v.wpr_size=cursor; cursor=(uint32_t)v.hs.size;
    if (!append(&cursor,(uint32_t)v.boot.size,256,&v.boot_offset) ||
        !append(&cursor,v.wpr_size,4096,&v.wpr_offset)) return TG_GPU_ACR_SIZE;
    v.size=cursor; *image=v; return TG_GPU_ACR_OK;
}
static bool address_span(uint64_t address, uint64_t bytes)
{ return address && address<ADDRESS_LIMIT && bytes && bytes<=ADDRESS_LIMIT-address; }
enum tg_gpu_acr_result tg_gpu_acr_pack(const struct tg_gpu_acr_image *v,
    const struct tg_gpu_acr_addresses *a, void *storage, size_t capacity, uint32_t desc[19])
{
    uint8_t *out=storage, *wpr, *d;
    uint64_t hs_va;
    uint32_t args;
    if (!v || !a || !out || !desc) return TG_GPU_ACR_ARGUMENT;
    if (!v->size || capacity<v->size) return TG_GPU_ACR_SIZE;
    if ((a->gpu_va&255) || (a->physical&255) || (a->wpr_base&0x1ffff) ||
        (a->wpr_size&0x1ffff) || a->wpr_size<v->wpr_size ||
        !address_span(a->gpu_va,v->size) || !address_span(a->physical,v->size) ||
        !address_span(a->wpr_base,a->wpr_size) ||
        (a->physical<a->wpr_base+a->wpr_size && a->physical+v->size>a->wpr_base) ||
        !a->pmu_args_size || (a->pmu_args_size&3) ||
        a->pmu_args_size>a->pmu_dmem_size ||
        a->pmu_dmem_size-a->pmu_args_size<v->lsf[1].data_size)
        return TG_GPU_ACR_ADDRESS;
    args=a->pmu_dmem_size-a->pmu_args_size;
    for (uint32_t i=0;i<v->size;++i) out[i]=0;
    for (unsigned i=0;i<19;++i) desc[i]=0;
    copy(out,v->hs.data,v->hs.size);
    copy(out+v->signature_offset,v->hs_signature.data,16);
    copy(out+v->boot_offset,v->boot.data,v->boot.size);
    /* flcn_acr_desc layout: union[512], region/offset/mmu, regions[52],
     * blob size, four alignment bytes, and the 64-bit physical staging base.
     */
    d=out+v->dmem_offset;
    put32(d+516,0); put32(d+524,1); put32(d+576,v->wpr_size);
    put32(d+584,(uint32_t)(a->physical+v->wpr_offset));
    put32(d+588,(uint32_t)((a->physical+v->wpr_offset)>>32));
    wpr=out+v->wpr_offset;
    for (unsigned i=0;i<2;++i) {
        const struct tg_gpu_acr_lsf *l=&v->lsf[i];
        uint8_t *header=wpr+i*20, *lsb=wpr+l->lsb_offset, *loader=wpr+l->loader_offset;
        uint64_t base=a->wpr_base+l->image_offset+l->app_start;
        uint32_t code=(uint32_t)((base+l->code_offset)>>8);
        uint32_t data=(uint32_t)((base+l->data_offset)>>8);
        put32(header,l->id); put32(header+4,l->lsb_offset);
        put32(header+16,1); /* COPY, not VALIDATION_DONE or BOOTSTRAP_READY */
        copy(lsb,l->signature.data,SIGNATURE_BYTES);
        put32(lsb+76,l->image_offset); put32(lsb+80,l->ucode_size);
        put32(lsb+84,l->full_data_size); put32(lsb+88,l->boot_size);
        put32(lsb+92,l->boot_imem); put32(lsb+96,l->loader_offset);
        put32(lsb+100,256);
        if (l->id) {
            put32(lsb+104,l->boot_size); put32(lsb+108,l->ucode_size-l->boot_size);
            put32(lsb+112,l->ucode_size); put32(lsb+116,l->full_data_size);
            put32(loader+36,code); put32(loader+44,l->code_size);
            put32(loader+56,l->entry); put32(loader+60,data); put32(loader+64,l->data_size);
        } else {
            put32(lsb+120,4); /* PMU requires its instance context. */
            put32(loader+4,code); put32(loader+8,l->app_size);
            put32(loader+12,l->code_size); put32(loader+16,l->entry);
            put32(loader+20,data); put32(loader+24,l->data_size);
            put32(loader+28,code); put32(loader+32,1); put32(loader+36,args);
        }
        copy(wpr+l->image_offset,l->image.data,l->image.size);
    }
    put32(wpr+40,UINT32_MAX);
    hs_va=a->gpu_va;
    desc[8]=1; desc[9]=(uint32_t)(hs_va>>8); /* PMU virtual DMA index */
    desc[10]=v->nonsec_offset; desc[11]=v->nonsec_size;
    desc[12]=v->secure_offset; desc[13]=v->secure_size;
    desc[15]=(uint32_t)((hs_va+v->dmem_offset)>>8); desc[16]=v->dmem_size;
    return TG_GPU_ACR_OK;
}
