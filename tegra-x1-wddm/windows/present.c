/* SPDX-License-Identifier: MIT
 * Fixed-panel GDI shadow copies. B0B5 pitch-linear encoding follows NVK's
 * copy engine; VidMm owns the virtual mappings and allocation residency.
 */
#include "present.h"
#include "allocation.h"
#include "adapter.h"
#include "work_queue.h"
#include "../core/present_blit.h"
#include "../platform/t214_resources.h"
#include "../platform/t214_dc.h"

#define PRESENT_MAGIC 0x50724754u
#define SNAPSHOT_TAG 0x53674754u
typedef struct {
    UINT Magic,Bytes;
    TG_GPU_CONTEXT *Context;
    PVOID CpuAddress;
    UINT64 GpuAddress;
} PRESENT_BUFFER;
C_ASSERT(sizeof(PRESENT_BUFFER)<=sizeof(TG_GPU_QUEUE_ENTRY));

static struct tg_gpu_blit_rect blit_rect(RECT r)
{
    return (struct tg_gpu_blit_rect){r.left,r.top,r.right,r.bottom};
}
static BOOLEAN surface_va(TG_GPU_CONTEXT *c,UINT64 va,UINT64 bytes)
{
    const UINT64 internal=c->Device->Process->Internal.Base;
    return va && !(va&3) && va<TG_GPU_VA_LIMIT && bytes && bytes<=TG_GPU_VA_LIMIT-va &&
        (va+bytes<=internal || va>=internal+TG_GPU_INTERNAL_COVERAGE);
}

NTSTATUS APIENTRY TgGpuDdiPresent(HANDLE context,DXGKARG_PRESENT *a)
{
    TG_GPU_CONTEXT *c=context;
    PAGED_CODE();
    if(!c || !a)return STATUS_INVALID_PARAMETER;
    /* NoWait keeps the GPU pipeline running while the scheduler performs
     * the VSync MMIO flip. This no-DMA path emits no GPU wait commands. */
    /* A flip binds already oriented storage. Rotate is a Blt operation and
     * must execute the copy below, not turn into a no-DMA success receipt. */
    const DXGK_PRESENTFLAGS flips={.Flip=1,.FlipWithNoWait=1,
        .FlipWithMultiPlaneOverlay=1,.RedirectedFlip=1};
    if(a->Flags.Flip && !(a->Flags.Value&~flips.Value) && !a->pDmaBuffer && !a->MultipassOffset)
        return STATUS_SUCCESS; /* Panel DDIs perform the scheduled MMIO flip. */
    const DXGK_PRESENTFLAGS blt={.Blt=1,.Rotate=1};
    if(!a->Flags.Blt || (a->Flags.Value&~blt.Value) || !c->Flags.GdiContext || !c->Flags.VirtualAddressing ||
       !c->Device->Process)return STATUS_NOT_SUPPORTED;
    if(!a->pAllocationInfo || !a->pDmaBuffer || !a->pDmaBufferPrivateData ||
       ((ULONG_PTR)a->pDmaBuffer&3) || !a->DmaBufferGpuVirtualAddress ||
       (a->SubRectCnt && !a->pDstSubRects))
        return STATUS_INVALID_PARAMETER;
    if(a->DmaBufferPrivateDataSize<sizeof(TG_GPU_QUEUE_ENTRY))
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    /* Virtual contexts use the 32-byte present records, not the legacy
     * 24-byte patch allocation list. See SHADOW_PRESENT.md for the checked
     * WDK layout and the installed Dxgkrnl producer. */
    const DXGK_PRESENTALLOCATIONINFO *src=&a->pAllocationInfo[DXGK_PRESENT_SOURCE_INDEX];
    const DXGK_PRESENTALLOCATIONINFO *dst=&a->pAllocationInfo[DXGK_PRESENT_DESTINATION_INDEX];
    TG_GPU_PRIMARY_ALLOCATION_REQUEST source,destination;
    NTSTATUS status=TgGpuGetPresentSurface(c->Device,src->hDeviceSpecificAllocation,FALSE,&source);
    if(!NT_SUCCESS(status))return status;
    status=TgGpuGetPresentSurface(c->Device,dst->hDeviceSpecificAllocation,TRUE,&destination);
    if(!NT_SUCCESS(status))return status;
    UINT rotation=0;
    if(a->Flags.Rotate) {
        /* WDDM serializes the mode change against this source's presents.
         * Do not infer rotation merely from mismatching surface dimensions. */
        const TG_GPU_ADAPTER *adapter=(const TG_GPU_ADAPTER *)c->Device->Engine;
        switch(adapter->Panel.PathRotation) {
        case D3DKMDT_VPPR_IDENTITY: break;
        case D3DKMDT_VPPR_ROTATE90: rotation=90;break;
        case D3DKMDT_VPPR_ROTATE180: rotation=180;break;
        case D3DKMDT_VPPR_ROTATE270: rotation=270;break;
        default: return STATUS_NOT_SUPPORTED;
        }
    }
    struct tg_gpu_present_blit plan;
    if(!tg_gpu_present_blit_init(&plan,
        (struct tg_gpu_blit_surface){source.Width,source.Height,source.Pitch},
        (struct tg_gpu_blit_surface){destination.Width,destination.Height,destination.Pitch},
        blit_rect(a->SrcRect),blit_rect(a->DstRect),rotation))
        return STATUS_INVALID_PARAMETER;
    if(src->PhysicalAdapterIndex || dst->PhysicalAdapterIndex ||
       !surface_va(c,src->AllocationVirtualAddress,source.Buffer.bytes) ||
       !surface_va(c,dst->AllocationVirtualAddress,destination.Buffer.bytes))return STATUS_INVALID_PARAMETER;
    const BOOLEAN opaque=source.Format==D3DDDIFMT_X8R8G8B8 && destination.Format==D3DDDIFMT_A8R8G8B8;
    const UINT packet_words=rotation==180?TG_GPU_PRESENT_HALFTURN_WORDS:(opaque?17:13);
    /* Screen-to-screen overlap is not advertised. Shadow/primary ranges are
     * distinct; reject overlap before emitting any commands. */
    if(src->AllocationVirtualAddress<dst->AllocationVirtualAddress+destination.Buffer.bytes &&
       dst->AllocationVirtualAddress<src->AllocationVirtualAddress+source.Buffer.bytes)return STATUS_NOT_SUPPORTED;
    const UINT rectangles=a->SubRectCnt?a->SubRectCnt:1;
    UINT64 total=0;
    for(UINT i=0;i<rectangles;++i) {
        const RECT r=a->SubRectCnt?a->pDstSubRects[i]:a->DstRect;
        const UINT stripes=tg_gpu_present_blit_stripes(&plan,blit_rect(r));
        if(!stripes || total+stripes>UINT32_MAX)return STATUS_INVALID_PARAMETER;
        total+=stripes;
    }
    if(a->MultipassOffset>=total)return STATUS_INVALID_PARAMETER;
    const UINT bytes=a->DmaSize<TG_GPU_COMMAND_CAPTURE_BYTES?a->DmaSize:TG_GPU_COMMAND_CAPTURE_BYTES;
    UINT *begin=a->pDmaBuffer,*p=begin,available=bytes/sizeof(UINT);
    UINT skip=a->MultipassOffset,next=a->MultipassOffset;
    for(UINT i=0;i<rectangles;++i) {
        const struct tg_gpu_blit_rect r=blit_rect(a->SubRectCnt?a->pDstSubRects[i]:a->DstRect);
        const UINT stripes=tg_gpu_present_blit_stripes(&plan,r);
        if(skip>=stripes){skip-=stripes;continue;}
        for(UINT stripe=skip;stripe<stripes;++stripe) {
            if(available<packet_words)goto encoded;
            UINT words;
            if(rotation==180) {
                struct tg_gpu_present_halfturn half;
                if(!tg_gpu_present_blit_halfturn(&plan,r,&half))return STATUS_INVALID_PARAMETER;
                words=tg_gpu_encode_present_halfturn(p,available,
                    src->AllocationVirtualAddress,dst->AllocationVirtualAddress,&half,opaque!=FALSE);
            } else {
                struct tg_gpu_present_copy copy;
                if(!tg_gpu_present_blit_copy(&plan,r,stripe,&copy))return STATUS_INVALID_PARAMETER;
                words=tg_gpu_encode_present_copy(p,available,
                    src->AllocationVirtualAddress,dst->AllocationVirtualAddress,&copy,opaque!=FALSE);
            }
            if(!words)return STATUS_INVALID_PARAMETER;
            p+=words;available-=words;++next;
        }
        skip=0;
    }
encoded:
    if(p!=begin) {
        PRESENT_BUFFER *private_data=a->pDmaBufferPrivateData;
        RtlZeroMemory(private_data,sizeof(TG_GPU_QUEUE_ENTRY));
        *private_data=(PRESENT_BUFFER){PRESENT_MAGIC,(UINT)((UCHAR *)p-(UCHAR *)begin),
            c,begin,a->DmaBufferGpuVirtualAddress};
        a->pDmaBuffer=p;
        a->pDmaBufferPrivateData=(UCHAR *)private_data+sizeof(TG_GPU_QUEUE_ENTRY);
        a->MultipassOffset=next;
    }
    return next==total?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
}

NTSTATUS TgGpuCapturePresentSnapshot(TG_GPU_CONTEXT_ENGINE *e,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *a,TG_GPU_COMMAND_SNAPSHOT **out)
{
    TG_GPU_CONTEXT_WORK work={0};
    PAGED_CODE();
    if(!out)return STATUS_INVALID_PARAMETER;
    *out=NULL;
    if(!a || a->DmaBufferUmdPrivateDataSize>a->DmaBufferPrivateDataSize || !a->pDmaBufferPrivateData ||
       a->DmaBufferPrivateDataSize!=sizeof(TG_GPU_QUEUE_ENTRY) ||
       a->DmaBufferSize>TG_GPU_COMMAND_CAPTURE_BYTES)return STATUS_INVALID_PARAMETER;
    /* Resolve the OS context through the engine list before interpreting a
     * KMD pointer. Application UMD submissions cannot use this GDI path. */
    NTSTATUS status=TgGpuCaptureContextWork(e,a,NULL,&work);
    if(!NT_SUCCESS(status))return status;
    const PRESENT_BUFFER *p=a->pDmaBufferPrivateData;
    if(!work.Context->Flags.GdiContext || p->Magic!=PRESENT_MAGIC || p->Context!=work.Context ||
       !p->CpuAddress || p->GpuAddress!=a->DmaBufferVirtualAddress || p->Bytes!=a->DmaBufferSize)
        status=STATUS_INVALID_PARAMETER;
    else {
        TG_GPU_COMMAND_SNAPSHOT *s=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*s)+p->Bytes,SNAPSHOT_TAG);
        if(!s)status=STATUS_NO_MEMORY;
        else {
            const struct tg_gpu_command_va va={
                work.Root.VaLimit,work.InternalVa,work.InternalVa+TG_GPU_INTERNAL_COVERAGE
            };
            s->Bytes=p->Bytes;
            RtlCopyMemory(s->Words,p->CpuAddress,s->Bytes);
            if (!tg_gpu_prepare_command_stream(s->Words,s->Bytes,&va,&s->Info)) {
                TgGpuFreeCommandSnapshot(s);
                status=STATUS_INVALID_PARAMETER;
            } else {
                *out=s;
            }
        }
    }
    (VOID)TgGpuCancelCapturedWork(e,&work);
    return status;
}
