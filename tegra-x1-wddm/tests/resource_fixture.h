/* SPDX-License-Identifier: MIT */
/* Controlled Dxgkrnl resource callbacks; never maps real hardware. */
static _Alignas(8) UCHAR resource_storage[4096];
static UINT resource_maps,resource_unmaps,resource_fail_map,resource_fail_unmap;
static ULONG resource_live;
static const ULONG resource_profile[][2]={
    {T214_GPU_REG_BASE,T214_GPU_REG_SIZE},{T214_GPU_BAR1_BASE,T214_GPU_BAR1_SIZE},
    {T214_GPU_AUX_BASE,T214_GPU_AUX_SIZE},
#define RESOURCE_PAIR(base,size) {base,size},
    T214_GPU_EXTRA_RESOURCES(RESOURCE_PAIR)
#undef RESOURCE_PAIR
};
static NTSTATUS APIENTRY ResourceInfo(HANDLE device,DXGK_DEVICE_INFO *info)
{
    CHECK(device==(HANDLE)1);
    info->TranslatedResourceList=(PCM_RESOURCE_LIST)resource_storage;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY ResourceMap(HANDLE device,PHYSICAL_ADDRESS address,ULONG bytes,
    BOOLEAN io,BOOLEAN user,MEMORY_CACHING_TYPE cache,PVOID *output)
{
    CHECK(device==(HANDLE)1&&!io&&!user&&cache==MmNonCached);
    ++resource_maps;
    if(resource_maps<=T214_GPU_RESOURCE_COUNT)
        CHECK(address.QuadPart==resource_profile[resource_maps-1][0]&&
              bytes==resource_profile[resource_maps-1][1]);
    else CHECK(address.QuadPart==T214_GPU_LIC_BASE&&bytes==T214_GPU_LIC_SIZE);
    if(resource_maps==resource_fail_map) return STATUS_INSUFFICIENT_RESOURCES;
    resource_live|=1u<<(resource_maps-1);
    *output=(PVOID)(ULONG_PTR)(0x10000+resource_maps*PAGE_SIZE);
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY ResourceUnmap(HANDLE device,PVOID address)
{
    UINT index=(UINT)(((ULONG_PTR)address-0x10000)/PAGE_SIZE);
    CHECK(device==(HANDLE)1&&index&&index<=T214_GPU_RESOURCE_COUNT+1);
    CHECK(resource_live&(1u<<(index-1)));++resource_unmaps;
    if(index==resource_fail_unmap) return STATUS_UNSUCCESSFUL;
    resource_live&=~(1u<<(index-1));return STATUS_SUCCESS;
}
static CM_PARTIAL_RESOURCE_LIST *ResourceList(BOOLEAN lic)
{
    PCM_RESOURCE_LIST list=(PCM_RESOURCE_LIST)resource_storage;
    RtlZeroMemory(resource_storage,sizeof(resource_storage));list->Count=1;
    CM_PARTIAL_RESOURCE_LIST *partial=&list->List[0].PartialResourceList;
    partial->Count=T214_GPU_RESOURCE_COUNT+2+(lic?1:0);
    for(UINT i=0;i<T214_GPU_RESOURCE_COUNT+(lic?1u:0u);++i) {
        CM_PARTIAL_RESOURCE_DESCRIPTOR *r=&partial->PartialDescriptors[i];
        r->Type=CmResourceTypeMemory;r->ShareDisposition=CmResourceShareDeviceExclusive;
        r->u.Memory.Start.QuadPart=i<T214_GPU_RESOURCE_COUNT?resource_profile[i][0]:T214_GPU_LIC_BASE;
        r->u.Memory.Length=i<T214_GPU_RESOURCE_COUNT?resource_profile[i][1]:T214_GPU_LIC_SIZE;
    }
    for(UINT i=0;i<2;++i) {
        CM_PARTIAL_RESOURCE_DESCRIPTOR *r=&partial->PartialDescriptors[partial->Count-2+i];
        r->Type=CmResourceTypeInterrupt;r->ShareDisposition=CmResourceShareDeviceExclusive;
        r->u.Interrupt.Vector=0x200+i;r->u.Interrupt.Level=5+i;r->u.Interrupt.Affinity=1;
    }
    resource_maps=resource_unmaps=resource_live=resource_fail_map=resource_fail_unmap=0;
    return partial;
}
