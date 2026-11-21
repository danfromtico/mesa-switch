/* SPDX-License-Identifier: MIT
 * WDDM allocation/handle ownership follows Microsoft's graphics-driver-samples
 * at de4a2161 and the documented Dxgkrnl contracts. See README for references.
 */
#include "allocation.h"
#include "adapter.h"
#include "adapter_abi.h"
#include "../platform/t214_dc.h"
#include "../platform/t214_resources.h"

#define TAG 0x41674754u
typedef struct _TG_GPU_RESOURCE {
    TG_GPU_CONTEXT_ENGINE *Engine;
    UINT Allocations,Builders;
    BOOLEAN FixedAllocations;
    struct tg_gpu_resource_request Metadata;
} TG_GPU_RESOURCE;
typedef union _TG_GPU_ALLOCATION_DESC {
    struct tg_gpu_allocation_request Buffer;
    TG_GPU_PRIMARY_ALLOCATION_REQUEST Primary;
    TG_GPU_LINEAR_ALLOCATION_REQUEST Linear;
} TG_GPU_ALLOCATION_DESC;
typedef struct _TG_GPU_ALLOCATION {
    TG_GPU_CONTEXT_ENGINE *Engine;
    TG_GPU_RESOURCE *Resource;
    TG_GPU_ALLOCATION_DESC Desc;
    SIZE_T Bytes;
    UINT Opens;
} TG_GPU_ALLOCATION;
typedef struct _TG_GPU_OPEN_ALLOCATION {
    TG_GPU_DEVICE *Device;
    TG_GPU_ALLOCATION *Allocation;
    BOOLEAN ReadOnly;
} TG_GPU_OPEN_ALLOCATION;

static NTSTATUS linear_layout(UINT width,UINT height,D3DDDIFORMAT format,UINT *pitch,UINT64 *bytes)
{
    UINT bpp;
    if(!width || !height)return STATUS_INVALID_PARAMETER;
    switch(format) {
    case D3DDDIFMT_UNKNOWN:
        /* ExistingHeap is delivered as an untyped one-row byte buffer. */
        if(height!=1 || (width&(PAGE_SIZE-1)))return STATUS_INVALID_PARAMETER;
        *pitch=width;*bytes=width;
        return width<=TG_GPU_MEMORY_BUDGET_BYTES?STATUS_SUCCESS:STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    case D3DDDIFMT_A8R8G8B8:case D3DDDIFMT_X8R8G8B8:
    case D3DDDIFMT_A8B8G8R8:case D3DDDIFMT_X8B8G8R8:
    case D3DDDIFMT_A2R10G10B10:case D3DDDIFMT_A2B10G10R10:
        bpp=4;break;
    case D3DDDIFMT_A16B16G16R16F:bpp=8;break;
    default:return STATUS_NOT_SUPPORTED;
    }
    const UINT64 row=((UINT64)width*bpp+127)&~127ull;
    const UINT64 rows=((UINT64)height+3)&~3ull;
    if(row>TG_GPU_MEMORY_BUDGET_BYTES || rows>TG_GPU_MEMORY_BUDGET_BYTES/row)
        return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    *pitch=(UINT)row;
    *bytes=(row*rows+PAGE_SIZE-1)&~(UINT64)(PAGE_SIZE-1);
    return STATUS_SUCCESS;
}

static NTSTATUS request(const DXGK_ALLOCATIONINFO *info,TG_GPU_ALLOCATION_DESC *desc)
{
    const UINT size=info->PrivateDriverDataSize;
    const struct tg_gpu_allocation_request *r=&desc->Buffer;
    if(!info->pPrivateDriverData || (size!=sizeof(*r) &&
        size!=TG_GPU_PRIMARY_LEGACY_BYTES && size!=sizeof(desc->Primary)))
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    RtlZeroMemory(desc,sizeof(*desc));
    RtlCopyMemory(desc,info->pPrivateDriverData,size);
    if(info->FlagsWddm2.CreateInVpr) return STATUS_INVALID_PARAMETER;
    if(r->version!=TG_GPU_ALLOCATION_ABI) return STATUS_GRAPHICS_DRIVER_MISMATCH;
    if((r->flags&~(TG_GPU_ALLOCATION_CPU_VISIBLE|TG_GPU_ALLOCATION_DISPLAY_PRIMARY|
                   TG_GPU_ALLOCATION_LINEAR_SURFACE)) ||
        !r->bytes || r->bytes>TG_GPU_VA_LIMIT)
        return STATUS_INVALID_PARAMETER;
    if(r->flags&TG_GPU_ALLOCATION_LINEAR_SURFACE) {
        UINT pitch;UINT64 bytes;
        if(size!=sizeof(desc->Linear) || r->flags!=(TG_GPU_ALLOCATION_CPU_VISIBLE|
            TG_GPU_ALLOCATION_LINEAR_SURFACE) || desc->Linear.Format==D3DDDIFMT_UNKNOWN)
            return STATUS_INVALID_PARAMETER;
        NTSTATUS status=linear_layout(desc->Linear.Width,desc->Linear.Height,
                                     desc->Linear.Format,&pitch,&bytes);
        if(!NT_SUCCESS(status))return status;
        return pitch==desc->Linear.Pitch && bytes==r->bytes?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
    }
    if(size==sizeof(*r))return STATUS_SUCCESS;
    TG_GPU_PRIMARY_ALLOCATION_REQUEST *primary=&desc->Primary;
    if(size==TG_GPU_PRIMARY_LEGACY_BYTES) {
        if(primary->Rotation)return STATUS_INVALID_PARAMETER; /* Old reserved word. */
        primary->Rotation=D3DDDI_ROTATION_IDENTITY;
        primary->Width=T214_DC_PANEL_WIDTH;
        primary->Height=T214_DC_PANEL_HEIGHT;
        primary->Pitch=T214_DC_PANEL_PITCH;
    }
    if(!(r->flags&TG_GPU_ALLOCATION_CPU_VISIBLE) || r->bytes!=T214_GPU_POST_FB_SIZE ||
        primary->ModeRotation>D3DDDI_ROTATION_270 ||
        (primary->ModeRotation && !(r->flags&TG_GPU_ALLOCATION_DISPLAY_PRIMARY)) ||
        primary->Rotation!=D3DDDI_ROTATION_IDENTITY ||
        !((primary->Width==T214_DC_PANEL_WIDTH && primary->Height==T214_DC_PANEL_HEIGHT) ||
          (!(r->flags&TG_GPU_ALLOCATION_DISPLAY_PRIMARY) &&
           primary->Width==T214_DC_PANEL_HEIGHT && primary->Height==T214_DC_PANEL_WIDTH)) ||
        primary->Pitch!=primary->Width*4 ||
        (primary->Format!=D3DDDIFMT_X8R8G8B8 && primary->Format!=D3DDDIFMT_A8R8G8B8))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}
static UINT64 allocation_size(UINT64 bytes)
{
    return (bytes+PAGE_SIZE-1)&~(UINT64)(PAGE_SIZE-1);
}

static NTSTATUS resource_request(const VOID *data,UINT bytes,
                                struct tg_gpu_resource_request *desc)
{
    if(!data || bytes<sizeof(*desc))return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(desc,data,sizeof(*desc));
    if(desc->version!=TG_GPU_RESOURCE_ABI)return STATUS_GRAPHICS_DRIVER_MISMATCH;
    if(desc->header_bytes!=sizeof(*desc) || !desc->allocation_count ||
        desc->driver_data_bytes!=bytes-sizeof(*desc) || desc->reserved[0] || desc->reserved[1] ||
        !desc->allocation_bytes || desc->allocation_bytes>TG_GPU_MEMORY_BUDGET_BYTES)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY TgGpuDdiCreateAllocation(HANDLE adapter,DXGKARG_CREATEALLOCATION *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_RESOURCE *r=NULL;
    TG_GPU_ALLOCATION_DESC desc;TG_GPU_ALLOCATION *allocation;
    NTSTATUS status;KIRQL irql;UINT i,created=0;BOOLEAN new_resource=FALSE;
    struct tg_gpu_resource_request resource_desc={0};
    UINT64 bytes=0, resource_bytes=0, limit;
    if(!e || !a || !e->Tables || e->Tables->SegmentId!=TG_GPU_EXPLICIT_TABLE_SEGMENT ||
        a->Flags.Reserved || (!a->Flags.Resource && a->hResource) ||
        (a->NumAllocations && !a->pAllocationInfo) || (!a->NumAllocations && !a->Flags.Resource))
        return STATUS_INVALID_PARAMETER;
    /* Validate all user descriptors before allocating or changing a resource. */
    for(i=0;i<a->NumAllocations;++i) a->pAllocationInfo[i].hAllocation=NULL;
    if(a->PrivateDriverDataSize) {
        if(!a->Flags.Resource || a->hResource)
            return STATUS_INVALID_PARAMETER;
        status=resource_request(a->pPrivateDriverData,a->PrivateDriverDataSize,&resource_desc);
        if(!NT_SUCCESS(status))return status;
        if(resource_desc.allocation_count!=a->NumAllocations)
            return STATUS_INVALID_PARAMETER;
    }
    limit=TG_GPU_MEMORY_BUDGET_BYTES-e->Tables->Scanout.Bytes;
    for(i=0;i<a->NumAllocations;++i) {
        status=request(&a->pAllocationInfo[i],&desc);
        if(!NT_SUCCESS(status)) return status;
        const UINT64 size=allocation_size(desc.Buffer.bytes);
        if(a->PrivateDriverDataSize) {
            if(size>resource_desc.allocation_bytes-resource_bytes)
                return STATUS_INVALID_PARAMETER;
            resource_bytes+=size;
        }
        if(desc.Buffer.flags&TG_GPU_ALLOCATION_DISPLAY_PRIMARY) {
            /* VidMm manages residency within the already charged segment. */
            if(!e->Tables->Scanout.Handle || !(desc.Buffer.flags&TG_GPU_ALLOCATION_CPU_VISIBLE) ||
                desc.Buffer.bytes!=T214_GPU_POST_FB_SIZE) return STATUS_INVALID_PARAMETER;
            continue;
        }
        if(size>limit-bytes) return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
        bytes+=size;
    }
    if(a->PrivateDriverDataSize && resource_bytes!=resource_desc.allocation_bytes)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY; }
    if(a->Flags.Resource && a->hResource) {
        r=a->hResource;
        if(r->Engine!=e) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER; }
        if(r->FixedAllocations) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_PARAMETER; }
    }
    /* Charge the batch before releasing the lock so concurrent devices
     * compete for the same UMA ceiling, including in-flight creates. */
    if(e->AllocationBytes>limit || bytes>limit-e->AllocationBytes) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_GRAPHICS_NO_VIDEO_MEMORY;
    }
    e->AllocationBytes+=bytes;
    if(r) ++r->Builders;
    ++e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    status=STATUS_NO_MEMORY;
    if(a->Flags.Resource && !r) {
        r=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*r),TAG);
        if(!r) goto fail;
        r->Engine=e;r->FixedAllocations=a->PrivateDriverDataSize!=0;
        r->Metadata=resource_desc;new_resource=TRUE;
    }
    for(i=0;i<a->NumAllocations;++i) {
        status=request(&a->pAllocationInfo[i],&desc);
        if(!NT_SUCCESS(status))goto fail;
        allocation=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*allocation),TAG);
        if(!allocation) { status=STATUS_NO_MEMORY;goto fail; }
        /* Retain normalized geometry, including legacy portrait descriptions. */
        allocation->Desc=desc;
        allocation->Bytes=(SIZE_T)allocation_size(allocation->Desc.Buffer.bytes);
        allocation->Engine=e;allocation->Resource=r;
        a->pAllocationInfo[i].hAllocation=allocation;++created;
    }
    for(i=0;i<a->NumAllocations;++i) {
        DXGK_ALLOCATIONINFO *info=&a->pAllocationInfo[i];
        PVOID private_data=info->pPrivateDriverData;UINT private_bytes=info->PrivateDriverDataSize;
        /* FlagsWddm2 is output except OS-reserved input bits in the WDK. */
        UINT os_flags=info->FlagsWddm2.Value&0xfff83800u;
        allocation=info->hAllocation;
        RtlZeroMemory(info,sizeof(*info));
        info->pPrivateDriverData=private_data;info->PrivateDriverDataSize=private_bytes;
        info->hAllocation=allocation;info->Size=allocation->Bytes;info->Alignment=PAGE_SIZE;
        const BOOLEAN primary=(allocation->Desc.Buffer.flags&TG_GPU_ALLOCATION_DISPLAY_PRIMARY)!=0;
        const UINT segment=primary ? TG_GPU_SCANOUT_SEGMENT : TG_GPU_PAGING_APERTURE_SEGMENT;
        /* VidMm redirects GpuMmu allocations from the aperture to implicit
         * system memory when AccessedPhysically is clear. */
        info->PreferredSegment.SegmentId0=segment;
        info->SupportedReadSegmentSet=info->SupportedWriteSegmentSet=1u<<(segment-1);
        info->FlagsWddm2.Value=os_flags;
        info->FlagsWddm2.CpuVisible=(allocation->Desc.Buffer.flags&TG_GPU_ALLOCATION_CPU_VISIBLE)!=0;
        info->FlagsWddm2.AccessedPhysically=primary;
        info->FlagsWddm2.DisableLargePageMapping=1;
        info->AllocationPriority=D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    }
    KeAcquireSpinLock(&e->Lock,&irql);
    e->AllocationCount+=created;
    if(r) {
        r->Allocations+=created;
        if(new_resource) ++e->ResourceCount;else --r->Builders;
        a->hResource=r;
    }
    --e->Creating;KeReleaseSpinLock(&e->Lock,irql);return STATUS_SUCCESS;
fail:
    for(i=0;i<created;++i) {
        ExFreePoolWithTag(a->pAllocationInfo[i].hAllocation,TAG);a->pAllocationInfo[i].hAllocation=NULL;
    }
    if(new_resource) ExFreePoolWithTag(r,TAG);
    KeAcquireSpinLock(&e->Lock,&irql);
    if(r && !new_resource) --r->Builders;
    e->AllocationBytes-=bytes;
    --e->Creating;KeReleaseSpinLock(&e->Lock,irql);return status;
}

static NTSTATUS APIENTRY describe_allocation(HANDLE adapter,DXGKARG_DESCRIBEALLOCATION *args)
{
    PAGED_CODE();
    if(!adapter || !args || !args->hAllocation) return STATUS_INVALID_PARAMETER;
    /* Dxgkrnl holds the KMD allocation alive for this call. */
    const TG_GPU_ALLOCATION *allocation=args->hAllocation;
    if(allocation->Engine!=adapter) return STATUS_INVALID_PARAMETER;
    const TG_GPU_PRIMARY_ALLOCATION_REQUEST *desc=&allocation->Desc.Primary;
    const BOOLEAN linear=(desc->Buffer.flags&TG_GPU_ALLOCATION_LINEAR_SURFACE)!=0;
    const BOOLEAN buffer=desc->Format==D3DDDIFMT_UNKNOWN;
    /* ExistingHeap's cross-adapter creation also asks DescribeAllocation.
     * Describe an untyped buffer in bytes, without inventing a display mode.
     * A legacy primary still requires its actual panel format. */
    if(buffer && ((desc->Buffer.flags&TG_GPU_ALLOCATION_DISPLAY_PRIMARY) ||
        args->Flags.CheckDisplayMode || desc->Buffer.bytes>UINT32_MAX))
        return STATUS_NOT_SUPPORTED;
    UINT width=buffer?(UINT)desc->Buffer.bytes:linear?allocation->Desc.Linear.Width:desc->Width;
    UINT height=buffer?1:linear?allocation->Desc.Linear.Height:desc->Height;
    /* A display-mode query describes the mode, not the application's rotated
     * storage extent. Dxgkrnl compares width/height BEFORE mode rotation; its
     * cached mode stays panel-oriented while DXGI exposes rotated dimensions.
     * Undo only the storage quarter-turn here. NONPREROTATED storage already
     * has identity orientation. Ordinary allocation queries, pitch, backing
     * and the DC's real scan direction remain unchanged. */
    if(args->Flags.CheckDisplayMode && !linear && !buffer &&
       (desc->Buffer.flags&TG_GPU_ALLOCATION_DISPLAY_PRIMARY) &&
       (desc->Rotation==D3DDDI_ROTATION_90 || desc->Rotation==D3DDDI_ROTATION_270)) {
        const UINT swap=width;width=height;height=swap;
    }
    *args=(DXGKARG_DESCRIBEALLOCATION){
        .hAllocation=args->hAllocation,
        .Width=width,
        .Height=height,
        .Format=(D3DDDIFORMAT)desc->Format,
        .MultisampleMethod={.NumSamples=1},
        .RefreshRate=(linear||buffer)?(D3DDDI_RATIONAL){0}:
            (D3DDDI_RATIONAL){desc->RefreshRate.Numerator,desc->RefreshRate.Denominator},
        .Flags=args->Flags,
        .Rotation=(linear||buffer)?D3DDDI_ROTATION_IDENTITY:
            (D3DDDI_ROTATION)(desc->ModeRotation?desc->ModeRotation:desc->Rotation),
    };
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY TgGpuDdiDestroyAllocation(HANDLE adapter,const DXGKARG_DESTROYALLOCATION *a)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;TG_GPU_RESOURCE *resource=NULL;
    TG_GPU_ALLOCATION *allocation;UINT i,j,resource_children=0;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!e || !a || a->Flags.Reserved || (a->NumAllocations && !a->pAllocationList))
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(a->Flags.DestroyResource) {
        resource=a->hResource;
        if(!resource || resource->Engine!=e) { status=STATUS_INVALID_PARAMETER;goto unlock; }
        if(resource->Builders) { status=STATUS_DEVICE_BUSY;goto unlock; }
    }
    /* Handles here are KMD objects supplied by Dxgkrnl. Do not create a second
     * global handle table or cast a UMD D3DKMT_HANDLE into one of these objects.
     */
    for(i=0;i<a->NumAllocations;++i) {
        allocation=a->pAllocationList[i];
        if(!allocation || allocation->Engine!=e) { status=STATUS_INVALID_PARAMETER;goto unlock; }
        if(allocation->Opens) { status=STATUS_DEVICE_BUSY;goto unlock; }
        for(j=0;j<i;++j) if(a->pAllocationList[j]==allocation) { status=STATUS_INVALID_PARAMETER;goto unlock; }
        if(resource && allocation->Resource==resource) ++resource_children;
    }
    if(resource && resource->Allocations!=resource_children) { status=STATUS_DEVICE_BUSY;goto unlock; }
    ++e->Creating; /* release callbacks/metadata frees must finish before stop */
    for(i=0;i<a->NumAllocations;++i) {
        allocation=a->pAllocationList[i];
        if(allocation->Resource) --allocation->Resource->Allocations;
        if(!(allocation->Desc.Buffer.flags&TG_GPU_ALLOCATION_DISPLAY_PRIMARY))
            e->AllocationBytes-=allocation->Bytes;
        --e->AllocationCount;
    }
    if(resource) --e->ResourceCount;
unlock:
    KeReleaseSpinLock(&e->Lock,irql);
    if(!NT_SUCCESS(status)) return status;
    for(i=0;i<a->NumAllocations;++i) ExFreePoolWithTag(a->pAllocationList[i],TAG);
    if(resource) ExFreePoolWithTag(resource,TAG);
    KeAcquireSpinLock(&e->Lock,&irql);--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return STATUS_SUCCESS;
}
static void close_one(TG_GPU_CONTEXT_ENGINE *e,TG_GPU_OPEN_ALLOCATION *open)
{
    KIRQL irql;
    KeAcquireSpinLock(&e->Lock,&irql);
    --open->Allocation->Opens;--open->Device->OpenAllocations;
    KeReleaseSpinLock(&e->Lock,irql);
    ExFreePoolWithTag(open,TAG);
}
NTSTATUS APIENTRY TgGpuDdiOpenAllocation(HANDLE device,const DXGKARG_OPENALLOCATION *a)
{
    TG_GPU_DEVICE *d=device;TG_GPU_CONTEXT_ENGINE *e;TG_GPU_OPEN_ALLOCATION *open;
    TG_GPU_ALLOCATION *allocation;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;UINT i,created=0;
    struct tg_gpu_resource_request metadata={0};TG_GPU_RESOURCE *resource=NULL;
    DXGKARGCB_GETHANDLEDATA query={.Type=DXGK_HANDLE_ALLOCATION};
    if(!d || !a || !a->NumAllocations || !a->pOpenAllocation ||
        a->Flags.Reserved) return STATUS_INVALID_PARAMETER;
    e=d->Engine;
    if(!e->Dxgk.DxgkCbAcquireHandleData || !e->Dxgk.DxgkCbReleaseHandleData)
        return STATUS_INVALID_DEVICE_STATE;
    for(i=0;i<a->NumAllocations;++i) a->pOpenAllocation[i].hDeviceSpecificAllocation=NULL;
    if(a->PrivateDriverSize) {
        status=resource_request(a->pPrivateDriverData,a->PrivateDriverSize,&metadata);
        if(!NT_SUCCESS(status))return status;
        if(a->NumAllocations>metadata.allocation_count)return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping) { KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_NOT_READY; }
    ++d->OpenAllocations;++e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    for(i=0;i<a->NumAllocations;++i) {
        DXGKARGCB_RELEASEHANDLEDATA release={.Type=DXGK_HANDLE_ALLOCATION};
        open=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*open),TAG);
        if(!open) { status=STATUS_NO_MEMORY;break; }
        query.hObject=a->pOpenAllocation[i].hAllocation;
        allocation=e->Dxgk.DxgkCbAcquireHandleData(&query,&release.ReleaseHandle);
        BOOLEAN valid=allocation && release.ReleaseHandle && allocation->Engine==e;
        if(valid) {
            TG_GPU_RESOURCE *group=allocation->Resource;
            if(group && group->FixedAllocations) {
                /* Dxgkrnl forwards the original resource-level data here.
                 * All opens in this call must refer to that allocation set.
                 * The renderer owns interpretation of its trailing payload.
                 */
                valid=a->PrivateDriverSize && (!resource || resource==group) &&
                    metadata.allocation_count==group->Metadata.allocation_count &&
                    metadata.allocation_bytes==group->Metadata.allocation_bytes &&
                    metadata.driver_data_bytes==group->Metadata.driver_data_bytes;
                resource=group;
            } else {
                valid=!a->PrivateDriverSize;
            }
        }
        if(valid) {
            open->Device=d;open->Allocation=allocation;open->ReadOnly=a->Flags.ReadOnly;
            KeAcquireSpinLock(&e->Lock,&irql);++allocation->Opens;++d->OpenAllocations;KeReleaseSpinLock(&e->Lock,irql);
            a->pOpenAllocation[i].hDeviceSpecificAllocation=open;++created;
        } else {
            ExFreePoolWithTag(open,TAG);
            status=STATUS_INVALID_PARAMETER;
        }
        /* The lookup reference is scoped to this DDI. Dxgkrnl owns the
         * binding and calls CloseAllocation before destroying its object. */
        if(release.ReleaseHandle) e->Dxgk.DxgkCbReleaseHandleData(release);
        if(!NT_SUCCESS(status))break;
    }
    if(!NT_SUCCESS(status)) for(i=0;i<created;++i) {
        close_one(e,a->pOpenAllocation[i].hDeviceSpecificAllocation);
        a->pOpenAllocation[i].hDeviceSpecificAllocation=NULL;
    }
    KeAcquireSpinLock(&e->Lock,&irql);--d->OpenAllocations;--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return status;
}
NTSTATUS APIENTRY TgGpuDdiCloseAllocation(HANDLE device,const DXGKARG_CLOSEALLOCATION *a)
{
    TG_GPU_DEVICE *d=device;TG_GPU_CONTEXT_ENGINE *e;TG_GPU_OPEN_ALLOCATION *open;
    UINT i,j;KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    if(!d || !a || (a->NumAllocations && !a->pOpenHandleList)) return STATUS_INVALID_PARAMETER;
    e=d->Engine;KeAcquireSpinLock(&e->Lock,&irql);
    for(i=0;i<a->NumAllocations;++i) {
        open=a->pOpenHandleList[i];
        if(!open || open->Device!=d || !open->Allocation || open->Allocation->Engine!=e) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        for(j=0;j<i;++j) if(a->pOpenHandleList[j]==open) { status=STATUS_INVALID_PARAMETER;break; }
        if(!NT_SUCCESS(status)) break;
    }
    if(NT_SUCCESS(status)) { ++d->OpenAllocations;++e->Creating; }
    KeReleaseSpinLock(&e->Lock,irql);
    if(!NT_SUCCESS(status)) return status;
    for(i=0;i<a->NumAllocations;++i) close_one(e,a->pOpenHandleList[i]);
    KeAcquireSpinLock(&e->Lock,&irql);--d->OpenAllocations;--e->Creating;KeReleaseSpinLock(&e->Lock,irql);
    return STATUS_SUCCESS;
}
static NTSTATUS validate_open_access(TG_GPU_DEVICE *device, const TG_GPU_OPEN_ALLOCATION *open, BOOLEAN write)
{
    if (open->Device != device || !open->Allocation || open->Allocation->Engine != device->Engine)
        return STATUS_INVALID_PARAMETER;
    if (write && open->ReadOnly)
        return STATUS_ACCESS_DENIED;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuValidateOpenAllocationRange(TG_GPU_DEVICE *d,HANDLE handle,
                                         UINT64 offset,UINT64 bytes,BOOLEAN write)
{
    TG_GPU_OPEN_ALLOCATION *open=handle;NTSTATUS status=STATUS_SUCCESS;KIRQL irql;
    if(!d || !open || !bytes) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&d->Engine->Lock,&irql);
    status=validate_open_access(d,open,write);
    if(NT_SUCCESS(status) && (offset>open->Allocation->Bytes || bytes>open->Allocation->Bytes-offset))
        status=STATUS_INVALID_BUFFER_SIZE;
    KeReleaseSpinLock(&d->Engine->Lock,irql);return status;
}
void TgGpuRegisterAllocationDdis(DRIVER_INITIALIZATION_DATA *d)
{
    d->DxgkDdiCreateAllocation=TgGpuDdiCreateAllocation;
    d->DxgkDdiDescribeAllocation=describe_allocation;
    d->DxgkDdiDestroyAllocation=TgGpuDdiDestroyAllocation;
    d->DxgkDdiOpenAllocation=TgGpuDdiOpenAllocation;
    d->DxgkDdiCloseAllocation=TgGpuDdiCloseAllocation;
}
/* A copy may use a window-sized linear RGB allocation. Direct scanout has
 * its own stricter format, orientation and extent validation below. */
static NTSTATUS copy_surface_layout(const TG_GPU_ALLOCATION *allocation,
                                    TG_GPU_PRIMARY_ALLOCATION_REQUEST *desc)
{
    const TG_GPU_ALLOCATION_DESC *layout = &allocation->Desc;
    if (layout->Buffer.flags & TG_GPU_ALLOCATION_LINEAR_SURFACE) {
        const TG_GPU_LINEAR_ALLOCATION_REQUEST *linear = &layout->Linear;
        if (linear->Buffer.flags != (TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_LINEAR_SURFACE))
            return STATUS_NOT_SUPPORTED;
        if (linear->Buffer.bytes > allocation->Bytes ||
            (UINT64)linear->Pitch * linear->Height > linear->Buffer.bytes ||
            (UINT64)linear->Width * sizeof(UINT32) > linear->Pitch)
            return STATUS_INVALID_BUFFER_SIZE;
        *desc = (TG_GPU_PRIMARY_ALLOCATION_REQUEST) {
            .Buffer = linear->Buffer, .Format = linear->Format,
            .Rotation = D3DDDI_ROTATION_IDENTITY, .Width = linear->Width,
            .Height = linear->Height, .Pitch = linear->Pitch
        };
    } else {
        if (allocation->Bytes < T214_GPU_POST_FB_SIZE)
            return STATUS_INVALID_BUFFER_SIZE;
        if (layout->Buffer.bytes != T214_GPU_POST_FB_SIZE)
            return STATUS_NOT_SUPPORTED;
        *desc = layout->Primary;
    }
    if (desc->Format != D3DDDIFMT_X8R8G8B8 && desc->Format != D3DDDIFMT_A8R8G8B8)
        return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

NTSTATUS TgGpuGetPresentSurfaces(TG_GPU_DEVICE *device,HANDLE source,HANDLE destination,
    TG_GPU_PRIMARY_ALLOCATION_REQUEST *source_layout,TG_GPU_PRIMARY_ALLOCATION_REQUEST *destination_layout)
{
    const TG_GPU_OPEN_ALLOCATION *src = source, *dst = destination;
    if (!device || !src || !dst || !source_layout || !destination_layout)
        return STATUS_INVALID_PARAMETER;
    KIRQL irql;
    KeAcquireSpinLock(&device->Engine->Lock, &irql);
    NTSTATUS status = validate_open_access(device, src, FALSE);
    if (NT_SUCCESS(status))
        status = validate_open_access(device, dst, TRUE);
    /* Two device handles and disjoint VAs can still refer to the same backing. */
    if (NT_SUCCESS(status) && src->Allocation == dst->Allocation)
        status = STATUS_NOT_SUPPORTED;
    if (NT_SUCCESS(status))
        status = copy_surface_layout(src->Allocation, source_layout);
    if (NT_SUCCESS(status))
        status = copy_surface_layout(dst->Allocation, destination_layout);
    KeReleaseSpinLock(&device->Engine->Lock, irql);
    return status;
}
NTSTATUS TgGpuValidatePagingAllocation(TG_GPU_CONTEXT_ENGINE *e,HANDLE handle,UINT64 offset,UINT64 bytes)
{
    TG_GPU_ALLOCATION *a=handle;NTSTATUS status=STATUS_SUCCESS;KIRQL irql;
    if(!e || !a)return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(a->Engine!=e)status=STATUS_INVALID_PARAMETER;
    else if(offset>a->Bytes || bytes>a->Bytes-offset)status=STATUS_INVALID_BUFFER_SIZE;
    KeReleaseSpinLock(&e->Lock,irql);return status;
}

NTSTATUS TgGpuGetScanoutSurface(TG_GPU_CONTEXT_ENGINE *engine,HANDLE handle,
    TG_GPU_PRIMARY_ALLOCATION_REQUEST *desc)
{
    const TG_GPU_ALLOCATION *allocation=handle;
    if(!engine || !allocation || !desc || allocation->Engine!=engine)
        return STATUS_INVALID_PARAMETER;
    const TG_GPU_PRIMARY_ALLOCATION_REQUEST *primary=&allocation->Desc.Primary;
    if(primary->Buffer.flags!=(TG_GPU_ALLOCATION_CPU_VISIBLE|TG_GPU_ALLOCATION_DISPLAY_PRIMARY) ||
        primary->Buffer.bytes!=T214_GPU_POST_FB_SIZE ||
        (primary->Format!=D3DDDIFMT_X8R8G8B8 && primary->Format!=D3DDDIFMT_A8R8G8B8))
        return STATUS_NOT_SUPPORTED;
    /* No engine spin lock here: this immutable object is retained by the OS,
     * and the fast flip DDI can run above DISPATCH_LEVEL. */
    *desc=*primary;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY TgGpuDdiGetStandardAllocationDriverData(HANDLE context,
    DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *args)
{
    TG_GPU_ADAPTER *adapter = context;
    PAGED_CODE();
    if (!adapter || !args || args->PhysicalAdapterIndex)
        return STATUS_INVALID_PARAMETER;
    if (args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE) {
        /* A windowed blit may stage the back buffer for a CPU read. WDDM
         * fixes the staging format to X8R8G8B8 and supplies its dimensions. */
        args->AllocationPrivateDriverDataSize = sizeof(TG_GPU_LINEAR_ALLOCATION_REQUEST);
        args->ResourcePrivateDriverDataSize = 0;
        if (!args->pAllocationPrivateDriverData)
            return STATUS_SUCCESS;
        D3DKMDT_STAGINGSURFACEDATA *surface = args->pCreateStagingSurfaceData;
        if (!surface)
            return STATUS_INVALID_PARAMETER;
        TG_GPU_LINEAR_ALLOCATION_REQUEST desc = {
            .Buffer = {TG_GPU_ALLOCATION_ABI, TG_GPU_ALLOCATION_CPU_VISIBLE | TG_GPU_ALLOCATION_LINEAR_SURFACE, 0},
            .Format = D3DDDIFMT_X8R8G8B8, .Width = surface->Width, .Height = surface->Height,
        };
        NTSTATUS status = linear_layout(desc.Width, desc.Height, desc.Format, &desc.Pitch, &desc.Buffer.bytes);
        if (!NT_SUCCESS(status))
            return status;
        RtlCopyMemory(args->pAllocationPrivateDriverData, &desc, sizeof(desc));
        surface->Pitch = desc.Pitch;
        return STATUS_SUCCESS;
    }
    if(args->StandardAllocationType==D3DKMDT_STANDARDALLOCATION_GDISURFACE) {
        D3DKMDT_GDISURFACEDATA *gdi=args->pCreateGdiSurfaceData;
        if(!gdi || gdi->Flags.Value)return STATUS_INVALID_PARAMETER;
        if(gdi->Type!=D3DKMDT_GDISURFACE_TEXTURE_CROSSADAPTER &&
           gdi->Type!=D3DKMDT_GDISURFACE_TEXTURE_CPUVISIBLE_CROSSADAPTER)
            return STATUS_NOT_SUPPORTED;
        TG_GPU_LINEAR_ALLOCATION_REQUEST desc={
            .Buffer={TG_GPU_ALLOCATION_ABI,TG_GPU_ALLOCATION_CPU_VISIBLE,0},
            .Format=gdi->Format,.Width=gdi->Width,.Height=gdi->Height,
        };
        NTSTATUS status=linear_layout(gdi->Width,gdi->Height,gdi->Format,&desc.Pitch,&desc.Buffer.bytes);
        if(!NT_SUCCESS(status))return status;
        const UINT bytes=gdi->Format==D3DDDIFMT_UNKNOWN?sizeof(desc.Buffer):sizeof(desc);
        if(gdi->Format!=D3DDDIFMT_UNKNOWN)desc.Buffer.flags|=TG_GPU_ALLOCATION_LINEAR_SURFACE;
        gdi->Pitch=desc.Pitch;
        args->AllocationPrivateDriverDataSize=bytes;
        args->ResourcePrivateDriverDataSize=0;
        if(args->pAllocationPrivateDriverData)RtlCopyMemory(args->pAllocationPrivateDriverData,&desc,bytes);
        return STATUS_SUCCESS;
    }
    if (!adapter->Panel.Dc.owned)
        return STATUS_DEVICE_NOT_READY;
    const BOOLEAN primary = args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE;
    const BOOLEAN shadow = args->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE;
    if (!primary && !shadow)
        return STATUS_NOT_SUPPORTED;
    if ((primary && (!args->pCreateSharedPrimarySurfaceData ||
                     args->pCreateSharedPrimarySurfaceData->VidPnSourceId != 0)) ||
        (shadow && !args->pCreateShadowSurfaceData))
        return STATUS_INVALID_PARAMETER;
    const UINT width = primary ? args->pCreateSharedPrimarySurfaceData->Width : args->pCreateShadowSurfaceData->Width;
    const UINT height = primary ? args->pCreateSharedPrimarySurfaceData->Height : args->pCreateShadowSurfaceData->Height;
    const D3DDDIFORMAT format = primary ? args->pCreateSharedPrimarySurfaceData->Format : args->pCreateShadowSurfaceData->Format;
    if (!((width == T214_DC_PANEL_WIDTH && height == T214_DC_PANEL_HEIGHT) ||
          (shadow && width == T214_DC_PANEL_HEIGHT && height == T214_DC_PANEL_WIDTH)) ||
        (format != D3DDDIFMT_X8R8G8B8 && format != D3DDDIFMT_A8R8G8B8))
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    TG_GPU_PRIMARY_ALLOCATION_REQUEST request = {
        .Buffer = {
            TG_GPU_ALLOCATION_ABI,
            TG_GPU_ALLOCATION_CPU_VISIBLE | (primary ? TG_GPU_ALLOCATION_DISPLAY_PRIMARY : 0),
            T214_GPU_POST_FB_SIZE,
        },
        .Format = format,
        .Rotation = D3DDDI_ROTATION_IDENTITY,
        .Width = width, .Height = height, .Pitch = width * sizeof(UINT32),
    };
    if (primary) {
        const UINT rotation=adapter->Panel.PathRotation;
        request.ModeRotation=rotation>=D3DKMDT_VPPR_IDENTITY && rotation<=D3DKMDT_VPPR_ROTATE270?
            rotation:D3DDDI_ROTATION_IDENTITY;
        request.RefreshRate.Numerator = args->pCreateSharedPrimarySurfaceData->RefreshRate.Numerator;
        request.RefreshRate.Denominator = args->pCreateSharedPrimarySurfaceData->RefreshRate.Denominator;
    } else
        args->pCreateShadowSurfaceData->Pitch = width * sizeof(UINT32);
    args->AllocationPrivateDriverDataSize = sizeof(request);
    args->ResourcePrivateDriverDataSize = 0;
    if (args->pAllocationPrivateDriverData)
        RtlCopyMemory(args->pAllocationPrivateDriverData, &request, sizeof(request));
    return STATUS_SUCCESS;
}
