/* SPDX-License-Identifier: MIT */
#include "command_snapshot.h"
#define TAG 0x53674754u
NTSTATUS TgGpuCaptureCommandSnapshot(const DXGKARG_SUBMITCOMMANDVIRTUAL *a,
    const struct tg_gpu_command_va *va,TG_GPU_COMMAND_SNAPSHOT **out)
{
    struct tg_gpu_submit_request request;
    struct tg_gpu_submit_indirect indirect={0};
    TG_GPU_COMMAND_SNAPSHOT *s;
    UINT header_bytes=sizeof(request);
    BOOLEAN has_indirect=FALSE;
    PAGED_CODE();
    if(!out)return STATUS_INVALID_PARAMETER;
    *out=NULL;
    if(!a || !a->pDmaBufferPrivateData || a->DmaBufferUmdPrivateDataSize<sizeof(request) ||
       a->DmaBufferUmdPrivateDataSize>a->DmaBufferPrivateDataSize ||
       !a->DmaBufferSize || a->DmaBufferSize>TG_GPU_SUBMIT_MAX_BYTES || (a->DmaBufferSize&3))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(&request,a->pDmaBufferPrivateData,sizeof(request));
    if(request.version==TG_GPU_SUBMIT_ABI_INDIRECT) {
        header_bytes+=sizeof(indirect);
        has_indirect=TRUE;
    } else if(request.version!=TG_GPU_SUBMIT_ABI)
        return STATUS_INVALID_PARAMETER;
    if(request.reserved || request.command_bytes!=a->DmaBufferSize ||
       a->DmaBufferUmdPrivateDataSize<header_bytes ||
       request.size!=header_bytes+request.command_bytes ||
       request.size!=a->DmaBufferUmdPrivateDataSize)
        return STATUS_INVALID_PARAMETER;
    if(has_indirect)
        RtlCopyMemory(&indirect,(const UCHAR *)a->pDmaBufferPrivateData+sizeof(request),sizeof(indirect));
    s=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*s)+request.command_bytes,TAG);
    if(!s)return STATUS_NO_MEMORY;
    RtlZeroMemory(s,sizeof(*s));
    s->Bytes=request.command_bytes;
    s->HasIndirect=has_indirect;
    s->Indirect=indirect;
    RtlCopyMemory(s->Words,(const UCHAR *)a->pDmaBufferPrivateData+header_bytes,s->Bytes);
    if(!(has_indirect ?
         tg_gpu_prepare_indirect_command_stream(s->Words,s->Bytes,&s->Indirect,va,&s->Info) :
         tg_gpu_prepare_command_stream(s->Words,s->Bytes,va,&s->Info))) {
        ExFreePoolWithTag(s,TAG);return STATUS_INVALID_PARAMETER;
    }
    *out=s;return STATUS_SUCCESS;
}
VOID TgGpuFreeCommandSnapshot(TG_GPU_COMMAND_SNAPSHOT *s)
{if(s)ExFreePoolWithTag(s,TAG);}
