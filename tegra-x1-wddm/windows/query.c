/* SPDX-License-Identifier: MIT */
#include "query.h"
#include "work_queue.h"

static const DXGK_WDDMVERSION wddm_version=DXGKDDI_WDDMv2_6;

static NTSTATUS buffers(const DXGKARG_QUERYADAPTERINFO *a, UINT input, UINT output)
{
    if (a->InputDataSize < input || a->OutputDataSize < output)
        return STATUS_BUFFER_TOO_SMALL;
    if ((input && !a->pInputData) || (output && !a->pOutputData))
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

static NTSTATUS query_adapter_info(HANDLE adapter, const DXGKARG_QUERYADAPTERINFO *a, BOOLEAN display)
{
    TG_GPU_CONTEXT_ENGINE *e=adapter;
    DXGK_SEGMENTDESCRIPTOR3 table;
    NTSTATUS status;
    if (!e || !a) return STATUS_INVALID_PARAMETER;
    if (a->Flags.Value) return STATUS_NOT_SUPPORTED; /* no GPU virtualization */
    if (a->Type==DXGKQAITYPE_WDDMDEVICECAPS) {
        /* DxgKrnl asks before StartDevice has installed callbacks or tables. */
        const DXGK_WDDMDEVICECAPS caps={.WDDMVersion=wddm_version};
        /* The version reply is fixed; an output-only query needs no input. */
        status=buffers(a,a->InputDataSize ? sizeof(DXGK_WDDMDEVICECAPSIN) : 0,sizeof(caps));
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(a->pOutputData,&caps,sizeof(caps));
        return STATUS_SUCCESS;
    }
    if (a->Type==DXGKQAITYPE_GPUVERSION) {
        /* GM20B is integrated and has no separate GPU VBIOS. */
        const DXGK_GPUVERSION version={.GpuArchitecture=L"Maxwell GM20B"};
        UINT index;
        status=buffers(a,sizeof(index),sizeof(version));
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(&index,a->pInputData,sizeof(index));
        if (index) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(a->pOutputData,&version,sizeof(version));
        return STATUS_SUCCESS;
    }
    switch (a->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS caps={0};
        status=buffers(a,0,sizeof(caps));
        if (!NT_SUCCESS(status)) return status;
        caps.HighestAcceptableAddress.QuadPart=T214_SYSTEM_LIMIT-1;
        caps.InterruptMessageNumber=0; /* line-based */
        caps.WDDMVersion=wddm_version;
        caps.MiscCaps.ComputeOnly=!display;
        caps.FlipCaps.FlipOnVSyncMmIo=display;
        caps.SupportNonVGA=display; /* The panel DDIs release POST ownership. */
        caps.SupportSmoothRotation=display;
        /* MPO3 also carries the ordinary primary on current Windows. That
         * entry point does not imply support for extra overlay planes:
         * SupportMultiPlaneOverlay and MaxOverlayPlanes remain zero. */
        /* QUERYSEGMENT4 exposes only a noncoherent paging aperture. */
        caps.PresentationCaps.NoCacheCoherentApertureMemory=display;
        /* The fixed-panel Present path copies between shadow and primary;
         * it does not implement copies within the same surface. */
        caps.PresentationCaps.NoScreenToScreenBlt=display;
        caps.PresentationCaps.NoOverlapScreenBlt=display;
        caps.GpuEngineTopology.NbAsymetricProcessingNodes=1;
        caps.SchedulingCaps.MultiEngineAware=1;
        caps.SchedulingCaps.PreemptionAware=1;
        caps.SchedulingCaps.LowIrqlPreemptCommand=1;
        caps.MemoryManagementCaps.VirtualAddressingSupported=1;
        caps.MemoryManagementCaps.GpuMmuSupported=1;
        /* Linear system-memory resources support copy; no cross-adapter
         * texturing or scanout tier is declared. */
        caps.MemoryManagementCaps.CrossAdapterResource=1;
        caps.PreemptionCaps.GraphicsPreemptionGranularity=D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps.PreemptionCaps.ComputePreemptionGranularity=D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        /* The single GR/CE runlist escalates active engine timeouts to the
         * adapter-wide reset rather than discarding unrelated state. */
        caps.SupportPerEngineTDR=TRUE;
        RtlCopyMemory(a->pOutputData,&caps,sizeof(caps));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION: {
        const DXGK_DISPLAY_DRIVERCAPS_EXTENSION caps={0};
        if (!display) return STATUS_NOT_SUPPORTED;
        status=buffers(a,0,sizeof(caps));
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(a->pOutputData,&caps,sizeof(caps));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_UMDRIVERPRIVATE:
    case DXGKQAITYPE_QUERYSEGMENT4:
    case DXGKQAITYPE_GPUMMUCAPS:
    case DXGKQAITYPE_PAGETABLELEVELDESC:
    case DXGKQAITYPE_PHYSICALADAPTERCAPS:
    case DXGKQAITYPE_NUMPOWERCOMPONENTS:
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    /* Segment 1 backs page tables. Segment 2 gives VidMm a window into
     * system memory for its DMA pool. */
    if (e->Stopping || !e->Dxgk.DeviceHandle || !e->Tables || e->Tables->SegmentId!=1)
        return STATUS_DEVICE_NOT_READY;
    status=TgGpuDescribeTableSegment(e->Tables,&table);
    if (!NT_SUCCESS(status)) return status;

    switch (a->Type) {
    case DXGKQAITYPE_NUMPOWERCOMPONENTS: {
        const UINT components=0;
        status=buffers(a,0,sizeof(components));
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(a->pOutputData,&components,sizeof(components));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        /* Paging-only mode does not produce any GPU history timestamps. */
        const DXGKARG_HISTORYBUFFERPRECISION precision={0};
        status=buffers(a,0,sizeof(precision));
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(a->pOutputData,&precision,sizeof(precision));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_UMDRIVERPRIVATE: {
        struct tg_gpu_adapter_info info={.version=TG_GPU_ADAPTER_ABI,.size=sizeof(info),
            .memory_budget_bytes=TG_GPU_MEMORY_BUDGET_BYTES,.vendor=0x10de,
            .chipset=0x12e,.sm=53,.gr_class=0xb197};
        /* The public KMT call can reach the miniport with output only. */
        status=buffers(a,a->InputDataSize?sizeof(info):0,sizeof(info));
        if(!NT_SUCCESS(status))return status;
        if(a->InputDataSize) {
            struct tg_gpu_adapter_query request;
            /* Snapshot a supplied header before writing a possibly aliased reply. */
            RtlCopyMemory(&request,a->pInputData,sizeof(request));
            if(request.version!=TG_GPU_ADAPTER_ABI || request.size!=sizeof(info))
                return STATUS_GRAPHICS_DRIVER_MISMATCH;
        }
        if(e->AdapterIdentityReady) {
            info.flags=TG_GPU_ADAPTER_LUID_VALID;
            info.luid_low=e->AdapterLuid.LowPart;info.luid_high=e->AdapterLuid.HighPart;
            if(!e->PagingOnly)info.flags|=TG_GPU_ADAPTER_NATIVE_SUBMIT;
        }
        RtlCopyMemory(a->pOutputData,&info,sizeof(info));
        if(a->OutputDataSize>=sizeof(struct tg_gpu_adapter_nvdec_info)) {
            struct tg_nvdec_resource_info nvdec=e->NvdecInfo;
            /* Older start/test paths do not populate the optional snapshot. */
            nvdec.version=TG_NVDEC_QUERY_ABI;nvdec.size=sizeof(nvdec);
            RtlCopyMemory((UCHAR *)a->pOutputData+sizeof(info),&nvdec,sizeof(nvdec));
        }
        if(a->OutputDataSize>=sizeof(struct tg_gpu_adapter_nvdec_clients_info)) {
            struct tg_nvdec_client_info clients=e->NvdecClients;
            clients.version=TG_NVDEC_CLIENT_QUERY_ABI;clients.size=sizeof(clients);
            RtlCopyMemory((UCHAR *)a->pOutputData+sizeof(struct tg_gpu_adapter_nvdec_info),
                          &clients,sizeof(clients));
        }
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT4: {
        const DXGK_QUERYSEGMENTIN4 *in=a->pInputData;
        DXGK_QUERYSEGMENTOUT4 *out=a->pOutputData;
        DXGK_SEGMENTDESCRIPTOR4 desc={0},aperture={0};
        status=buffers(a,sizeof(*in),sizeof(*out));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        const UINT segments=e->Tables->Scanout.Handle ? TG_GPU_SCANOUT_SEGMENT :
            TG_GPU_PAGING_APERTURE_SEGMENT;
        /* WDDM's first call permits accessing only NbSegment. The other
         * members are not valid inputs until the second call.
         */
        if (!out->NbSegment) { out->NbSegment=segments;return STATUS_SUCCESS; }
        if (out->NbSegment!=segments || !out->pSegmentDescriptor ||
            out->SegmentDescriptorStride<sizeof(desc) ||
            out->SegmentDescriptorStride>SIZE_MAX/segments) return STATUS_INVALID_PARAMETER;
        desc.Flags=table.Flags;
        desc.BaseAddress=table.BaseAddress;
        desc.Size=table.Size;
        desc.CommitLimit=table.CommitLimit;
        desc.SystemMemoryEndAddress=table.SystemMemoryEndAddress;
        desc.CpuTranslatedAddress=table.CpuTranslatedAddress;
        /* Only our known descriptor bytes belong to this WDDM 2.6 driver;
         * preserve a larger OS stride and its extension bytes.
         */
        RtlCopyMemory(out->pSegmentDescriptor,&desc,sizeof(desc));
        aperture.Flags.Aperture=1;
        aperture.Size=aperture.CommitLimit=TG_GPU_PAGING_APERTURE_BYTES;
        RtlCopyMemory(out->pSegmentDescriptor+out->SegmentDescriptorStride,
                      &aperture,sizeof(aperture));
        if (segments==TG_GPU_SCANOUT_SEGMENT) {
            status=TgGpuDescribeScanoutSegment(e->Tables,&desc);
            if (!NT_SUCCESS(status)) return status;
            RtlCopyMemory(out->pSegmentDescriptor+2*out->SegmentDescriptorStride,
                          &desc,sizeof(desc));
        }
        out->PagingBufferSegmentId=TG_GPU_PAGING_APERTURE_SEGMENT;
        out->PagingBufferSize=TG_GPU_DMA_BUFFER_BYTES;
        out->PagingBufferPrivateDataSize=TG_GPU_PAGING_PRIVATE_BYTES;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_GPUMMUCAPS: {
        const DXGK_QUERYGPUMMUCAPSIN *in=a->pInputData;
        DXGK_GPUMMUCAPS caps={0};
        status=buffers(a,sizeof(*in),sizeof(caps));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        caps.ReadOnlyMemorySupported=1;
        caps.PageTableUpdateRequireAddressSpaceIdle=1;
        caps.PageTableUpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
        caps.VirtualAddressBitCount=TG_GPU_DIRECTORY_SHIFT+TG_GPU_TABLE_BITS;
        caps.PageTableLevelCount=2;
        /* No coherent, zero, NX, large or dual-PTE support. Initial CPU
         * updates remain a separate validated path in BuildPagingBuffer.
         */
        RtlCopyMemory(a->pOutputData,&caps,sizeof(caps));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PAGETABLELEVELDESC: {
        const DXGK_QUERYPAGETABLELEVELDESCIN *in=a->pInputData;
        DXGK_PAGE_TABLE_LEVEL_DESC desc;
        status=buffers(a,sizeof(*in),sizeof(desc));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        status=TgGpuDescribePageTable(in->LevelIndex,e->Tables->SegmentId,&desc);
        if (!NT_SUCCESS(status)) return status;
        RtlCopyMemory(a->pOutputData,&desc,sizeof(desc));
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_PHYSICALADAPTERCAPS: {
        const DXGK_QUERYPHYSICALADAPTERCAPSIN *in=a->pInputData;
        DXGK_PHYSICALADAPTERCAPS caps={0};
        status=buffers(a,sizeof(*in),sizeof(caps));
        if (!NT_SUCCESS(status)) return status;
        if (in->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        caps.NumExecutionNodes=1;
        caps.PagingNodeIndex=0; /* paging shares the ordered execution node */
        caps.DxgkPhysicalAdapterHandle=e->Dxgk.DeviceHandle;
        caps.Flags.GpuMmuSupported=1;
        RtlCopyMemory(a->pOutputData,&caps,sizeof(caps));
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* DriverEntry selects the display entry together with the panel DDIs.
 * This decision is valid from AddDevice, before VidMm owns any segments. */
NTSTATUS APIENTRY TgGpuDdiQueryAdapterInfo(HANDLE adapter, const DXGKARG_QUERYADAPTERINFO *a)
{
    return query_adapter_info(adapter,a,FALSE);
}

NTSTATUS APIENTRY TgGpuDdiQueryDisplayAdapterInfo(HANDLE adapter, const DXGKARG_QUERYADAPTERINFO *a)
{
    return query_adapter_info(adapter,a,TRUE);
}

void TgGpuRegisterQueryDdi(DRIVER_INITIALIZATION_DATA *d)
{ d->DxgkDdiQueryAdapterInfo=TgGpuDdiQueryAdapterInfo; }
