/* SPDX-License-Identifier: MIT
 * Synchronous, adapter-owned H.264 transport. HardwareAccess escapes use
 * WDDM level-two serialization; DispatchGate also excludes the private GPU
 * worker, DVFS and MC reset paths. No video capability is advertised here.
 */
#include "adapter.h"
#include "nvdec_control.h"
#include "nvdec_escape_abi.h"

#define TAG 0x4164564eu
#define VIDEO_CHANNEL 11u
#define VIDEO_SYNCPOINT 191u
#define CHANNEL_BASE (VIDEO_CHANNEL * 0x4000u)
#define SYNCPOINT_OFFSET (0x2100u + 0xf80u + 4u * VIDEO_SYNCPOINT)
#define TG_NVDEC_SHUTDOWN_WAIT_100NS (-10000000LL) /* one second, relative */
static DXGKDDI_ESCAPE *previous_escape;

static void lock(KMUTEX *gate)
{ (VOID)KeWaitForSingleObject(gate,Executive,KernelMode,FALSE,NULL); }
static NTSTATUS lock_gate(KMUTEX *gate,BOOLEAN bounded)
{
    LARGE_INTEGER timeout={.QuadPart=TG_NVDEC_SHUTDOWN_WAIT_100NS};
    NTSTATUS status=KeWaitForSingleObject(gate,Executive,KernelMode,FALSE,bounded?&timeout:NULL);
    return status==STATUS_SUCCESS?STATUS_SUCCESS:(NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status);
}
static void unlock(KMUTEX *gate) { (VOID)KeReleaseMutex(gate,FALSE); }

/* Translate only assigned ranges. Never manufacture an MMIO mapping from a
 * physical address, even for a read-only preflight. */
static volatile ULONG *register_address(TG_GPU_ADAPTER *a,
    enum tg_nvdec_aperture ap, UINT32 off)
{
    TG_GPU_RESOURCES *r=&a->Resources;
    UINT32 phys;
    static const UINT32 bases[TgNvdecApertureCount]={
#define VIDEO_BASE(name, base, size, writable) base,
        T214_NVDEC_RESOURCES(VIDEO_BASE)
#undef VIDEO_BASE
    };
    static const UINT32 sizes[TgNvdecApertureCount]={
#define VIDEO_SIZE(name, base, size, writable) size,
        T214_NVDEC_RESOURCES(VIDEO_SIZE)
#undef VIDEO_SIZE
    };
    if (off&3u) goto invalid;
    switch (ap) {
    case TG_NVDEC_AP_NVDEC:
        if (off>=0x40000u) goto invalid;
        phys=0x54480000u+off; break;
    case TG_NVDEC_AP_HOST1X:
        if (off>=0x40000u) goto invalid;
        phys=0x50000000u+off; break;
    case TG_NVDEC_AP_PMC:
        if (off>=0x1000u) goto invalid;
        phys=0x7000e400u+off; break;
    case TG_NVDEC_AP_CAR:
        if (off>=0x1000u) goto invalid;
        phys=0x60006000u+off; break;
    case TG_NVDEC_AP_MC:
        if ((off==0x970u || off==0x974u) && r->Mapping[TgGpuMcFlush])
            return (volatile ULONG *)((PUCHAR)r->Mapping[TgGpuMcFlush]+off-0x970u);
        if (off>=0x1000u) goto invalid;
        phys=0x70019000u+off; break;
    default: goto invalid;
    }
    for (UINT i=0;i<TgNvdecApertureCount;++i)
        if (phys>=bases[i] && phys-bases[i]<=sizes[i]-sizeof(ULONG) && r->NvdecMapping[i])
            return (volatile ULONG *)((PUCHAR)r->NvdecMapping[i]+phys-bases[i]);
invalid:
    a->Video.IoStatus=STATUS_DEVICE_CONFIGURATION_ERROR;
    return NULL;
}
static uint32_t read32(void *ctx, enum tg_nvdec_aperture ap, uint32_t off)
{
    volatile ULONG *p=register_address(ctx,ap,off);
    return p ? READ_REGISTER_ULONG(p) : MAXULONG;
}
static void write32(void *ctx, enum tg_nvdec_aperture ap, uint32_t off, uint32_t value)
{
    TG_GPU_ADAPTER *a=ctx;
    BOOLEAN allowed=FALSE;
    if (ap==TG_NVDEC_AP_NVDEC) allowed=TRUE;
    else if (ap==TG_NVDEC_AP_CAR) {
        allowed=off==0x698u || ((off==0x29cu || off==0x2a0u || off==0x2a8u || off==0x2acu) && value==(1u<<2));
    } else if (ap==TG_NVDEC_AP_MC && off==0x970u) {
        ULONG old=read32(ctx,ap,off);
        allowed=old!=MAXULONG && !((old^value)&~(1u<<5));
    } else if (ap==TG_NVDEC_AP_HOST1X) {
        if (off==0x21acu) {
            ULONG old=read32(ctx,ap,off);
            allowed=old!=MAXULONG && !((old^value)&~(1u<<VIDEO_CHANNEL));
        } else if (off==0x21b0u) allowed=value==(1u<<VIDEO_CHANNEL);
        else allowed=off==CHANNEL_BASE+0x14u || off==CHANNEL_BASE+0x18u ||
            off==CHANNEL_BASE+0x20u || off==CHANNEL_BASE+0x24u || off==CHANNEL_BASE+0x98u;
    }
    volatile ULONG *p=allowed ? register_address(a,ap,off) : NULL;
    if (p) { KeMemoryBarrier(); WRITE_REGISTER_ULONG(p,value); }
    else a->Video.IoStatus=STATUS_ACCESS_DENIED;
}
static void delay_us(void *ctx, uint32_t us)
{
    UNREFERENCED_PARAMETER(ctx);
    while (us) {
        ULONG step=us>50u?50u:us;
        KeStallExecutionProcessor(step); us-=step;
    }
}
static bool clock_live(void *ctx)
{
    TG_GPU_ADAPTER *a=ctx;
    return !InterlockedCompareExchange(&a->Video.Closing,0,0) &&
        a->Video.IoStatus==STATUS_SUCCESS && TgHost1xClockHeld(a->Video.Clock,NULL,NULL) &&
        read32(a,TG_NVDEC_AP_CAR,0x180u)==a->Video.HostSource;
}
static bool clock_acquire(void *ctx) { return clock_live(ctx); }
static NTSTATUS clock_quiesce(PVOID ctx)
{
    TG_GPU_ADAPTER *a=ctx;
    InterlockedExchange(&a->Video.Closing,1);
    /* No Video.Gate here: release may hold it while waiting for this callback.
     * Terminal shutdown bounds every DispatchGate acquisition to avoid hanging
     * behind a wedged GPU worker. */
    NTSTATUS gate=lock_gate(&a->Queue.DispatchGate,a->Video.ShutdownBounded || a->ShutdownFinal);
    if (gate!=STATUS_SUCCESS) return gate;
    NTSTATUS status=TgNvdecQuiesce(a->Video.Backend);
    unlock(&a->Queue.DispatchGate);
    return status;
}
static bool clock_release(void *ctx)
{
    TG_GPU_ADAPTER *a=ctx;
    /* The backend has already proved quiescence. Keep its object and the
     * lifecycle gate, but let the provider's passive callback acquire MC. */
    a->Video.DispatchGateHeld=FALSE;
    unlock(&a->Queue.DispatchGate);
    NTSTATUS status=TgHost1xClockRelease(&a->Video.Clock);
    NTSTATUS relock=lock_gate(&a->Queue.DispatchGate,a->Video.ShutdownBounded);
    if (relock!=STATUS_SUCCESS) return false;
    a->Video.DispatchGateHeld=TRUE;
    return status==STATUS_SUCCESS;
}
static NTSTATUS close_locked(TG_GPU_ADAPTER *a)
{
    TG_NVDEC_ADAPTER *v=&a->Video;
    InterlockedExchange(&v->Closing,1);
    NTSTATUS status=TgNvdecDestroy(v->Backend);
    if (status!=STATUS_SUCCESS) return status;
    v->Backend=NULL;
    if (v->Clock && !clock_release(a)) return STATUS_DEVICE_BUSY;
    if (v->Device) TgGpuDereferenceVideoDevice(v->Device);
    v->Device=NULL; v->Session=0;
    return STATUS_SUCCESS;
}
static NTSTATUS close_device(PVOID owner, TG_GPU_DEVICE *device)
{
    TG_GPU_ADAPTER *a=owner;
    NTSTATUS status=STATUS_SUCCESS;
    lock(&a->Video.Gate); lock(&a->Queue.DispatchGate);
    a->Video.ShutdownBounded=FALSE; a->Video.DispatchGateHeld=TRUE;
    if (a->Video.Device==device) status=close_locked(a);
    if (a->Video.DispatchGateHeld) { a->Video.DispatchGateHeld=FALSE; unlock(&a->Queue.DispatchGate); }
    unlock(&a->Video.Gate);
    return status;
}
void TgNvdecAdapterInitialize(TG_GPU_ADAPTER *a)
{
    if (!a->Video.Initialized) {
        KeInitializeMutex(&a->Video.Gate,0);
        a->Video.Initialized=TRUE;
    }
    a->Engine.VideoOwner=a;
    a->Engine.VideoCloseDevice=close_device;
}
static NTSTATUS adapter_stop(TG_GPU_ADAPTER *a,BOOLEAN shutdown)
{
    NTSTATUS status;
    PAGED_CODE();
    if (!a || !a->Video.Initialized) return STATUS_SUCCESS;
    status=lock_gate(&a->Video.Gate,shutdown);
    if (status!=STATUS_SUCCESS) return status;
    status=lock_gate(&a->Queue.DispatchGate,shutdown);
    if (status!=STATUS_SUCCESS) {
        unlock(&a->Video.Gate);
        return status;
    }
    a->Video.ShutdownBounded=shutdown;
    a->Video.DispatchGateHeld=TRUE;
    status=close_locked(a);
    a->Video.ShutdownBounded=FALSE;
    if (a->Video.DispatchGateHeld) {
        a->Video.DispatchGateHeld=FALSE;
        unlock(&a->Queue.DispatchGate);
    }
    unlock(&a->Video.Gate);
    return status;
}
NTSTATUS TgNvdecAdapterStop(TG_GPU_ADAPTER *a)
{
    return adapter_stop(a,FALSE);
}
NTSTATUS TgNvdecAdapterShutdown(TG_GPU_ADAPTER *a)
{
    return adapter_stop(a,TRUE);
}

/* This miniport owns the assigned Host1x aperture and exposes one decoder.
 * Refuse active DMA/FIFO state, display syncpoints and an existing threshold IRQ
 * owner. A stable counter is only a preflight, never completion evidence. */
static BOOLEAN host_reject(TG_GPU_ADAPTER *a, ULONG check, ULONG index,
                           ULONG v0, ULONG v1, ULONG v2, ULONG v3)
{
    a->Video.HostCheck=check; a->Video.HostIndex=index;
    a->Video.HostValue[0]=v0; a->Video.HostValue[1]=v1;
    a->Video.HostValue[2]=v2; a->Video.HostValue[3]=v3;
    a->Video.HostValue[4]=a->Video.HostValue[5]=0;
    if (check==TgNvdecHostChannelFifo || check==TgNvdecHostChannelPointers ||
        check==TgNvdecHostChannelDmaState) {
        /* Read-only inherited ring evidence. The ownership guard never
         * resets a channel or treats these values as completed work. */
        ULONG base=index*0x4000u;
        a->Video.HostValue[4]=read32(a,TG_NVDEC_AP_HOST1X,base+0x14u);
        a->Video.HostValue[5]=read32(a,TG_NVDEC_AP_HOST1X,base+0x20u);
    }
    return FALSE;
}
static BOOLEAN host_idle(TG_GPU_ADAPTER *a)
{
    if (!a->Resources.DisplayRegisters)
        return host_reject(a,TgNvdecHostNoDisplay,0,0,0,0,0);
    ULONG dc=READ_REGISTER_ULONG((PULONG)((PUCHAR)a->Resources.DisplayRegisters+4u*0x028u));
    if (dc==MAXULONG || ((dc&(1u<<8)) && (dc&255u)==VIDEO_SYNCPOINT))
        return host_reject(a,TgNvdecHostDisplaySyncpoint,0x028u,dc,0,0,0);
    /* This adapter has device-exclusive PnP ownership and reserves channel11
     * for its single decoder under Video.Gate/DispatchGate. No software queue
     * exists at create time. Require STOP and an empty FIFO; the backend also
     * requires reset/gated video engines before it can initialize a new ring.
     * Inherited GET/PUT equality is not the fresh-channel ownership contract.
     * Other channels and display syncpoints are never initialized here. */
    {
        const ULONG channel=VIDEO_CHANNEL;
        ULONG off=channel*0x4000u, status=read32(a,TG_NVDEC_AP_HOST1X,off);
        if (status==MAXULONG || !(status&(1u<<11))) {
            /* Read-only evidence on rejection; do not reset a channel or
             * change its admission predicate to manufacture ownership. */
            ULONG put=read32(a,TG_NVDEC_AP_HOST1X,off+0x18u);
            ULONG get=read32(a,TG_NVDEC_AP_HOST1X,off+0x1cu);
            ULONG ctrl=read32(a,TG_NVDEC_AP_HOST1X,off+0x24u);
            return host_reject(a,TgNvdecHostChannelFifo,channel,status,put,get,ctrl);
        }
        ULONG put=read32(a,TG_NVDEC_AP_HOST1X,off+0x18u);
        ULONG get=read32(a,TG_NVDEC_AP_HOST1X,off+0x1cu);
        ULONG ctrl=read32(a,TG_NVDEC_AP_HOST1X,off+0x24u);
        if (ctrl==MAXULONG || (ctrl&7u)!=1u)
            return host_reject(a,TgNvdecHostChannelDmaState,channel,status,put,get,ctrl);
    }
    ULONG irq=read32(a,TG_NVDEC_AP_HOST1X,0x2100u+0xf20u+4u*(VIDEO_SYNCPOINT/32u));
    if (irq&(1u<<(VIDEO_SYNCPOINT%32u)))
        return host_reject(a,TgNvdecHostSyncpointIrq,VIDEO_SYNCPOINT,irq,0,0,0);
    ULONG first=read32(a,TG_NVDEC_AP_HOST1X,SYNCPOINT_OFFSET);
    if (first==MAXULONG)
        return host_reject(a,TgNvdecHostSyncpointValue,VIDEO_SYNCPOINT,first,0,0,0);
    LARGE_INTEGER wait; wait.QuadPart=-500000; /* 50ms, passive wait */
    (VOID)KeDelayExecutionThread(KernelMode,FALSE,&wait);
    ULONG last=read32(a,TG_NVDEC_AP_HOST1X,SYNCPOINT_OFFSET);
    if (first!=last)
        return host_reject(a,TgNvdecHostSyncpointChanged,VIDEO_SYNCPOINT,first,last,0,0);
    if (!clock_live(a))
        return host_reject(a,TgNvdecHostClockChanged,0x180u,
            read32(a,TG_NVDEC_AP_CAR,0x180u),a->Video.IoStatus,a->Video.Closing,0);
    return TRUE;
}
static NTSTATUS create_locked(TG_GPU_ADAPTER *a, TG_GPU_DEVICE *device)
{
    TG_NVDEC_ADAPTER *v=&a->Video;
    NTSTATUS status;
    if (v->Device || v->Backend || v->Clock) return STATUS_DEVICE_BUSY;
    v->Operation="assigned resources";
    v->HostSource=v->PllpBase=v->CoreBootFloorUv=0;
    for (UINT i=0;i<TgNvdecApertureCount;++i)
        if (!a->Resources.NvdecMapping[i]) return STATUS_DEVICE_NOT_READY;
    if (!a->Resources.Mapping[TgGpuHidrev] || !a->Resources.Mapping[TgGpuMcFlush]) return STATUS_DEVICE_NOT_READY;
    struct tg_nvdec_platform_io io={a,read32,write32,delay_us};
    v->Operation="live SMMU route";
    if (!tg_nvdec_platform_direct_dma(&io)) return STATUS_NOT_SUPPORTED;
    status=TgGpuReferenceVideoDevice(&a->Engine,device,&v->Device);
    if (status!=STATUS_SUCCESS) return status;
    if (!++v->Generation) ++v->Generation;
    v->Session=v->Generation; v->IoStatus=STATUS_SUCCESS;
    InterlockedExchange(&v->Closing,0);
    v->Operation="Host1x clock lease";
    status=TgHost1xClockAcquire(&a->Platform,clock_quiesce,a,&v->Clock,&v->Operation);
    if (status==STATUS_SUCCESS) {
        ULONG pllp=0;
        v->HostSource=read32(a,TG_NVDEC_AP_CAR,0x180u);
        v->Operation="inherited clock route";
        BOOLEAN live=TgHost1xClockHeld(v->Clock,&pllp,&v->CoreBootFloorUv);
        v->PllpBase=pllp;
        if (!live || !tg_nvdec_platform_clocks_supported(pllp,v->HostSource,v->CoreBootFloorUv)) status=STATUS_DEVICE_NOT_READY;
        else {
            v->Operation="Host1x ownership";
            if (!host_idle(a)) status=STATUS_DEVICE_NOT_READY;
        }
        if (status==STATUS_SUCCESS) {
            v->Operation="B01 backend initialize";
            struct tg_nvdec_platform_owner owner={a,
                READ_REGISTER_ULONG((PULONG)a->Resources.Mapping[TgGpuHidrev]),
                pllp,clock_acquire,clock_release,clock_live};
            status=TgNvdecCreate(&a->Engine.Dxgk,&io,&owner,VIDEO_CHANNEL,VIDEO_SYNCPOINT,&v->Backend);
        }
    }
    if (status!=STATUS_SUCCESS) {
        NTSTATUS cleanup=close_locked(a);
        if (cleanup!=STATUS_SUCCESS) return cleanup;
    }
    return status;
}
static void error_text(TG_GPU_ADAPTER *a, char *out)
{
    UINT n=0;
    if (a->Video.HostCheck) {
        const char prefix[]="Host1x";
        for (UINT i=0;i<sizeof(prefix)-1;++i) out[n++]=prefix[i];
        const ULONG values[]={a->Video.HostCheck,a->Video.HostIndex,
            a->Video.HostValue[0],a->Video.HostValue[1],a->Video.HostValue[2],a->Video.HostValue[3],
            a->Video.HostValue[4],a->Video.HostValue[5],
            a->Video.PllpBase,a->Video.HostSource,a->Video.CoreBootFloorUv};
        const char *labels[]={" check="," index="," a="," b="," c="," d="," e="," f="," pll="," src="," uv="};
        for (UINT i=0;i<RTL_NUMBER_OF(values);++i) {
            for (UINT j=0;labels[i][j] && n<TG_NVDEC_ERROR_SIZE-1u;++j) out[n++]=labels[i][j];
            for (int shift=28;shift>=0 && n<TG_NVDEC_ERROR_SIZE-1u;shift-=4)
                out[n++]="0123456789abcdef"[(values[i]>>shift)&15u];
        }
        out[n]=0; return;
    }
    const char *parts[]={a->Video.Operation?a->Video.Operation:"no session",
        ": ",TgNvdecLastError(a->Video.Backend),"; PLLP=0x"};
    for (UINT p=0;p<RTL_NUMBER_OF(parts);++p)
        for (UINT i=0;parts[p][i] && n<104u;++i) out[n++]=parts[p][i];
    ULONG values[]={a->Video.PllpBase,a->Video.HostSource,a->Video.CoreBootFloorUv};
    const char *labels[]={""," HostSource=0x"," CoreFloorUv=0x"};
    for (UINT i=0;i<RTL_NUMBER_OF(values);++i) {
        for (UINT j=0;labels[i][j] && n<TG_NVDEC_ERROR_SIZE-1u;++j) out[n++]=labels[i][j];
        for (int shift=28;shift>=0 && n<TG_NVDEC_ERROR_SIZE-1u;shift-=4)
            out[n++]="0123456789abcdef"[(values[i]>>shift)&15u];
    }
    out[n]=0;
}
static NTSTATUS execute(TG_GPU_ADAPTER *a, TG_GPU_DEVICE *device,
    struct tg_nvdec_escape *h, const void *in, void *out)
{
    TG_NVDEC_ADAPTER *v=&a->Video;
    NTSTATUS status;
    v->HostCheck=0; /* A new request must not report an earlier rejection. */
    if (h->command==TG_NVDEC_ESCAPE_LOAD) {
        if (h->session || h->input_bytes<sizeof(struct tg_nvdec_load_request) ||
            h->output_bytes!=sizeof(struct tg_nvdec_load_result)) return STATUS_INVALID_PARAMETER;
        const struct tg_nvdec_load_request *q=in;
        struct tg_nvdec_load_result *r=out;
        r->version=TG_NVDEC_CONTROL_VERSION;
        if (q->version!=TG_NVDEC_CONTROL_VERSION || q->size!=128000u ||
            h->input_bytes!=sizeof(*q)+q->size || !TgNvdecFirmwareValid(q+1,q->size,q->sha256))
            status=STATUS_INVALID_PARAMETER;
        else {
            status=create_locked(a,device);
            if (status==STATUS_SUCCESS) {
                v->Operation="Falcon firmware boot";
                status=TgNvdecLoadFirmware(v->Backend,q+1,q->size,q->sha256);
            }
            h->session=v->Device==device?v->Session:0;
            if (status==STATUS_SUCCESS) {
                r->firmware_bytes=q->size; r->booted=TgNvdecFirmwareReady(v->Backend);
                RtlCopyMemory(r->sha256,q->sha256,sizeof(r->sha256));
            }
        }
        r->result=(UINT32)status; error_text(a,r->error);
        return status;
    }
    if (v->Device!=device || !h->session || h->session!=v->Session) return STATUS_INVALID_HANDLE;
    switch (h->command) {
    case TG_NVDEC_ESCAPE_CLOSE:
        if (h->input_bytes || h->output_bytes) return STATUS_INVALID_PARAMETER;
        v->Operation="session retirement";
        status=close_locked(a);
        if (status==STATUS_SUCCESS) h->session=0;
        return status;
    case TG_NVDEC_ESCAPE_STATUS: {
        if (h->input_bytes || h->output_bytes!=sizeof(struct tg_nvdec_status_result)) return STATUS_INVALID_PARAMETER;
        struct tg_nvdec_status_result *r=out;
        r->version=TG_NVDEC_CONTROL_VERSION; r->created=v->Backend!=NULL;
        r->firmware_ready=TgNvdecFirmwareReady(v->Backend);
        r->surfaces_ready=TgNvdecSurfacesReady(v->Backend);
        TgNvdecGetConfig(v->Backend,&r->width,&r->height,&r->pitch,&r->aligned_height,&r->frame_size);
        error_text(a,r->error);
        return STATUS_SUCCESS;
    }
    case TG_NVDEC_ESCAPE_CONFIGURE: {
        if (h->input_bytes!=sizeof(struct tg_nvdec_config_request) ||
            h->output_bytes!=sizeof(struct tg_nvdec_config_result)) return STATUS_INVALID_PARAMETER;
        const struct tg_nvdec_config_request *q=in;
        struct tg_nvdec_config_result *r=out;
        v->Operation="surface configuration";
        r->version=TG_NVDEC_CONTROL_VERSION;
        status=q->version!=TG_NVDEC_CONTROL_VERSION ? STATUS_INVALID_PARAMETER :
            !clock_live(a)?STATUS_DEVICE_NOT_READY:
            TgNvdecConfigure(v->Backend,q->width,q->height,&r->pitch,&r->aligned_height,&r->frame_size);
        r->result=(UINT32)status; error_text(a,r->error);
        return status;
    }
    case TG_NVDEC_ESCAPE_DECODE: {
        if (h->input_bytes<sizeof(struct tg_nvdec_decode_request) ||
            h->output_bytes<sizeof(struct tg_nvdec_decode_result)) return STATUS_INVALID_PARAMETER;
        const struct tg_nvdec_decode_request *q=in;
        struct tg_nvdec_decode_result *r=out;
        r->version=TG_NVDEC_CONTROL_VERSION;
        if (q->version!=TG_NVDEC_CONTROL_VERSION || !q->bitstream_size ||
            !q->slice_count || q->slice_count>TG_NVDEC_MAX_SLICES ||
            sizeof(*q)+(UINT64)q->bitstream_size+4ull*(q->slice_count+1u)!=h->input_bytes ||
            sizeof(*r)+(UINT64)q->output_size!=h->output_bytes) return STATUS_INVALID_PARAMETER;
        TgNvdecGetConfig(v->Backend,&r->width,&r->height,&r->pitch,&r->aligned_height,&r->frame_size);
        if (!r->frame_size || q->output_size!=r->frame_size) return STATUS_INVALID_PARAMETER;
        /* The packed bitstream need not end on a UINT32 boundary. Copy its
         * offset table into aligned storage before invoking the codec. */
        UINT32 offsets[TG_NVDEC_MAX_SLICES+1u];
        const UCHAR *bitstream=(const UCHAR *)(q+1);
        RtlCopyMemory(offsets,bitstream+q->bitstream_size,4u*(q->slice_count+1u));
        struct tg_nvdec_status st={0};
        v->Operation="H.264 submission";
        status=!clock_live(a)?STATUS_DEVICE_NOT_READY:
            TgNvdecDecode(v->Backend,&q->pic,bitstream,q->bitstream_size,offsets,q->slice_count,
                r+1,q->output_size,&st,&r->expected_syncpoint,&r->observed_syncpoint);
        r->result=(UINT32)status;
        r->mbs_correctly_decoded=st.mbs_correctly_decoded; r->mbs_in_error=st.mbs_in_error;
        r->cycles=st.cycles; r->error_status=st.error_status; r->slice_header_error_code=st.slice_header_error_code;
        if (status==STATUS_SUCCESS) r->bytes_written=r->frame_size;
        error_text(a,r->error);
        return status;
    }
    default: return STATUS_NOT_SUPPORTED;
    }
}
static NTSTATUS APIENTRY escape(HANDLE adapter, const DXGKARG_ESCAPE *args)
{
    PAGED_CODE();
    if (!adapter || !args || !args->pPrivateDriverData || args->PrivateDriverDataSize<sizeof(UINT32))
        return STATUS_INVALID_PARAMETER;
    UINT32 magic;
    RtlCopyMemory(&magic,args->pPrivateDriverData,sizeof(magic));
    if (magic!=TG_NVDEC_ESCAPE_MAGIC)
        return previous_escape ? previous_escape(adapter,args) : STATUS_NOT_SUPPORTED;
    if (args->Flags.Value!=1u || !args->hDevice || args->hContext ||
        args->PrivateDriverDataSize<sizeof(struct tg_nvdec_escape)) return STATUS_INVALID_PARAMETER;
    struct tg_nvdec_escape h;
    RtlCopyMemory(&h,args->pPrivateDriverData,sizeof(h));
    UINT32 payload=h.input_bytes>h.output_bytes?h.input_bytes:h.output_bytes;
    if (h.version!=TG_NVDEC_ESCAPE_VERSION || h.reserved || h.result ||
        h.size!=args->PrivateDriverDataSize || payload>TG_NVDEC_ESCAPE_MAX_PAYLOAD ||
        h.size!=sizeof(h)+payload) return STATUS_INVALID_PARAMETER;
    TG_GPU_ADAPTER *a=adapter;
    if (!a->Video.Initialized || !a->EngineReady || !a->QueueReady) return STATUS_DEVICE_NOT_READY;
    TG_GPU_DEVICE *device=NULL;
    NTSTATUS status=TgGpuReferenceVideoDevice(&a->Engine,args->hDevice,&device);
    if (status!=STATUS_SUCCESS) return status;
    void *input=NULL, *output=NULL;
    if (h.input_bytes) input=ExAllocatePool2(POOL_FLAG_PAGED,h.input_bytes,TAG);
    if (h.output_bytes) output=ExAllocatePool2(POOL_FLAG_PAGED,h.output_bytes,TAG);
    if ((h.input_bytes && !input) || (h.output_bytes && !output)) status=STATUS_NO_MEMORY;
    else {
        if (input) RtlCopyMemory(input,(const UCHAR *)args->pPrivateDriverData+sizeof(h),h.input_bytes);
        lock(&a->Video.Gate); lock(&a->Queue.DispatchGate);
        a->Video.ShutdownBounded=FALSE; a->Video.DispatchGateHeld=TRUE;
        if (h.command!=TG_NVDEC_ESCAPE_CLOSE && h.command!=TG_NVDEC_ESCAPE_STATUS &&
            (a->State!=TgGpuAdapterReady || a->Engine.Stopping || a->Engine.Resetting ||
             a->Engine.RecoveryPending || a->ShutdownFinal))
            h.result=STATUS_DEVICE_NOT_READY;
        else h.result=execute(a,device,&h,input,output);
        if (h.result==STATUS_SUCCESS && a->Video.IoStatus!=STATUS_SUCCESS && h.command!=TG_NVDEC_ESCAPE_CLOSE)
            h.result=a->Video.IoStatus;
        if (h.command==TG_NVDEC_ESCAPE_DECODE && output && h.output_bytes>=sizeof(struct tg_nvdec_decode_result)) {
            struct tg_nvdec_decode_result *r=output;
            r->result=(UINT32)h.result;
            if (h.result!=STATUS_SUCCESS) {
                r->bytes_written=0;
                RtlZeroMemory(r+1,h.output_bytes-sizeof(*r));
            }
        }
        if (a->Video.DispatchGateHeld) { a->Video.DispatchGateHeld=FALSE; unlock(&a->Queue.DispatchGate); }
        unlock(&a->Video.Gate);
        RtlCopyMemory(args->pPrivateDriverData,&h,sizeof(h));
        if (output) RtlCopyMemory((UCHAR *)args->pPrivateDriverData+sizeof(h),output,h.output_bytes);
        status=STATUS_SUCCESS; /* delivery only; caller must inspect h.result */
    }
    if (input) ExFreePoolWithTag(input,TAG);
    if (output) ExFreePoolWithTag(output,TAG);
    TgGpuDereferenceVideoDevice(device);
    return status;
}
void TgNvdecRegisterEscape(DRIVER_INITIALIZATION_DATA *d)
{
    previous_escape=d->DxgkDdiEscape;
    d->DxgkDdiEscape=escape;
}
