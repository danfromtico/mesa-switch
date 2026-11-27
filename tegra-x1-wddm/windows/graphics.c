/* SPDX-License-Identifier: MIT */
#include "graphics.h"

static BOOLEAN instance_valid(const TG_GPU_PRIVATE_MEMORY *m)
{
    UINT64 physical;
    if(!m || !m->Handle || !m->CpuAddress || m->Bytes!=TG_GPU_CONTEXT_PRIVATE_BYTES ||
       ((ULONG_PTR)m->CpuAddress&(PAGE_SIZE-1)))return FALSE;
    physical=(UINT64)m->SystemPhysicalAddress.QuadPart;
    return !(physical&(PAGE_SIZE-1)) && physical>=0x80000000 &&
        physical<=UINT64_C(0x100000000)-TG_GPU_CONTEXT_PRIVATE_BYTES;
}
static BOOLEAN internal_valid(const TG_GPU_INTERNAL_VA *v)
{
    return v && v->Leaf.Handle && v->Leaf.CpuAddress && v->Leaf.Bytes==TG_GPU_TABLE_BYTES &&
        !((ULONG_PTR)v->Leaf.CpuAddress&(PAGE_SIZE-1)) && v->Base &&
        !(v->Base&(TG_GPU_INTERNAL_COVERAGE-1)) &&
        v->Base<=TG_GPU_VA_LIMIT-TG_GPU_INTERNAL_COVERAGE;
}
NTSTATUS TgGpuCreateGraphicsTemplate(const DXGKRNL_INTERFACE *dxgk,
    const struct tg_gpu_gr_work *work,const struct tg_gpu_gr_context_status *s,
    TG_GPU_GRAPHICS_TEMPLATE *out)
{
    NTSTATUS status;const UCHAR *source;
    if(!dxgk || !out || out->Memory.Handle || !work || !work->cpu || !s || s->result ||
       s->stage!=9 || s->image_magic!=0x600dc0de || !s->image_changed_words ||
       s->gpcs!=1 || s->tpcs!=2 || s->zculls!=4 ||
       s->context_bytes<TG_GPU_GR_IMAGE_HEADER_BYTES || s->context_bytes>TG_GPU_GR_IMAGE_MAX_BYTES ||
       (s->context_bytes&3) || ((ULONG_PTR)work->cpu&3) ||
       work->bytes<TG_GPU_GR_GOLD_OFFSET+s->context_bytes)return STATUS_INVALID_PARAMETER;
    source=(const UCHAR *)work->cpu+TG_GPU_GR_GOLD_OFFSET;
    if(*(const UINT32 *)(source+0xfc)!=0x600dc0de)return STATUS_DEVICE_DATA_ERROR;
    status=TgGpuAllocatePrivateMemory(dxgk,TG_GPU_GR_IMAGE_MAX_BYTES,0x80000000,0xffffffff,&out->Memory);
    if(!NT_SUCCESS(status))return status;
    RtlCopyMemory(out->Memory.CpuAddress,source,s->context_bytes);
    out->ImageBytes=s->context_bytes;TgGpuDataSyncBarrier();return STATUS_SUCCESS;
}
NTSTATUS TgGpuBuildContextGraphics(const TG_GPU_GRAPHICS_TEMPLATE *t,
    const TG_GPU_PRIVATE_MEMORY *m,UINT64 internal,UINT id)
{
    struct tg_gpu_gr_patch patch[TG_GPU_GR_GLOBAL_PATCH_COUNT];
    struct tg_gpu_gr_image_config config;
    struct tg_gpu_gr_global_addresses globals;
    UINT64 va;UCHAR *cpu;UINT32 *instance;
    if(!t || !t->Memory.Handle || !t->Memory.CpuAddress || t->Memory.Bytes!=TG_GPU_GR_IMAGE_MAX_BYTES ||
       !instance_valid(m) || !internal || (internal&(TG_GPU_INTERNAL_COVERAGE-1)) ||
       internal>TG_GPU_VA_LIMIT-TG_GPU_INTERNAL_COVERAGE || !id ||
       t->ImageBytes<TG_GPU_GR_IMAGE_HEADER_BYTES || t->ImageBytes>TG_GPU_GR_IMAGE_MAX_BYTES)
        return STATUS_INVALID_PARAMETER;
    va=internal+TG_GPU_CONTEXT_GR_VA_OFFSET;
    globals=(struct tg_gpu_gr_global_addresses){va+TG_GPU_CONTEXT_BUNDLE_OFFSET,
        va+TG_GPU_CONTEXT_PAGEPOOL_OFFSET,va+TG_GPU_CONTEXT_ATTRIB_OFFSET};
    config=(struct tg_gpu_gr_image_config){va+TG_GPU_CONTEXT_ACCESS_OFFSET,
        va+TG_GPU_CONTEXT_PATCH_OFFSET,TG_GPU_GR_PATCH_BYTES,TG_GPU_GR_GLOBAL_PATCH_COUNT,id};
    if(!tg_gpu_gr_context_global_patches(&globals,patch,TG_GPU_GR_GLOBAL_PATCH_COUNT))
        return STATUS_INVALID_ADDRESS;
    cpu=(UCHAR *)m->CpuAddress+TG_GPU_CONTEXT_GR_OFFSET;
    RtlZeroMemory(cpu,TG_GPU_CONTEXT_GR_BYTES);
    if(tg_gpu_gr_image_build(cpu,TG_GPU_GR_IMAGE_MAX_BYTES,t->Memory.CpuAddress,t->ImageBytes,&config)
           !=TG_GPU_GR_IMAGE_OK ||
       !tg_gpu_gr_access_map_build(cpu+TG_GPU_CONTEXT_ACCESS_OFFSET,TG_GPU_GR_ACCESS_MAP_BYTES) ||
       !tg_gpu_gr_patch_build(cpu+TG_GPU_CONTEXT_PATCH_OFFSET,TG_GPU_GR_PATCH_BYTES,
                             patch,TG_GPU_GR_GLOBAL_PATCH_COUNT))return STATUS_DEVICE_DATA_ERROR;
    instance=m->CpuAddress;instance[132]=(UINT32)va|4;instance[133]=(UINT32)(va>>32);
    TgGpuDataSyncBarrier();return STATUS_SUCCESS;
}
NTSTATUS TgGpuMapContextGraphics(TG_GPU_INTERNAL_VA *v,const TG_GPU_PRIVATE_MEMORY *m)
{
    UINT64 entries[TG_GPU_CONTEXT_GR_BYTES/PAGE_SIZE];
    UINT64 physical;volatile UINT64 *pt;
    if(!internal_valid(v) || !instance_valid(m))return STATUS_INVALID_PARAMETER;
    physical=(UINT64)m->SystemPhysicalAddress.QuadPart+TG_GPU_CONTEXT_GR_OFFSET;
    for(UINT i=0;i<TG_GPU_CONTEXT_GR_BYTES/PAGE_SIZE;++i) {
        BOOLEAN read_only=i*PAGE_SIZE>=TG_GPU_CONTEXT_ACCESS_OFFSET;
        if(!tg_gpu_pte_4k(physical+(UINT64)i*PAGE_SIZE,read_only,true,&entries[i]))
            return STATUS_INVALID_ADDRESS;
        entries[i]|=2; /* GR private buffers use privileged mappings in nvgpu. */
    }
    pt=(volatile UINT64 *)v->Leaf.CpuAddress+TG_GPU_CONTEXT_GR_VA_OFFSET/PAGE_SIZE;
    TgGpuDataSyncBarrier();
    for(UINT i=0;i<TG_GPU_CONTEXT_GR_BYTES/PAGE_SIZE;++i)pt[i]=entries[i];
    v->GraphicsPhysical=physical;TgGpuDataSyncBarrier();return STATUS_SUCCESS;
}
VOID TgGpuUnmapContextGraphics(TG_GPU_INTERNAL_VA *v,const TG_GPU_PRIVATE_MEMORY *m)
{
    volatile UINT64 *pt;
    if(!internal_valid(v) || !instance_valid(m) ||
       v->GraphicsPhysical!=(UINT64)m->SystemPhysicalAddress.QuadPart+TG_GPU_CONTEXT_GR_OFFSET)return;
    pt=(volatile UINT64 *)v->Leaf.CpuAddress+TG_GPU_CONTEXT_GR_VA_OFFSET/PAGE_SIZE;
    TgGpuDataSyncBarrier();
    for(UINT i=0;i<TG_GPU_CONTEXT_GR_BYTES/PAGE_SIZE;++i)pt[i]=0;
    v->GraphicsPhysical=0;TgGpuDataSyncBarrier();
}
