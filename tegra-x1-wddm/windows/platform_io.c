/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include <initguid.h>
#include <wdmguid.h>
#include "platform_io.h"
#include "../platform/t214_resources.h"

#define TG_PLATFORM_TAG 0x50674754u
#define TG_PLATFORM_TIMEOUT_100NS (-10000000LL) /* pending IRP wait: 1 s */
#define TG_PLATFORM_READY_RETRIES 12u
#define TG_PLATFORM_READY_RETRY_US 50000u       /* 50 ms step */
#define TG_PLATFORM_PREFLIGHT_BUDGET_100NS 6000000ULL /* 600 ms total monotonic budget */

enum tg_platform_irp_owner {
    TgPlatformIrpInFlight = 0,
    TgPlatformIrpCompletedSync = 1,
    TgPlatformIrpTimedOut = 2,
};

enum tg_platform_rail_op_state {
    TgRailOpNone = 0,
    TgRailOpInFlight = 1,
    TgRailOpSucceeded = 2,
    TgRailOpFailed = 3,
};

enum tg_platform_rail_abi {
    TgRailAbiUnknown = 0,
    TgRailAbiToken,
    TgRailAbiSwBattLegacy,
};

/* Independent nonpaged refcounted rail-ownership & bridge state.
 * Async IRP completions, PnP notifications, and async reconnect/compensate work
 * items reference ONLY this object (never a bare TG_GPU_PLATFORM pointer).
 * TgGpuClosePlatform clears Owner and sets Closed under Lock before releasing
 * its own reference, so late callbacks after adapter removal can never touch
 * freed miniport adapter memory (N1). */
typedef struct _TG_PLATFORM_RAIL_STATE {
    KSPIN_LOCK Lock;
    volatile LONG RefCount;
    TG_GPU_PLATFORM *Owner;
    PFILE_OBJECT ActiveFile;
    HANDLE ActiveBridge;
    PVOID ActivePnpNotify;
    enum tg_platform_rail_abi RailAbi; /* Per open file, under Lock. */
    ULONG BridgeEpoch;
    ULONG ProviderGeneration;
    ULONG ClaimEpoch;
    ULONG ProviderToken;
    ULONG LastRailOpSeq;
    ULONG LastRailOpState;
    ULONG InFlightClaimSeq;
    UCHAR LastPmicAddress;
    BOOLEAN Closed;
    BOOLEAN QueryRemove;
    BOOLEAN ClosedGpuIsolated;
    BOOLEAN EmcRailOwner;
    BOOLEAN RailClaimed;
    BOOLEAN RailOwnershipUncertain;
    BOOLEAN NeedReclaim;
    BOOLEAN DeferReleasePending;
    BOOLEAN DeferReleaseValueValid;
    BOOLEAN CompensateQueued;
    BOOLEAN CompensateRunRelease;
    UCHAR DeferReleaseAddress;
    USHORT DeferReleaseValue;
    ULONG DeferReleaseEpoch;
    ULONG DeferReleaseToken;
    PIO_WORKITEM CompensateWorkItem;
    PFILE_OBJECT CompensateFile;
    PDEVICE_OBJECT CompensateOwner;
} TG_PLATFORM_RAIL_STATE;

typedef struct _TG_PLATFORM_OPEN_CTX {
    TG_PLATFORM_RAIL_STATE *RailState;
    PDEVICE_OBJECT OwnerDevice;
    PDRIVER_OBJECT DriverObject;
    PIO_WORKITEM WorkItem;
    PIO_WORKITEM CleanupWorkItem;
    PVOID StaleNotify;
    BOOLEAN StaleNotifyHasRef;
    HANDLE Handle;
    PFILE_OBJECT File;
    PVOID Notify;
    BOOLEAN NotifyHasRef;
    KEVENT Done;
    volatile LONG RefCount;
    volatile LONG Handoff;
    NTSTATUS Status;
} TG_PLATFORM_OPEN_CTX;

typedef struct _TG_PLATFORM_IRP_CTX {
    PIRP Irp;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT TargetDevice;
    PDEVICE_OBJECT OwnerDevice;
    PIO_WORKITEM WorkItem;
    TG_PLATFORM_RAIL_STATE *RailState;
    ULONG BridgeEpoch;
    ULONG ClaimEpoch;
    ULONG Reg;
    ULONG RailOpSeq;
    KEVENT Done;
    volatile LONG RefCount;
    volatile LONG OwnerState;
    volatile LONG DispatchState;
    volatile LONG Completed;
    volatile LONG SuppressedAfterTimeout;
    volatile LONG ClaimCounted;
    NTSTATUS Status;
    ULONG_PTR Information;
    struct tg_gpu_pmic_request Buffer;
} TG_PLATFORM_IRP_CTX;

static PDRIVER_OBJECT g_PlatformDriverObject;
static volatile LONG g_NextRailClaimEpoch;
static volatile LONG g_ProviderTokenAware;
static volatile LONG g_ProviderRailSynced;
static volatile LONG g_InFlightNewClaims;

static VOID advance_claim_epoch_floor(ULONG floor_epoch)
{
    if(!floor_epoch)return;
    for(;;) {
        const LONG cur=InterlockedCompareExchange(&g_NextRailClaimEpoch,0,0);
        if((ULONG)cur>=floor_epoch)
            return;
        if(InterlockedCompareExchange(&g_NextRailClaimEpoch,(LONG)floor_epoch,cur)==cur)
            return;
    }
}

static ULONG next_rail_claim_epoch(VOID)
{
    ULONG e=(ULONG)InterlockedIncrement(&g_NextRailClaimEpoch);
    if(!e)e=(ULONG)InterlockedIncrement(&g_NextRailClaimEpoch);
    return e;
}

/* Driver-lifetime lock-free 64-bit ledger for unresolved PMIC rail claims (A05):
 * survives repeated Close, StopDevice, RemoveDevice, and final unload failure,
 * and synchronizes across KMD image reload via SwitchFan's persistent rail state.
 * State64 layout:
 *   bits  0..31 : EpochWatermark (monotonic claim generation, NEVER reset to 0!)
 *   bit  32     : Active (unresolved release owed for EpochWatermark)
 *   bit  33     : GpuIsolated (LATEST hardware CAR_RESET_X & PMC_GPU_CLAMP proof)
 *   bit  34     : IsoSampled (hardware isolation sampled for EpochWatermark;
 *                 late CLAIM receipts must NEVER overwrite a sampled reading!)
 *   bits 40..47 : PmicAddress
 *   bits 48..63 : ProviderToken (16-bit SwitchFan token when token-aware) */
#define TG_UNRESOLVED_ACTIVE      (1ULL<<32)
#define TG_UNRESOLVED_ISOLATED    (1ULL<<33)
#define TG_UNRESOLVED_ISO_SAMPLED (1ULL<<34)
typedef struct _TG_UNRESOLVED_RAIL_LEDGER {
    volatile LONGLONG State64;
    volatile LONG RetryCount;
    volatile LONG LastStatus;
} TG_UNRESOLVED_RAIL_LEDGER;
static TG_UNRESOLVED_RAIL_LEDGER g_UnresolvedRail;

static BOOLEAN probe_resources_isolated(const TG_GPU_RESOURCES *r)
{
    if(!r || !r->Mapping[TgGpuCarX] || !r->Mapping[TgGpuClamp] ||
       r->Memory[TgGpuCarX].Type!=CmResourceTypeMemory ||
       r->Memory[TgGpuCarX].u.Memory.Length<0x10u ||
       r->Memory[TgGpuClamp].Type!=CmResourceTypeMemory ||
       r->Memory[TgGpuClamp].u.Memory.Length<4u)
        return FALSE;
    const ULONG reset=READ_REGISTER_ULONG((PULONG)((PUCHAR)r->Mapping[TgGpuCarX]+0x0cu));
    const ULONG clamp=READ_REGISTER_ULONG((PULONG)r->Mapping[TgGpuClamp]);
#if defined(_M_ARM64) || defined(_ARM64_)
    _DataSynchronizationBarrier();
#else
    KeMemoryBarrier();
#endif
    return reset!=MAXULONG && clamp!=MAXULONG &&
        (reset&(1u<<24))!=0u && (clamp&1u)!=0u;
}

static VOID record_unresolved_rail_epoch_ex(ULONG claim_epoch,ULONG provider_token,
                                            UCHAR address,BOOLEAN gpu_isolated,
                                            BOOLEAN from_closed_receipt,
                                            BOOLEAN preserve_sampled_iso,
                                            NTSTATUS status)
{
    if(!claim_epoch)claim_epoch=next_rail_claim_epoch();
    advance_claim_epoch_floor(claim_epoch);
    for(;;) {
        const LONGLONG cur=InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0);
        const BOOLEAN cur_active=((ULONGLONG)cur&TG_UNRESOLVED_ACTIVE)!=0ULL;
        const ULONG cur_epoch=(ULONG)(ULONGLONG)cur;
        const UCHAR cur_addr=(UCHAR)(((ULONGLONG)cur>>40)&0xffULL);
        const ULONG cur_tok=(ULONG)(((ULONGLONG)cur>>48)&0xffffULL);
        /* A05 late_claim: NEVER let an older claim generation overwrite or revive
         * past a newer watermark (even when cur_active==FALSE!), and never let a
         * late closed receipt revive an already-settled same generation! */
        if(cur_epoch>claim_epoch)
            return;
        if(from_closed_receipt && cur_epoch==claim_epoch && !cur_active)
            return;
        UCHAR kept_addr=address;
        ULONG kept_tok=provider_token & 0xffffu;
        /* A05 sticky & revoke: hardware/handoff samples (!preserve_sampled_iso)
         * ALWAYS overwrite with the latest reading (1 -> 0 revokes immediately),
         * while late CLAIM completion receipts (preserve_sampled_iso) NEVER
         * resurrect an already-sampled/revoked isolation bit! */
        BOOLEAN kept_iso=gpu_isolated;
        ULONGLONG iso_sampled=preserve_sampled_iso ?
            (gpu_isolated ? TG_UNRESOLVED_ISO_SAMPLED : 0ULL) :
            TG_UNRESOLVED_ISO_SAMPLED;
        if(cur_epoch==claim_epoch) {
            if(!kept_addr)kept_addr=cur_addr;
            if(!kept_tok)kept_tok=cur_tok;
            if(preserve_sampled_iso && ((ULONGLONG)cur & TG_UNRESOLVED_ISO_SAMPLED)) {
                kept_iso=((ULONGLONG)cur & TG_UNRESOLVED_ISOLATED)!=0ULL;
                iso_sampled=TG_UNRESOLVED_ISO_SAMPLED;
            }
        }
        const ULONGLONG next=(ULONGLONG)claim_epoch | TG_UNRESOLVED_ACTIVE |
            iso_sampled | (kept_iso?TG_UNRESOLVED_ISOLATED:0ULL) |
            ((ULONGLONG)kept_addr<<40) | ((ULONGLONG)kept_tok<<48);
        if(InterlockedCompareExchange64(&g_UnresolvedRail.State64,(LONGLONG)next,cur)==cur)
            break;
    }
    if(!NT_SUCCESS(status))
        InterlockedExchange(&g_UnresolvedRail.LastStatus,(LONG)status);
}

static VOID update_unresolved_rail_isolation(ULONG claim_epoch,BOOLEAN gpu_isolated)
{
    for(;;) {
        const LONGLONG cur=InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0);
        const BOOLEAN cur_active=((ULONGLONG)cur&TG_UNRESOLVED_ACTIVE)!=0ULL;
        const ULONG cur_epoch=(ULONG)(ULONGLONG)cur;
        if(!cur_active || (claim_epoch && cur_epoch!=claim_epoch))
            return;
        const ULONGLONG base=((ULONGLONG)cur | TG_UNRESOLVED_ISO_SAMPLED);
        const ULONGLONG next=gpu_isolated ?
            (base|TG_UNRESOLVED_ISOLATED) :
            (base&~TG_UNRESOLVED_ISOLATED);
        if(InterlockedCompareExchange64(&g_UnresolvedRail.State64,(LONGLONG)next,cur)==cur)
            return;
    }
}

static VOID clear_unresolved_rail_epoch(ULONG claim_epoch)
{
    if(!claim_epoch)return;
    for(;;) {
        const LONGLONG cur=InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0);
        const BOOLEAN cur_active=((ULONGLONG)cur&TG_UNRESOLVED_ACTIVE)!=0ULL;
        const ULONG cur_epoch=(ULONG)(ULONGLONG)cur;
        /* Only the matching claim generation may clear the unresolved ledger,
         * and cur_epoch is retained as the monotonic watermark (A05)! */
        if(!cur_active || cur_epoch!=claim_epoch)
            return;
        const ULONGLONG next=(ULONGLONG)cur_epoch;
        if(InterlockedCompareExchange64(&g_UnresolvedRail.State64,(LONGLONG)next,cur)==cur)
            return;
    }
}

static VOID supersede_older_unresolved_rail(ULONG active_claim_epoch)
{
    if(!active_claim_epoch)return;
    advance_claim_epoch_floor(active_claim_epoch);
    for(;;) {
        const LONGLONG cur=InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0);
        const ULONG cur_epoch=(ULONG)(ULONGLONG)cur;
        if(cur_epoch>=active_claim_epoch)
            return;
        const ULONGLONG next=(ULONGLONG)active_claim_epoch;
        if(InterlockedCompareExchange64(&g_UnresolvedRail.State64,(LONGLONG)next,cur)==cur)
            return;
    }
}

VOID TgGpuSetPlatformDriverObject(PVOID DriverObject)
{
    g_PlatformDriverObject=(PDRIVER_OBJECT)DriverObject;
    /* A05: Never wipe g_UnresolvedRail on unload (DriverObject == NULL)! */
}

static NTSTATUS send_pmic_irp(TG_PLATFORM_RAIL_STATE *state,ULONG epoch,
                              ULONG claim_epoch,ULONG rail_op_seq,
                              PFILE_OBJECT file,PDEVICE_OBJECT owner_dev,
                              struct tg_gpu_pmic_request *op,
                              BOOLEAN has_deadline,ULONGLONG deadline);

static VOID notify_provider_handoff_via_file(PFILE_OBJECT file,PDEVICE_OBJECT owner_dev,
                                             ULONG provider_token,UCHAR address,
                                             BOOLEAN active,BOOLEAN gpu_isolated)
{
    if(!file || !owner_dev || !InterlockedCompareExchange(&g_ProviderTokenAware,0,0))
        return;
    struct tg_gpu_pmic_request ho={
        .address=TG_GPU_RAIL_HANDOFF_MAGIC | (UINT32)address,
        .reg=TG_GPU_PMIC_REPORT_GPU_STATUS,
        .write=1u,
        .value=((UINT32)(provider_token&0xffffu)<<16) |
               (active?TG_GPU_RAIL_STATE_HANDOFF:0u) |
               (gpu_isolated?TG_GPU_RAIL_STATE_ISOLATED:0u),
    };
    (VOID)send_pmic_irp(NULL,0,0,0,file,owner_dev,&ho,FALSE,0);
}

static VOID notify_provider_handoff_standalone(ULONG provider_token,UCHAR address,
                                               BOOLEAN active,BOOLEAN gpu_isolated)
{
    OBJECT_ATTRIBUTES attributes;IO_STATUS_BLOCK iosb={0};
    HANDLE handle=NULL;PFILE_OBJECT file=NULL;PDEVICE_OBJECT owner_dev=NULL;
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\SwitchBacklight");
    if(!InterlockedCompareExchange(&g_ProviderTokenAware,0,0) ||
       !g_PlatformDriverObject || !g_PlatformDriverObject->DeviceObject)
        return;
    owner_dev=g_PlatformDriverObject->DeviceObject;
    InitializeObjectAttributes(&attributes,(PUNICODE_STRING)&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    if(!NT_SUCCESS(ZwCreateFile(&handle,GENERIC_READ|GENERIC_WRITE,&attributes,&iosb,NULL,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,NULL,0)))
        return;
    if(NT_SUCCESS(ObReferenceObjectByHandle(handle,FILE_READ_DATA|FILE_WRITE_DATA,*IoFileObjectType,
        KernelMode,(PVOID *)&file,NULL))) {
        ObfReferenceObject(owner_dev);
        notify_provider_handoff_via_file(file,owner_dev,provider_token,address,active,gpu_isolated);
        ObfDereferenceObject(owner_dev);
        ObfDereferenceObject(file);
    }
    (VOID)ZwClose(handle);
}

static VOID ingest_provider_rail_query(const struct tg_gpu_pmic_request *q)
{
    if(!q || !(q->address & TG_GPU_RAIL_STATE_ACK))
        return;
    InterlockedExchange(&g_ProviderTokenAware,1);
    InterlockedExchange(&g_ProviderRailSynced,1);
    const ULONG seq=(q->write>>16)&0xffffu;
    const ULONG tok=q->write&0xffffu;
    if(seq)advance_claim_epoch_floor(seq);
    if(q->address & (TG_GPU_RAIL_STATE_CLAIMED|TG_GPU_RAIL_STATE_HANDOFF)) {
        const LONGLONG cur=InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0);
        if((ULONG)(ULONGLONG)cur==0u) {
            const ULONG rec_epoch=tok ? tok : (seq ? seq : next_rail_claim_epoch());
            const BOOLEAN rec_iso=(q->address & TG_GPU_RAIL_STATE_ISOLATED)!=0u;
            const UCHAR rec_addr=(UCHAR)(q->value & 0x7fu);
            record_unresolved_rail_epoch_ex(rec_epoch,tok?tok:rec_epoch,rec_addr,
                rec_iso,FALSE,FALSE,STATUS_UNSUCCESSFUL);
        }
    }
}

/* SwBatt creates this named control device in DriverEntry and deletes it in
 * DriverUnload. It is separate from its battery PnP stack. The referenced
 * file keeps the endpoint alive until close. SwitchFan uses target PnP. */
static BOOLEAN swbatt_control_file(PFILE_OBJECT file)
{
    DECLARE_CONST_UNICODE_STRING(name,L"\\Driver\\swbatt");
    PDEVICE_OBJECT device=file->DeviceObject;
    return device && device->DeviceType==FILE_DEVICE_UNKNOWN && device->DriverObject &&
        RtlEqualUnicodeString(&device->DriverObject->DriverName,&name,TRUE);
}

/* A legacy SwBatt rejects the tagged read before touching the PMIC. Query
 * its untagged ABI on the same file within the original time budget. */
static NTSTATUS query_provider_address(TG_PLATFORM_RAIL_STATE *state,ULONG epoch,
    PFILE_OBJECT file,PDEVICE_OBJECT owner_dev,struct tg_gpu_pmic_request *q,
    BOOLEAN has_deadline,ULONGLONG deadline,BOOLEAN *used_legacy)
{
    if(used_legacy)*used_legacy=FALSE;
    NTSTATUS status=send_pmic_irp(state,epoch,0,0,file,owner_dev,q,has_deadline,deadline);
    if(status!=STATUS_ACCESS_DENIED || !swbatt_control_file(file))return status;

    struct tg_gpu_pmic_request legacy={0,TG_GPU_PMIC_QUERY_ADDRESS,0,0};
    status=send_pmic_irp(state,epoch,0,0,file,owner_dev,&legacy,has_deadline,deadline);
    if(!NT_SUCCESS(status))return status;
    if(!legacy.value || legacy.value>0x7f)return STATUS_DEVICE_PROTOCOL_ERROR;
    *q=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_QUERY_ADDRESS,0,legacy.value};
    if(used_legacy)*used_legacy=TRUE;
    return STATUS_SUCCESS;
}

/* On a freshly loaded KMD image (State64 == 0 && g_ProviderRailSynced == 0),
 * query SwitchFan once so any unresolved rail claim from an unloaded prior KMD
 * image is recovered into this image's ledger and watermark (A05 reload). */
static VOID sync_provider_rail_once(VOID)
{
    OBJECT_ATTRIBUTES attributes;IO_STATUS_BLOCK iosb={0};
    HANDLE handle=NULL;PFILE_OBJECT file=NULL;PDEVICE_OBJECT owner_dev=NULL;
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\SwitchBacklight");
    if(InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0)!=0 ||
       InterlockedCompareExchange(&g_ProviderRailSynced,0,0)!=0 ||
       !g_PlatformDriverObject || !g_PlatformDriverObject->DeviceObject)
        return;
    if(InterlockedCompareExchange(&g_ProviderRailSynced,1,0)!=0)
        return;
    owner_dev=g_PlatformDriverObject->DeviceObject;
    InitializeObjectAttributes(&attributes,(PUNICODE_STRING)&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    NTSTATUS st=ZwCreateFile(&handle,GENERIC_READ|GENERIC_WRITE,&attributes,&iosb,NULL,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,NULL,0);
    if(!NT_SUCCESS(st)) {
        InterlockedExchange(&g_ProviderRailSynced,0);
        return;
    }
    st=ObReferenceObjectByHandle(handle,FILE_READ_DATA|FILE_WRITE_DATA,*IoFileObjectType,
        KernelMode,(PVOID *)&file,NULL);
    if(NT_SUCCESS(st)) {
        ObfReferenceObject(owner_dev);
        struct tg_gpu_pmic_request q={TG_GPU_RAIL_QUERY_MAGIC,TG_GPU_PMIC_QUERY_ADDRESS,0,0};
        st=query_provider_address(NULL,0,file,owner_dev,&q,FALSE,0,NULL);
        if(NT_SUCCESS(st))
            ingest_provider_rail_query(&q);
        else
            InterlockedExchange(&g_ProviderRailSynced,0);
        ObfDereferenceObject(owner_dev);
        ObfDereferenceObject(file);
    } else {
        InterlockedExchange(&g_ProviderRailSynced,0);
    }
    (VOID)ZwClose(handle);
}

static BOOLEAN query_unresolved_rail_ex(ULONG *ClaimEpoch,ULONG *ProviderToken,
                                        BOOLEAN *GpuIsolated,UCHAR *PmicAddress,
                                        ULONG *RetryCount,NTSTATUS *LastStatus)
{
    const ULONGLONG st=(ULONGLONG)InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0);
    if(ClaimEpoch)*ClaimEpoch=(ULONG)st;
    if(ProviderToken)*ProviderToken=(ULONG)((st>>48)&0xffffULL);
    if(GpuIsolated)*GpuIsolated=(st&TG_UNRESOLVED_ISOLATED)!=0ULL;
    if(PmicAddress)*PmicAddress=(UCHAR)((st>>40)&0xffULL);
    if(RetryCount)*RetryCount=(ULONG)InterlockedCompareExchange(&g_UnresolvedRail.RetryCount,0,0);
    if(LastStatus)*LastStatus=(NTSTATUS)InterlockedCompareExchange(&g_UnresolvedRail.LastStatus,0,0);
    return (st&TG_UNRESOLVED_ACTIVE)!=0ULL;
}

BOOLEAN TgGpuQueryUnresolvedRail(BOOLEAN *GpuIsolated,UCHAR *PmicAddress,
                                 ULONG *RetryCount,NTSTATUS *LastStatus)
{
    sync_provider_rail_once();
    return query_unresolved_rail_ex(NULL,NULL,GpuIsolated,PmicAddress,RetryCount,LastStatus);
}

static VOID release_rail_state(TG_PLATFORM_RAIL_STATE *s);

VOID TgGpuPlatformRefreshIsolation(TG_GPU_PLATFORM *p,const TG_GPU_RESOURCES *resources)
{
    if(!p || !resources)return;
    sync_provider_rail_once();
    ULONG ledger_epoch=0,ledger_tok=0;UCHAR ledger_addr=0;
    const BOOLEAN ledger_active=query_unresolved_rail_ex(&ledger_epoch,&ledger_tok,NULL,&ledger_addr,NULL,NULL);
    /* Resource-only StartDevice/StopDevice never claims a rail and must perform
     * zero MMIO; probe CAR_RESET_X / PMC_GPU_CLAMP only when rail state exists. */
    if(!p->EmcRailOwner && !p->RailClaimed && !p->RailOwnershipUncertain &&
       !p->ClaimEpoch && !ledger_active) {
        p->PreservedGpuIsolated=FALSE;
        return;
    }
    BOOLEAN iso=probe_resources_isolated(resources);
    p->PreservedGpuIsolated=iso;
    BOOLEAN publish_ledger=FALSE;
    KIRQL irql,s_irql;
    TG_PLATFORM_RAIL_STATE *s=NULL;
    if(p->Initialized) {
        KeAcquireSpinLock(&p->HandleLock,&irql);
        s=(TG_PLATFORM_RAIL_STATE *)p->RailState;
        if(s)InterlockedIncrement(&s->RefCount);
        KeReleaseSpinLock(&p->HandleLock,irql);
    }
    if(s) {
        KeAcquireSpinLock(&s->Lock,&s_irql);
        iso=probe_resources_isolated(resources);
        s->ClosedGpuIsolated=iso;
        if(s->Owner==p)p->PreservedGpuIsolated=iso;
        /* Re-query ledger under s->Lock so a concurrent AbandonRail/late CLAIM
         * activation never misses an isolation revocation (GC-V9F-03). */
        if(query_unresolved_rail_ex(&ledger_epoch,&ledger_tok,NULL,&ledger_addr,NULL,NULL) &&
           (!s->ClaimEpoch || ledger_epoch==s->ClaimEpoch)) {
            update_unresolved_rail_isolation(ledger_epoch,iso);
            publish_ledger=TRUE;
        }
        KeReleaseSpinLock(&s->Lock,s_irql);
        release_rail_state(s);
    } else if(query_unresolved_rail_ex(&ledger_epoch,&ledger_tok,NULL,&ledger_addr,NULL,NULL) &&
              (!p->ClaimEpoch || ledger_epoch==p->ClaimEpoch)) {
        iso=probe_resources_isolated(resources);
        p->PreservedGpuIsolated=iso;
        update_unresolved_rail_isolation(ledger_epoch,iso);
        publish_ledger=TRUE;
    }
    if(publish_ledger) {
        BOOLEAN cur_iso=iso;
        if(query_unresolved_rail_ex(&ledger_epoch,&ledger_tok,&cur_iso,&ledger_addr,NULL,NULL)) {
            notify_provider_handoff_standalone(ledger_tok?ledger_tok:ledger_epoch,
                p->LastPmicAddress?p->LastPmicAddress:ledger_addr,TRUE,cur_iso);
        }
    }
}

VOID TgGpuHandOffUnresolvedRail(TG_GPU_PLATFORM *p,BOOLEAN GpuIsolated)
{
    if(!p)return;
    ULONG ledger_epoch=0,ledger_tok=0;UCHAR ledger_addr=0;
    const BOOLEAN ledger_active=query_unresolved_rail_ex(&ledger_epoch,&ledger_tok,NULL,&ledger_addr,NULL,NULL);
    if(p->EmcRailOwner && (p->RailClaimed || p->RailOwnershipUncertain)) {
        ULONG epoch=p->ClaimEpoch ? p->ClaimEpoch : next_rail_claim_epoch();
        ULONG tok=(ledger_active && ledger_epoch==epoch && ledger_tok) ? ledger_tok : epoch;
        UCHAR addr=p->LastPmicAddress ? p->LastPmicAddress : ledger_addr;
        p->ClaimEpoch=epoch;
        record_unresolved_rail_epoch_ex(epoch,tok,addr,GpuIsolated,FALSE,FALSE,
            !NT_SUCCESS(p->LastAbandonStatus)?p->LastAbandonStatus:STATUS_UNSUCCESSFUL);
        notify_provider_handoff_standalone(tok,addr,TRUE,GpuIsolated);
    } else if(ledger_active && (!p->ClaimEpoch || ledger_epoch==p->ClaimEpoch)) {
        /* Always refresh/revoke isolation bit in ledger & provider handoff (A05 sticky)! */
        update_unresolved_rail_isolation(ledger_epoch,GpuIsolated);
        notify_provider_handoff_standalone(ledger_tok?ledger_tok:ledger_epoch,
            p->LastPmicAddress?p->LastPmicAddress:ledger_addr,TRUE,GpuIsolated);
    }
}

static NTSTATUS failure(TG_GPU_PLATFORM *p,NTSTATUS status)
{
    if(p && !NT_SUCCESS(status) && NT_SUCCESS(p->IoStatus))p->IoStatus=status;
    return status;
}

static VOID release_rail_state(TG_PLATFORM_RAIL_STATE *s)
{
    if(s && InterlockedDecrement(&s->RefCount)==0)
        ExFreePoolWithTag(s,TG_PLATFORM_TAG);
}

static VOID release_open_ctx(TG_PLATFORM_OPEN_CTX *octx)
{
    if(octx && InterlockedDecrement(&octx->RefCount)==0) {
        release_rail_state(octx->RailState);
        ExFreePoolWithTag(octx,TG_PLATFORM_TAG);
    }
}

/* Caller holds s->Lock and !s->Closed && s->Owner. */
static VOID mirror_rail_state_locked(TG_PLATFORM_RAIL_STATE *s)
{
    TG_GPU_PLATFORM *p=s->Owner;
    if(!p)return;
    p->EmcRailOwner=s->EmcRailOwner;
    p->RailClaimed=s->RailClaimed;
    p->RailOwnershipUncertain=s->RailOwnershipUncertain;
    p->NeedReclaim=s->NeedReclaim;
    p->LastPmicAddress=s->LastPmicAddress;
    p->ClaimEpoch=s->ClaimEpoch;
}

static VOID detach_state_bridge_handles(TG_PLATFORM_RAIL_STATE *state,
                                        BOOLEAN query_remove,BOOLEAN mark_reclaim)
{
    PFILE_OBJECT file=NULL;HANDLE handle=NULL;KIRQL state_irql,irql=0;
    if(!state)return;
    KeAcquireSpinLock(&state->Lock,&state_irql);
    if(!state->Closed) {
        TG_GPU_PLATFORM *p=state->Owner;
        if(p)KeAcquireSpinLock(&p->HandleLock,&irql);
        /* Only latch QueryRemove=TRUE when a published ActivePnpNotify owns the
         * registration; an unpublished candidate notify is unregistered during
         * open rollback and would never receive REMOVE_COMPLETE (GC-V9F-01). */
        const BOOLEAN eff_qr=(query_remove && state->ActivePnpNotify!=NULL);
        file=state->ActiveFile;
        handle=state->ActiveBridge;
        state->ActiveFile=NULL;
        state->ActiveBridge=NULL;
        state->QueryRemove=eff_qr;
        ULONG epoch=++state->BridgeEpoch;
        if(mark_reclaim) {
            ++state->ProviderGeneration;
            if(state->RailClaimed || state->RailOwnershipUncertain)state->NeedReclaim=TRUE;
            state->RailOwnershipUncertain=FALSE;
            clear_unresolved_rail_epoch(state->ClaimEpoch);
        }
        if(p) {
            if(p->BridgeEpoch>=epoch) {
                epoch=++p->BridgeEpoch;
                state->BridgeEpoch=epoch;
            }
            p->FileObject=NULL;
            p->Bridge=NULL;
            p->QueryRemove=eff_qr;
            p->BridgeEpoch=epoch;
            if(p->ProviderGeneration<state->ProviderGeneration)
                p->ProviderGeneration=state->ProviderGeneration;
            mirror_rail_state_locked(state);
            KeReleaseSpinLock(&p->HandleLock,irql);
        }
    }
    KeReleaseSpinLock(&state->Lock,state_irql);
    if(file)ObfDereferenceObject(file);
    if(handle)(VOID)ZwClose(handle);
}

static NTSTATUS NTAPI platform_pnp_notify(PVOID NotificationStructure,PVOID Context)
{
    TARGET_DEVICE_REMOVAL_NOTIFICATION *n=NotificationStructure;
    TG_PLATFORM_RAIL_STATE *state=Context;
    KIRQL state_irql,irql=0;
    if(!n || !state)return STATUS_SUCCESS;
    if(IsEqualGUID(&n->Event,&GUID_TARGET_DEVICE_QUERY_REMOVE)) {
        detach_state_bridge_handles(state,TRUE,FALSE);
    } else if(IsEqualGUID(&n->Event,&GUID_TARGET_DEVICE_REMOVE_COMPLETE)) {
        /* Old SwitchBacklight FDO is gone; allow transparent reopen once a new
         * SwitchFan instance recreates \Device\SwitchBacklight. */
        detach_state_bridge_handles(state,FALSE,TRUE);
    } else if(IsEqualGUID(&n->Event,&GUID_TARGET_DEVICE_REMOVE_CANCELLED)) {
        KeAcquireSpinLock(&state->Lock,&state_irql);
        if(!state->Closed) {
            TG_GPU_PLATFORM *p=state->Owner;
            if(p)KeAcquireSpinLock(&p->HandleLock,&irql);
            state->QueryRemove=FALSE;
            ULONG epoch=++state->BridgeEpoch;
            if(p) {
                if(p->BridgeEpoch>=epoch) {
                    epoch=++p->BridgeEpoch;
                    state->BridgeEpoch=epoch;
                }
                p->QueryRemove=FALSE;
                p->BridgeEpoch=epoch;
                KeReleaseSpinLock(&p->HandleLock,irql);
            }
        }
        KeReleaseSpinLock(&state->Lock,state_irql);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS open_bridge_internal(TG_GPU_PLATFORM *p)
{
    OBJECT_ATTRIBUTES attributes;IO_STATUS_BLOCK iosb={0};
    HANDLE handle=NULL;PFILE_OBJECT file=NULL;PVOID notify=NULL,stale_notify=NULL;
    KIRQL irql,state_irql;NTSTATUS status;ULONG epoch;
    TG_PLATFORM_RAIL_STATE *state=NULL;
    BOOLEAN notify_has_ref=FALSE,stale_has_ref=FALSE;
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\SwitchBacklight");
    KeAcquireSpinLock(&p->HandleLock,&irql);
    if(p->Closing || p->QueryRemove || p->Opening){KeReleaseSpinLock(&p->HandleLock,irql);return STATUS_DEVICE_NOT_READY;}
    if(p->Bridge && p->FileObject){KeReleaseSpinLock(&p->HandleLock,irql);return STATUS_SUCCESS;}
    state=(TG_PLATFORM_RAIL_STATE *)p->RailState;
    if(!state){KeReleaseSpinLock(&p->HandleLock,irql);return STATUS_DEVICE_NOT_READY;}
    p->Opening=TRUE;
    epoch=p->BridgeEpoch;
    stale_notify=p->PnpNotify;
    p->PnpNotify=NULL;
    InterlockedIncrement(&state->RefCount);
    KeReleaseSpinLock(&p->HandleLock,irql);

    if(stale_notify) {
        KeAcquireSpinLock(&state->Lock,&state_irql);
        if(state->ActivePnpNotify==stale_notify) {
            state->ActivePnpNotify=NULL;
            stale_has_ref=TRUE;
        }
        KeReleaseSpinLock(&state->Lock,state_irql);
        (VOID)IoUnregisterPlugPlayNotificationEx(stale_notify);
        if(stale_has_ref)release_rail_state(state);
    }

    InitializeObjectAttributes(&attributes,(PUNICODE_STRING)&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    /* Open WITHOUT FILE_SYNCHRONOUS_IO_*: driver-owned IRPs bound waits safely
     * while SwitchFan retains its power-managed WDF queue. */
    status=ZwCreateFile(&handle,GENERIC_READ|GENERIC_WRITE,&attributes,&iosb,NULL,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,NULL,0);
    if(!NT_SUCCESS(status))goto done;
    status=ObReferenceObjectByHandle(handle,FILE_READ_DATA|FILE_WRITE_DATA,*IoFileObjectType,
        KernelMode,(PVOID *)&file,NULL);
    if(!NT_SUCCESS(status))goto done;
    if(g_PlatformDriverObject && !swbatt_control_file(file)) {
        InterlockedIncrement(&state->RefCount);
        notify_has_ref=TRUE;
        status=IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,0,file,
            g_PlatformDriverObject,platform_pnp_notify,state,&notify);
        if(!NT_SUCCESS(status)) {
            release_rail_state(state);
            notify_has_ref=FALSE;
            goto done;
        }
    }

    /* Publish both owners atomically against PnP detach and Close. The lock
     * order is state->Lock then p->HandleLock; no path nests the reverse order.
     * Until publication succeeds, local variables own all cleanup. */
    KeAcquireSpinLock(&state->Lock,&state_irql);
    if(state->Closed || state->QueryRemove || state->BridgeEpoch!=epoch ||
       state->ActiveFile || state->Owner!=p) {
        KeReleaseSpinLock(&state->Lock,state_irql);
        status=STATUS_DEVICE_NOT_READY;
        goto done;
    }
    /* Keep state locked until p owns the same handles. A PnP callback must
     * not consume staged handles that the caller still regards as local. */
    KeAcquireSpinLock(&p->HandleLock,&irql);
    if(p->Closing || p->QueryRemove || p->BridgeEpoch!=epoch || p->Bridge || p->FileObject || p->RailState!=state) {
        KeReleaseSpinLock(&p->HandleLock,irql);
        KeReleaseSpinLock(&state->Lock,state_irql);
        status=STATUS_DEVICE_NOT_READY;
        goto done;
    }
    state->BridgeEpoch=epoch;
    state->ActiveFile=file;
    state->RailAbi=TgRailAbiUnknown;
    state->ActiveBridge=handle;
    state->ActivePnpNotify=notify;
    p->Bridge=handle;
    p->FileObject=file;
    p->PnpNotify=notify;
    handle=NULL;file=NULL;notify=NULL;notify_has_ref=FALSE;
    KeReleaseSpinLock(&p->HandleLock,irql);
    KeReleaseSpinLock(&state->Lock,state_irql);
    status=STATUS_SUCCESS;
done:
    if(notify) {
        (VOID)IoUnregisterPlugPlayNotificationEx(notify);
        if(notify_has_ref)release_rail_state(state);
    }
    if(file)ObfDereferenceObject(file);
    if(handle)(VOID)ZwClose(handle);
    KeAcquireSpinLock(&p->HandleLock,&irql);
    p->Opening=FALSE;
    KeReleaseSpinLock(&p->HandleLock,irql);
    release_rail_state(state);
    return status;
}

/* Worker routine for deadline-bounded PnP reconnect (A04):
 * runs IoUnregisterPlugPlayNotificationEx + ZwCreateFile + ObReferenceObjectByHandle
 * + IoRegisterPlugPlayNotification off the caller thread without ever touching
 * TG_GPU_PLATFORM memory so a 600 ms timeout followed by Close/free is 100%
 * UAF-free even while CREATE or PnP unregister is still blocked (N1). */
static VOID NTAPI platform_open_work(PDEVICE_OBJECT DeviceObject,PVOID Context)
{
    TG_PLATFORM_OPEN_CTX *octx=Context;
    TG_PLATFORM_RAIL_STATE *state=octx->RailState;
    PIO_WORKITEM work_item=octx->WorkItem;
    PDEVICE_OBJECT owner_dev=octx->OwnerDevice;
    OBJECT_ATTRIBUTES attributes;IO_STATUS_BLOCK iosb={0};
    HANDLE handle=NULL;PFILE_OBJECT file=NULL;PVOID notify=NULL;
    BOOLEAN notify_has_ref=FALSE;
    NTSTATUS status=STATUS_DEVICE_NOT_READY;
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\SwitchBacklight");
    UNREFERENCED_PARAMETER(DeviceObject);
    octx->WorkItem=NULL;
    octx->OwnerDevice=NULL;

    if(octx->StaleNotify) {
        (VOID)IoUnregisterPlugPlayNotificationEx(octx->StaleNotify);
        if(octx->StaleNotifyHasRef)release_rail_state(state);
        octx->StaleNotify=NULL;
        octx->StaleNotifyHasRef=FALSE;
    }

    if(InterlockedCompareExchange(&octx->Handoff,0,0)!=0) {
        status=STATUS_IO_TIMEOUT;
        goto finish;
    }

    InitializeObjectAttributes(&attributes,(PUNICODE_STRING)&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    status=ZwCreateFile(&handle,GENERIC_READ|GENERIC_WRITE,&attributes,&iosb,NULL,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,NULL,0);
    if(!NT_SUCCESS(status))goto finish;
    if(InterlockedCompareExchange(&octx->Handoff,0,0)!=0) {
        status=STATUS_IO_TIMEOUT;
        goto finish;
    }
    status=ObReferenceObjectByHandle(handle,FILE_READ_DATA|FILE_WRITE_DATA,*IoFileObjectType,
        KernelMode,(PVOID *)&file,NULL);
    if(!NT_SUCCESS(status))goto finish;
    if(InterlockedCompareExchange(&octx->Handoff,0,0)!=0) {
        status=STATUS_IO_TIMEOUT;
        goto finish;
    }
    if(octx->DriverObject && !swbatt_control_file(file)) {
        InterlockedIncrement(&state->RefCount);
        notify_has_ref=TRUE;
        status=IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,0,file,
            octx->DriverObject,platform_pnp_notify,state,&notify);
        if(!NT_SUCCESS(status)) {
            release_rail_state(state);
            notify_has_ref=FALSE;
            goto finish;
        }
    }

    octx->Handle=handle;
    octx->File=file;
    octx->Notify=notify;
    octx->NotifyHasRef=notify_has_ref;
    octx->Status=STATUS_SUCCESS;
    if(InterlockedCompareExchange(&octx->Handoff,1,0)==0) {
        handle=NULL;file=NULL;notify=NULL;notify_has_ref=FALSE;
    } else {
        octx->Handle=NULL;octx->File=NULL;octx->Notify=NULL;octx->NotifyHasRef=FALSE;
        status=STATUS_IO_TIMEOUT;
    }

finish:
    if(notify) {
        (VOID)IoUnregisterPlugPlayNotificationEx(notify);
        if(notify_has_ref)release_rail_state(state);
    }
    if(file)ObfDereferenceObject(file);
    if(handle)(VOID)ZwClose(handle);
    if(InterlockedCompareExchange(&octx->Handoff,0,0)==0) {
        octx->Status=status;
        (VOID)InterlockedCompareExchange(&octx->Handoff,1,0);
    }
    KeSetEvent(&octx->Done,IO_NO_INCREMENT,FALSE);
    IoFreeWorkItem(work_item);
    if(owner_dev)ObfDereferenceObject(owner_dev);
    release_open_ctx(octx);
}

static VOID NTAPI platform_open_cleanup_work(PDEVICE_OBJECT DeviceObject,PVOID Context)
{
    TG_PLATFORM_OPEN_CTX *octx=Context;
    TG_PLATFORM_RAIL_STATE *state=octx->RailState;
    PIO_WORKITEM c_wi=octx->CleanupWorkItem;
    PVOID notify=octx->Notify;
    BOOLEAN notify_has_ref=octx->NotifyHasRef;
    PFILE_OBJECT file=octx->File;
    HANDLE handle=octx->Handle;
    UNREFERENCED_PARAMETER(DeviceObject);
    octx->CleanupWorkItem=NULL;
    octx->Notify=NULL;octx->NotifyHasRef=FALSE;
    octx->File=NULL;octx->Handle=NULL;
    if(notify) {
        (VOID)IoUnregisterPlugPlayNotificationEx(notify);
        if(notify_has_ref)release_rail_state(state);
    }
    if(file)ObfDereferenceObject(file);
    if(handle)(VOID)ZwClose(handle);
    if(c_wi)IoFreeWorkItem(c_wi);
    release_open_ctx(octx);
}

static NTSTATUS open_bridge_deadline(TG_GPU_PLATFORM *p,ULONGLONG deadline)
{
    KIRQL irql,state_irql;
    PDEVICE_OBJECT owner_dev=NULL;
    TG_PLATFORM_RAIL_STATE *state=NULL;
    PVOID stale_notify=NULL,notify=NULL;
    BOOLEAN stale_has_ref=FALSE,notify_has_ref=FALSE;
    HANDLE handle=NULL;
    PFILE_OBJECT file=NULL;
    PIO_WORKITEM cleanup_wi=NULL;
    ULONG epoch=0;
    TG_PLATFORM_OPEN_CTX *octx=NULL;
    LARGE_INTEGER timeout;
    NTSTATUS wait,status;

    ULONGLONG now=KeQueryInterruptTime();
    if(now>=deadline)return STATUS_IO_TIMEOUT;

    /* A04: Allocate context + work items BEFORE detaching p->PnpNotify so an OOM
     * failure never triggers a synchronous IoUnregisterPlugPlayNotificationEx on
     * the deadline-bounded caller thread; stale PnpNotify stays owned by p for
     * subsequent retry or TgGpuClosePlatform. */
    KeAcquireSpinLock(&p->HandleLock,&irql);
    if(p->Closing || p->QueryRemove || p->Opening) {
        KeReleaseSpinLock(&p->HandleLock,irql);
        return STATUS_DEVICE_NOT_READY;
    }
    if(p->Bridge && p->FileObject) {
        KeReleaseSpinLock(&p->HandleLock,irql);
        return STATUS_SUCCESS;
    }
    state=(TG_PLATFORM_RAIL_STATE *)p->RailState;
    owner_dev=(PDEVICE_OBJECT)p->OwnerDevice;
    if(!state || !owner_dev) {
        KeReleaseSpinLock(&p->HandleLock,irql);
        return STATUS_DEVICE_NOT_READY;
    }
    p->Opening=TRUE;
    ObfReferenceObject(owner_dev);
    InterlockedIncrement(&state->RefCount);
    KeReleaseSpinLock(&p->HandleLock,irql);

    octx=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*octx),TG_PLATFORM_TAG);
    if(!octx) {
        KeAcquireSpinLock(&p->HandleLock,&irql);
        p->Opening=FALSE;
        KeReleaseSpinLock(&p->HandleLock,irql);
        ObfDereferenceObject(owner_dev);
        release_rail_state(state);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    octx->WorkItem=IoAllocateWorkItem(owner_dev);
    if(octx->WorkItem)cleanup_wi=IoAllocateWorkItem(owner_dev);
    if(!octx->WorkItem || !cleanup_wi) {
        if(cleanup_wi)IoFreeWorkItem(cleanup_wi);
        if(octx->WorkItem)IoFreeWorkItem(octx->WorkItem);
        ExFreePoolWithTag(octx,TG_PLATFORM_TAG);
        KeAcquireSpinLock(&p->HandleLock,&irql);
        p->Opening=FALSE;
        KeReleaseSpinLock(&p->HandleLock,irql);
        ObfDereferenceObject(owner_dev);
        release_rail_state(state);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireSpinLock(&p->HandleLock,&irql);
    if(p->Closing || p->QueryRemove || p->Bridge || p->FileObject || p->RailState!=state) {
        const BOOLEAN already_open=(!p->Closing && !p->QueryRemove && p->Bridge && p->FileObject && p->RailState==state);
        p->Opening=FALSE;
        KeReleaseSpinLock(&p->HandleLock,irql);
        IoFreeWorkItem(cleanup_wi);
        IoFreeWorkItem(octx->WorkItem);
        ExFreePoolWithTag(octx,TG_PLATFORM_TAG);
        ObfDereferenceObject(owner_dev);
        release_rail_state(state);
        return already_open ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }
    epoch=p->BridgeEpoch;
    stale_notify=p->PnpNotify;
    p->PnpNotify=NULL;
    KeReleaseSpinLock(&p->HandleLock,irql);

    if(stale_notify) {
        KeAcquireSpinLock(&state->Lock,&state_irql);
        if(state->ActivePnpNotify==stale_notify) {
            state->ActivePnpNotify=NULL;
            stale_has_ref=TRUE;
        }
        KeReleaseSpinLock(&state->Lock,state_irql);
    }

    octx->RailState=state; /* transfers +1 state ref to octx */
    octx->OwnerDevice=owner_dev; /* transfers +1 owner_dev ref to platform_open_work */
    octx->DriverObject=g_PlatformDriverObject;
    octx->StaleNotify=stale_notify;
    octx->StaleNotifyHasRef=stale_has_ref;
    octx->RefCount=2;
    octx->Handoff=0;
    octx->Status=STATUS_UNSUCCESSFUL;
    KeInitializeEvent(&octx->Done,NotificationEvent,FALSE);

    IoQueueWorkItem(octx->WorkItem,platform_open_work,DelayedWorkQueue,octx);
    now=KeQueryInterruptTime();
    if(now>=deadline) {
        wait=STATUS_TIMEOUT;
    } else {
        ULONGLONG rem=deadline-now;
        if(rem>(ULONGLONG)(-TG_PLATFORM_TIMEOUT_100NS))
            rem=(ULONGLONG)(-TG_PLATFORM_TIMEOUT_100NS);
        timeout.QuadPart=-(LONGLONG)rem;
        wait=KeWaitForSingleObject(&octx->Done,Executive,KernelMode,FALSE,&timeout);
    }

    const BOOLEAN past_deadline=(wait!=STATUS_SUCCESS || KeQueryInterruptTime()>=deadline);
    if(past_deadline) {
        const LONG prev_ho=InterlockedCompareExchange(&octx->Handoff,2,0);
        if(prev_ho!=1) {
            /* Worker has not handed off yet and will clean up on its own thread! */
            IoFreeWorkItem(cleanup_wi);
            KeAcquireSpinLock(&p->HandleLock,&irql);
            p->Opening=FALSE;
            KeReleaseSpinLock(&p->HandleLock,irql);
            release_open_ctx(octx);
            return (wait==STATUS_TIMEOUT || NT_SUCCESS(wait)) ? STATUS_IO_TIMEOUT : wait;
        }
        /* Worker completed handoff right at the deadline boundary (A04):
         * if open succeeded, publish the valid connection in O(1) spinlock time
         * (zero unregister!) while still returning STATUS_IO_TIMEOUT to caller! */
    }

    status=octx->Status;
    handle=octx->Handle;octx->Handle=NULL;
    file=octx->File;octx->File=NULL;
    notify=octx->Notify;octx->Notify=NULL;
    notify_has_ref=octx->NotifyHasRef;octx->NotifyHasRef=FALSE;
    if(!NT_SUCCESS(status)) {
        if(past_deadline)
            status=(wait==STATUS_TIMEOUT || NT_SUCCESS(wait)) ? STATUS_IO_TIMEOUT : wait;
        goto done_open_dl;
    }

    KeAcquireSpinLock(&state->Lock,&state_irql);
    if(state->Closed || state->QueryRemove || state->BridgeEpoch!=epoch ||
       state->ActiveFile || state->Owner!=p) {
        KeReleaseSpinLock(&state->Lock,state_irql);
        status=past_deadline ?
            ((wait==STATUS_TIMEOUT || NT_SUCCESS(wait)) ? STATUS_IO_TIMEOUT : wait) :
            STATUS_DEVICE_NOT_READY;
        goto done_open_dl;
    }
    /* Keep state locked until p owns the same handles. A PnP callback must
     * not consume staged handles that the caller still regards as local. */
    KeAcquireSpinLock(&p->HandleLock,&irql);
    if(p->Closing || p->QueryRemove || p->BridgeEpoch!=epoch || p->Bridge || p->FileObject || p->RailState!=state) {
        KeReleaseSpinLock(&p->HandleLock,irql);
        KeReleaseSpinLock(&state->Lock,state_irql);
        status=past_deadline ?
            ((wait==STATUS_TIMEOUT || NT_SUCCESS(wait)) ? STATUS_IO_TIMEOUT : wait) :
            STATUS_DEVICE_NOT_READY;
        goto done_open_dl;
    }
    state->BridgeEpoch=epoch;
    state->ActiveFile=file;
    state->RailAbi=TgRailAbiUnknown;
    state->ActiveBridge=handle;
    state->ActivePnpNotify=notify;
    p->Bridge=handle;
    p->FileObject=file;
    p->PnpNotify=notify;
    handle=NULL;file=NULL;notify=NULL;notify_has_ref=FALSE;
    p->Opening=FALSE;
    KeReleaseSpinLock(&p->HandleLock,irql);
    KeReleaseSpinLock(&state->Lock,state_irql);
    IoFreeWorkItem(cleanup_wi);
    release_open_ctx(octx);
    return past_deadline ?
        ((wait==STATUS_TIMEOUT || NT_SUCCESS(wait)) ? STATUS_IO_TIMEOUT : wait) :
        STATUS_SUCCESS;

done_open_dl:
    KeAcquireSpinLock(&p->HandleLock,&irql);
    p->Opening=FALSE;
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(notify || file || handle) {
        /* Offload rollback IoUnregisterPlugPlayNotificationEx + ZwClose onto the
         * pre-allocated cleanup work item so caller NEVER blocks on unregister! */
        octx->CleanupWorkItem=cleanup_wi;cleanup_wi=NULL;
        octx->Notify=notify;octx->NotifyHasRef=notify_has_ref;
        octx->File=file;octx->Handle=handle;
        IoQueueWorkItem(octx->CleanupWorkItem,platform_open_cleanup_work,DelayedWorkQueue,octx);
        return status;
    }
    if(cleanup_wi)IoFreeWorkItem(cleanup_wi);
    release_open_ctx(octx);
    return status;
}

NTSTATUS TgGpuOpenPlatform(TG_GPU_PLATFORM *p,const TG_GPU_RESOURCES *resources)
{
    PDEVICE_OBJECT owner_dev=NULL;TG_PLATFORM_RAIL_STATE *state=NULL;NTSTATUS status;
    PAGED_CODE();
    if(!p || !resources || p->Bridge || p->FileObject || p->Resources || p->RailState)
        return STATUS_INVALID_PARAMETER;
    for(UINT i=0;i<TgGpuApertureCount;++i)
        if(!resources->Mapping[i] || resources->Memory[i].Type!=CmResourceTypeMemory ||
           resources->Memory[i].u.Memory.Length<4)return STATUS_DEVICE_CONFIGURATION_ERROR;
    if(!g_PlatformDriverObject)return STATUS_INVALID_DEVICE_STATE;
    owner_dev=g_PlatformDriverObject->DeviceObject;
    if(!owner_dev || owner_dev->DriverObject!=g_PlatformDriverObject)return STATUS_INVALID_DEVICE_STATE;
    state=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*state),TG_PLATFORM_TAG);
    if(!state)return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeSpinLock(&state->Lock);
    state->RefCount=1;
    state->Owner=p;
    /* Preserve monotonic BridgeEpoch and any unresolved rail ownership across
     * Stop->Start or adapter recreation from driver-lifetime ledger (V4-01, A05). */
    state->BridgeEpoch=p->BridgeEpoch;
    state->ProviderGeneration=p->ProviderGeneration;
    {
        ULONG ledger_epoch=0,ledger_tok=0;BOOLEAN ledger_iso=FALSE;UCHAR ledger_addr=0;
        const BOOLEAN ledger_active=query_unresolved_rail_ex(&ledger_epoch,&ledger_tok,&ledger_iso,&ledger_addr,NULL,NULL);
        if(p->RailOwnershipUncertain || ledger_active) {
            const BOOLEAN live_iso=probe_resources_isolated(resources);
            p->PreservedGpuIsolated=live_iso;
            UCHAR addr=p->LastPmicAddress ? p->LastPmicAddress : ledger_addr;
            ULONG c_epoch=ledger_active ? ledger_epoch :
                (p->ClaimEpoch ? p->ClaimEpoch : next_rail_claim_epoch());
            if(ledger_active)
                update_unresolved_rail_isolation(ledger_epoch,live_iso);
            state->ClaimEpoch=c_epoch;
            state->ProviderToken=ledger_tok ? ledger_tok : c_epoch;
            state->EmcRailOwner=TRUE;
            state->RailOwnershipUncertain=TRUE;
            state->LastPmicAddress=addr;
            p->ClaimEpoch=c_epoch;
            p->EmcRailOwner=TRUE;
            p->RailOwnershipUncertain=TRUE;
            p->LastPmicAddress=addr;
        } else {
            p->PreservedGpuIsolated=FALSE;
            p->ClaimEpoch=0;
        }
    }
    ObfReferenceObject(owner_dev);
    KeInitializeSpinLock(&p->HandleLock);
    p->Initialized=TRUE;
    p->Opening=p->Closing=FALSE;
    p->OwnerDevice=owner_dev;
    p->RailState=state;
    p->IoStatus=STATUS_SUCCESS;
    p->QueryRemove=FALSE;
    p->NeedReclaim=FALSE;
    if(!p->RailOwnershipUncertain) {
        p->LastPmicAddress=0;
        p->EmcRailOwner=FALSE;
        p->RailClaimed=FALSE;
    } else {
        p->RailClaimed=FALSE;
    }
    status=open_bridge_internal(p);
    if(!NT_SUCCESS(status)) {
        KIRQL irql;
        KeAcquireSpinLock(&state->Lock,&irql);
        state->Closed=TRUE;state->Owner=NULL;state->ActiveFile=NULL;
        state->ActiveBridge=NULL;state->ActivePnpNotify=NULL;
        KeReleaseSpinLock(&state->Lock,irql);
        p->RailState=NULL;
        release_rail_state(state);
        if(p->OwnerDevice){ObfDereferenceObject(p->OwnerDevice);p->OwnerDevice=NULL;}
        return failure(p,status);
    }
    p->Resources=resources;return STATUS_SUCCESS;
}

static bool read_pmic(void *context,UINT8 address,UINT8 reg,UINT8 *value);
static UINT32 read32(void *context,UINT32 address);
static VOID NTAPI platform_compensate_work(PDEVICE_OBJECT DeviceObject,PVOID Context);

NTSTATUS TgGpuReferencePlatformBridge(TG_GPU_PLATFORM *p,PFILE_OBJECT *file,
                                     PDEVICE_OBJECT *owner)
{
    KIRQL irql;
    NTSTATUS status;
    PAGED_CODE();
    if (!p || !file || !owner || !p->Initialized || !p->Resources)
        return STATUS_INVALID_PARAMETER;
    *file = NULL; *owner = NULL;
    status = open_bridge_internal(p);
    if (!NT_SUCCESS(status)) return status;
    KeAcquireSpinLock(&p->HandleLock,&irql);
    if (!p->Closing && !p->QueryRemove && p->Bridge && p->FileObject && p->OwnerDevice) {
        *file = p->FileObject; *owner = p->OwnerDevice;
        ObfReferenceObject(*file); ObfReferenceObject(*owner);
    }
    KeReleaseSpinLock(&p->HandleLock,irql);
    return *file ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

NTSTATUS TgGpuPlatformAbandonRail(TG_GPU_PLATFORM *p,UCHAR address)
{
    UINT8 enable=0,voltage=0;UINT32 reset,clamp;
    KIRQL irql,state_irql;BOOLEAN need_release=FALSE;
    TG_PLATFORM_RAIL_STATE *state=NULL;UCHAR verified=0;
    ULONG provider_tok=0;
    if(!p || !p->Initialized)return STATUS_SUCCESS;
    KeAcquireSpinLock(&p->HandleLock,&irql);
    state=(TG_PLATFORM_RAIL_STATE *)p->RailState;
    if(state)InterlockedIncrement(&state->RefCount);
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(!state)return (p->EmcRailOwner && (p->RailClaimed || p->RailOwnershipUncertain)) ?
        STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
    KeAcquireSpinLock(&state->Lock,&state_irql);
    if(!state->Closed) {
        need_release=state->EmcRailOwner && (state->RailClaimed || state->RailOwnershipUncertain);
        verified=state->LastPmicAddress;
        provider_tok=state->ProviderToken;
    }
    KeReleaseSpinLock(&state->Lock,state_irql);
    if(!need_release) {
        if(InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0)==0 &&
           InterlockedCompareExchange(&g_ProviderRailSynced,0,0)==0) {
            sync_provider_rail_once();
            ULONG rec_ep=0,rec_tok=0;UCHAR rec_addr=0;
            if(query_unresolved_rail_ex(&rec_ep,&rec_tok,NULL,&rec_addr,NULL,NULL)) {
                KeAcquireSpinLock(&state->Lock,&state_irql);
                if(!state->Closed) {
                    state->ClaimEpoch=rec_ep;
                    state->ProviderToken=rec_tok?rec_tok:rec_ep;
                    state->EmcRailOwner=TRUE;
                    state->RailOwnershipUncertain=TRUE;
                    if(!state->LastPmicAddress)state->LastPmicAddress=rec_addr;
                    mirror_rail_state_locked(state);
                    need_release=TRUE;
                    verified=state->LastPmicAddress;
                    provider_tok=state->ProviderToken;
                }
                KeReleaseSpinLock(&state->Lock,state_irql);
            }
        }
        if(!need_release){release_rail_state(state);return STATUS_SUCCESS;}
    }
    if(!address)address=verified;
    /* !Power.owned can mean that another channel was already running.
     * Only an actually reset and clamped GPU may hand the rail to EMC. */
    reset=read32(p,0x6000628cu);clamp=read32(p,0x7000e6d4u);
    const BOOLEAN gpu_isolated=(reset!=UINT32_MAX && clamp!=UINT32_MAX &&
                                (reset&(1u<<24))!=0u && (clamp&1u)!=0u);
    p->PreservedGpuIsolated=gpu_isolated;
    NTSTATUS status;
    BOOLEAN defer_recorded=FALSE;
    ULONG handoff_tok=0,handoff_epoch=0;UCHAR handoff_addr=0;BOOLEAN handoff_iso=gpu_isolated;
    if(!address) {
        status=STATUS_DEVICE_NOT_READY;
    } else if(!gpu_isolated) {
        status=STATUS_DEVICE_BUSY;
    } else {
        BOOLEAN claim_still_in_flight=FALSE,reclaim_in_flight=FALSE;
        KeAcquireSpinLock(&state->Lock,&state_irql);
        if(!state->Closed && state->InFlightClaimSeq!=0 &&
           state->LastRailOpSeq==state->InFlightClaimSeq &&
           state->LastRailOpState==TgRailOpInFlight) {
            claim_still_in_flight=TRUE;
            reclaim_in_flight=state->NeedReclaim;
        }
        KeReleaseSpinLock(&state->Lock,state_irql);

        BOOLEAN val_valid=FALSE;
        UINT32 rel_val16=0;
        if(!(claim_still_in_flight && reclaim_in_flight)) {
            if(read_pmic(p,address,0x06u,&enable) && read_pmic(p,address,0x23u,&voltage)) {
                rel_val16=(((UINT32)enable)<<8)|voltage;
                val_valid=TRUE;
            }
        }
        /* Re-check after PMIC reads in case a concurrent CLAIM started or finished. */
        KeAcquireSpinLock(&state->Lock,&state_irql);
        claim_still_in_flight=(!state->Closed && state->InFlightClaimSeq!=0 &&
                               state->LastRailOpSeq==state->InFlightClaimSeq &&
                               state->LastRailOpState==TgRailOpInFlight);
        KeReleaseSpinLock(&state->Lock,state_irql);

        if(claim_still_in_flight) {
            PFILE_OBJECT c_file=NULL;PDEVICE_OBJECT c_owner=NULL;PIO_WORKITEM c_wi=NULL;
            KeAcquireSpinLock(&p->HandleLock,&irql);
            if(!p->Closing && p->OwnerDevice) {
                c_owner=(PDEVICE_OBJECT)p->OwnerDevice;
                ObfReferenceObject(c_owner);
                if(p->FileObject) {
                    c_file=(PFILE_OBJECT)p->FileObject;
                    ObfReferenceObject(c_file);
                }
            }
            KeReleaseSpinLock(&p->HandleLock,irql);
            if(c_owner)c_wi=IoAllocateWorkItem(c_owner);

            BOOLEAN armed_defer=FALSE;
            BOOLEAN abort_busy=FALSE;
            KeAcquireSpinLock(&state->Lock,&state_irql);
            if(!state->Closed && state->InFlightClaimSeq!=0 &&
               state->LastRailOpSeq==state->InFlightClaimSeq &&
               state->LastRailOpState==TgRailOpInFlight) {
                const BOOLEAN live_iso=p->Resources ? probe_resources_isolated(p->Resources) : gpu_isolated;
                state->ClosedGpuIsolated=live_iso;
                if(state->Owner==p)p->PreservedGpuIsolated=live_iso;
                handoff_iso=live_iso;
                if(!live_iso) {
                    abort_busy=TRUE;
                } else {
                    state->DeferReleasePending=TRUE;
                    state->DeferReleaseValueValid=val_valid;
                    state->DeferReleaseAddress=address;
                    state->DeferReleaseValue=(USHORT)rel_val16;
                    state->DeferReleaseEpoch=state->ClaimEpoch ? state->ClaimEpoch : next_rail_claim_epoch();
                    state->ClaimEpoch=state->DeferReleaseEpoch;
                    state->DeferReleaseToken=state->ProviderToken;
                    state->EmcRailOwner=TRUE;
                    state->RailOwnershipUncertain=TRUE;
                    if(address)state->LastPmicAddress=address;
                    mirror_rail_state_locked(state);
                    handoff_epoch=state->ClaimEpoch;
                    handoff_tok=state->ProviderToken ? state->ProviderToken : state->ClaimEpoch;
                    handoff_addr=state->LastPmicAddress;
                    /* Publish ClosedGpuIsolated, DeferReleasePending, and ledger
                     * inside the SAME state->Lock critical section (GC-V9F-03). */
                    record_unresolved_rail_epoch_ex(handoff_epoch,handoff_tok,handoff_addr,
                        TRUE,FALSE,FALSE,STATUS_IO_TIMEOUT);
                    if(c_wi && c_owner && !state->CompensateWorkItem && !state->CompensateQueued) {
                        state->CompensateWorkItem=c_wi;c_wi=NULL;
                        state->CompensateFile=c_file;c_file=NULL;
                        state->CompensateOwner=c_owner;c_owner=NULL;
                        InterlockedIncrement(&state->RefCount);
                    }
                    armed_defer=TRUE;
                    defer_recorded=TRUE;
                }
            }
            KeReleaseSpinLock(&state->Lock,state_irql);
            if(c_wi)IoFreeWorkItem(c_wi);
            if(c_owner)ObfDereferenceObject(c_owner);
            if(c_file)ObfDereferenceObject(c_file);
            if(abort_busy) {
                status=STATUS_DEVICE_BUSY;
            } else if(armed_defer) {
                status=STATUS_IO_TIMEOUT;
            } else if(!val_valid && (!read_pmic(p,address,0x06u,&enable) || !read_pmic(p,address,0x23u,&voltage))) {
                status=STATUS_IO_DEVICE_ERROR;
            } else {
                if(!val_valid)rel_val16=(((UINT32)enable)<<8)|voltage;
                const UINT32 full_val=(InterlockedCompareExchange(&g_ProviderTokenAware,0,0) && provider_tok) ?
                    (((UINT32)(provider_tok&0xffffu)<<16)|rel_val16) : rel_val16;
                struct tg_gpu_pmic_request op={address,TG_GPU_PMIC_RELEASE_RAIL,1,full_val};
                status=TgGpuPlatformTryRequest(p,&op);
            }
        } else if(!val_valid) {
            status=STATUS_IO_DEVICE_ERROR;
        } else {
            const UINT32 full_val=(InterlockedCompareExchange(&g_ProviderTokenAware,0,0) && provider_tok) ?
                (((UINT32)(provider_tok&0xffffu)<<16)|rel_val16) : rel_val16;
            struct tg_gpu_pmic_request op={address,TG_GPU_PMIC_RELEASE_RAIL,1,full_val};
            status=TgGpuPlatformTryRequest(p,&op);
        }
    }
    if(!NT_SUCCESS(status)) {
        p->LastAbandonStatus=status;
        ++p->AbandonFailureCount;
        if(!defer_recorded) {
            KeAcquireSpinLock(&state->Lock,&state_irql);
            /* Never overwrite a confirmed RELEASE_RAIL receipt that completed
             * concurrently during IoCancelIrp (A05): only mark uncertain when the
             * rail is still claimed or uncertain, and sample live isolation under
             * state->Lock so a concurrent RefreshIsolation(FALSE) is never overwritten! */
            if(!state->Closed && state->EmcRailOwner &&
               (state->RailClaimed || state->RailOwnershipUncertain)) {
                const BOOLEAN live_iso=p->Resources ? probe_resources_isolated(p->Resources) : gpu_isolated;
                state->ClosedGpuIsolated=live_iso;
                if(state->Owner==p)p->PreservedGpuIsolated=live_iso;
                handoff_iso=live_iso;
                if(!state->ClaimEpoch)state->ClaimEpoch=next_rail_claim_epoch();
                state->RailOwnershipUncertain=TRUE;
                if(address)state->LastPmicAddress=address;
                mirror_rail_state_locked(state);
                handoff_epoch=state->ClaimEpoch;
                handoff_tok=state->ProviderToken ? state->ProviderToken : state->ClaimEpoch;
                handoff_addr=state->LastPmicAddress;
                record_unresolved_rail_epoch_ex(handoff_epoch,handoff_tok,handoff_addr,
                    live_iso,FALSE,FALSE,status);
            }
            KeReleaseSpinLock(&state->Lock,state_irql);
        }
        if(handoff_epoch) {
            BOOLEAN cur_iso=handoff_iso;
            ULONG cur_ep=0;
            if(query_unresolved_rail_ex(&cur_ep,&handoff_tok,&cur_iso,&handoff_addr,NULL,NULL) &&
               cur_ep==handoff_epoch) {
                PFILE_OBJECT h_file=NULL;PDEVICE_OBJECT h_owner=NULL;
                KeAcquireSpinLock(&p->HandleLock,&irql);
                if(!p->Closing && p->FileObject && p->OwnerDevice) {
                    h_file=(PFILE_OBJECT)p->FileObject;
                    h_owner=(PDEVICE_OBJECT)p->OwnerDevice;
                    ObfReferenceObject(h_file);
                    ObfReferenceObject(h_owner);
                }
                KeReleaseSpinLock(&p->HandleLock,irql);
                if(h_file && h_owner) {
                    notify_provider_handoff_via_file(h_file,h_owner,handoff_tok,handoff_addr,TRUE,cur_iso);
                    ObfDereferenceObject(h_owner);
                    ObfDereferenceObject(h_file);
                }
            }
        }
    }
    release_rail_state(state);
    return status;
}

NTSTATUS TgGpuClosePlatform(TG_GPU_PLATFORM *p)
{
    PVOID notify=NULL,owner=NULL;HANDLE handle=NULL;PFILE_OBJECT file=NULL;
    TG_PLATFORM_RAIL_STATE *state=NULL;
    KIRQL irql,state_irql;NTSTATUS status=STATUS_SUCCESS;
    BOOLEAN unresolved_rail=FALSE,gpu_isolated=FALSE,notify_has_ref=FALSE;
    UCHAR unresolved_addr=0;
    ULONG unresolved_epoch=0,unresolved_tok=0;
    PAGED_CODE();
    if(!p)return STATUS_INVALID_PARAMETER;
    /* Idempotent Close (A05): repeated Close calls after Initialized==FALSE
     * must be exact no-ops and never wipe preserved unresolved rail state. */
    if(!p->Initialized)return STATUS_SUCCESS;
    if(p->Resources && (p->EmcRailOwner || p->RailClaimed || p->RailOwnershipUncertain ||
       p->ClaimEpoch || query_unresolved_rail_ex(NULL,NULL,NULL,NULL,NULL,NULL))) {
        const UINT32 reset=read32(p,0x6000628cu);
        const UINT32 clamp=read32(p,0x7000e6d4u);
        gpu_isolated=(reset!=UINT32_MAX && clamp!=UINT32_MAX &&
                      (reset&(1u<<24))!=0u && (clamp&1u)!=0u);
        p->PreservedGpuIsolated=gpu_isolated;
    } else {
        gpu_isolated=p->PreservedGpuIsolated;
    }
    KeAcquireSpinLock(&p->HandleLock,&irql);
    p->Closing=TRUE;
    ++p->BridgeEpoch;
    ++p->ProviderGeneration;
    notify=p->PnpNotify;p->PnpNotify=NULL;
    state=(TG_PLATFORM_RAIL_STATE *)p->RailState;
    p->RailState=NULL;
    if(!state && p->EmcRailOwner && (p->RailClaimed || p->RailOwnershipUncertain)) {
        unresolved_rail=TRUE;
        unresolved_addr=p->LastPmicAddress;
        unresolved_epoch=p->ClaimEpoch ? p->ClaimEpoch : next_rail_claim_epoch();
        unresolved_tok=unresolved_epoch;
    }
    KeReleaseSpinLock(&p->HandleLock,irql);

    /* Detach Owner under state->Lock BEFORE returning so any concurrent or late
     * async IRP completion can never touch TG_GPU_PLATFORM/TG_GPU_ADAPTER memory.
     * Persist unresolved rail ownership into driver-lifetime g_UnresolvedRail as
     * well as p so repeated Close or RemoveDevice never drops accountability (A05). */
    if(state) {
        KeAcquireSpinLock(&state->Lock,&state_irql);
        if(p->Resources && (state->EmcRailOwner || state->RailClaimed || state->RailOwnershipUncertain ||
           state->ClaimEpoch || query_unresolved_rail_ex(NULL,NULL,NULL,NULL,NULL,NULL))) {
            gpu_isolated=probe_resources_isolated(p->Resources);
            p->PreservedGpuIsolated=gpu_isolated;
        }
        state->ClosedGpuIsolated=gpu_isolated;
        if(state->ActivePnpNotify==notify && notify!=NULL) {
            state->ActivePnpNotify=NULL;
            notify_has_ref=TRUE;
        }
        if(state->EmcRailOwner && (state->RailClaimed || state->RailOwnershipUncertain)) {
            if(!state->ClaimEpoch)state->ClaimEpoch=next_rail_claim_epoch();
            unresolved_rail=TRUE;
            unresolved_addr=state->LastPmicAddress;
            unresolved_epoch=state->ClaimEpoch;
            unresolved_tok=state->ProviderToken ? state->ProviderToken : state->ClaimEpoch;
            record_unresolved_rail_epoch_ex(unresolved_epoch,unresolved_tok,unresolved_addr,
                gpu_isolated,FALSE,FALSE,!NT_SUCCESS(p->LastAbandonStatus)?p->LastAbandonStatus:STATUS_UNSUCCESSFUL);
        } else {
            update_unresolved_rail_isolation(0,gpu_isolated);
        }
        state->Closed=TRUE;
        state->Owner=NULL;
        state->ActiveFile=NULL;
        state->ActiveBridge=NULL;
        ++state->BridgeEpoch;
        KeReleaseSpinLock(&state->Lock,state_irql);
    } else if(unresolved_rail) {
        record_unresolved_rail_epoch_ex(unresolved_epoch,unresolved_tok,unresolved_addr,
            gpu_isolated,FALSE,FALSE,!NT_SUCCESS(p->LastAbandonStatus)?p->LastAbandonStatus:STATUS_UNSUCCESSFUL);
    } else {
        update_unresolved_rail_isolation(0,gpu_isolated);
    }
    if(notify) {
        (VOID)IoUnregisterPlugPlayNotificationEx(notify);
        if(notify_has_ref)release_rail_state(state);
    }
    if(state)release_rail_state(state);

    KeAcquireSpinLock(&p->HandleLock,&irql);
    handle=p->Bridge;file=(PFILE_OBJECT)p->FileObject;
    owner=p->OwnerDevice;
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(unresolved_rail && file && owner) {
        notify_provider_handoff_via_file(file,(PDEVICE_OBJECT)owner,unresolved_tok,
            unresolved_addr,TRUE,gpu_isolated);
    }
    if(handle) {
        status=ZwClose(handle);
        if(!NT_SUCCESS(status))return failure(p,status);
    }
    KeAcquireSpinLock(&p->HandleLock,&irql);
    p->Bridge=NULL;p->FileObject=NULL;
    owner=p->OwnerDevice;p->OwnerDevice=NULL;
    p->QueryRemove=FALSE;p->NeedReclaim=FALSE;
    p->RailClaimed=FALSE;
    if(unresolved_rail) {
        p->ClaimEpoch=unresolved_epoch;
        p->EmcRailOwner=TRUE;
        p->RailOwnershipUncertain=TRUE;
        p->LastPmicAddress=unresolved_addr;
    } else {
        p->ClaimEpoch=0;
        p->EmcRailOwner=FALSE;
        p->RailOwnershipUncertain=FALSE;
        p->LastPmicAddress=0;
    }
    p->Closing=FALSE;
    p->Initialized=FALSE;
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(file)ObfDereferenceObject(file);
    if(owner)ObfDereferenceObject(owner);
    p->Resources=NULL;
    return STATUS_SUCCESS;
}

static VOID release_irp_ctx(TG_PLATFORM_IRP_CTX *ctx)
{
    if(InterlockedDecrement(&ctx->RefCount)==0) {
        if(InterlockedExchange(&ctx->ClaimCounted,0)==1)
            InterlockedDecrement(&g_InFlightNewClaims);
        IoFreeIrp(ctx->Irp);
        if(ctx->OwnerDevice)ObfDereferenceObject(ctx->OwnerDevice);
        ObfDereferenceObject(ctx->FileObject);
        release_rail_state(ctx->RailState);
        ExFreePoolWithTag(ctx,TG_PLATFORM_TAG);
    }
}

static PIO_WORKITEM schedule_compensate_locked(TG_PLATFORM_RAIL_STATE *state,BOOLEAN run_release)
{
    state->DeferReleasePending=FALSE;
    if(!state->CompensateWorkItem || state->CompensateQueued)
        return NULL;
    state->CompensateQueued=TRUE;
    state->CompensateRunRelease=run_release;
    return state->CompensateWorkItem;
}

static VOID NTAPI platform_compensate_work(PDEVICE_OBJECT DeviceObject,PVOID Context)
{
    TG_PLATFORM_RAIL_STATE *state=Context;
    PIO_WORKITEM wi=NULL;
    PFILE_OBJECT file=NULL;
    PDEVICE_OBJECT owner=NULL;
    BOOLEAN do_release=FALSE,val_valid=FALSE;
    UCHAR addr=0;
    USHORT val16=0;
    ULONG ep=0,tok=0;
    KIRQL irql;
    UNREFERENCED_PARAMETER(DeviceObject);

    KeAcquireSpinLock(&state->Lock,&irql);
    wi=state->CompensateWorkItem;state->CompensateWorkItem=NULL;
    file=state->CompensateFile;state->CompensateFile=NULL;
    owner=state->CompensateOwner;state->CompensateOwner=NULL;
    do_release=state->CompensateRunRelease;state->CompensateRunRelease=FALSE;
    val_valid=state->DeferReleaseValueValid;state->DeferReleaseValueValid=FALSE;
    addr=state->DeferReleaseAddress;
    val16=state->DeferReleaseValue;
    ep=state->DeferReleaseEpoch;
    tok=state->DeferReleaseToken ? state->DeferReleaseToken : state->ProviderToken;
    state->CompensateQueued=FALSE;
    KeReleaseSpinLock(&state->Lock,irql);

    if(do_release && addr && InterlockedCompareExchange(&g_InFlightNewClaims,0,0)==0) {
        ULONG cur_ep=0;BOOLEAN cur_iso=FALSE;
        const BOOLEAN cur_active=query_unresolved_rail_ex(&cur_ep,NULL,&cur_iso,NULL,NULL,NULL);
        if(cur_active && cur_ep==ep) {
            if(cur_iso) {
                if(!val_valid && file && owner) {
                    struct tg_gpu_pmic_request r_en={addr,0x06u,0,0};
                    struct tg_gpu_pmic_request r_vo={addr,0x23u,0,0};
                    if(NT_SUCCESS(send_pmic_irp(NULL,0,0,0,file,owner,&r_en,FALSE,0)) &&
                       NT_SUCCESS(send_pmic_irp(NULL,0,0,0,file,owner,&r_vo,FALSE,0))) {
                        val16=(USHORT)((((UINT32)(UINT8)r_en.value)<<8)|(UINT8)r_vo.value);
                        val_valid=TRUE;
                    }
                }
                ULONG chk_ep=0;BOOLEAN chk_iso=FALSE;
                if(InterlockedCompareExchange(&g_InFlightNewClaims,0,0)==0 &&
                   query_unresolved_rail_ex(&chk_ep,NULL,&chk_iso,NULL,NULL,NULL) &&
                   chk_ep==ep && chk_iso) {
                    ULONG op_seq=0,b_epoch=0;
                    BOOLEAN live_state=FALSE;
                    if(val_valid && file && owner) {
                        KeAcquireSpinLock(&state->Lock,&irql);
                        if(!state->Closed && state->ActiveFile==file &&
                           state->ClaimEpoch==ep && !state->InFlightClaimSeq) {
                            op_seq=++state->LastRailOpSeq;
                            b_epoch=state->BridgeEpoch;
                            state->LastRailOpState=TgRailOpInFlight;
                            live_state=TRUE;
                        }
                        KeReleaseSpinLock(&state->Lock,irql);
                    }
                    if(live_state) {
                        const UINT32 full_val=(InterlockedCompareExchange(&g_ProviderTokenAware,0,0) && tok) ?
                            (((UINT32)(tok&0xffffu)<<16)|val16) : (UINT32)val16;
                        struct tg_gpu_pmic_request rel={addr,TG_GPU_PMIC_RELEASE_RAIL,1,full_val};
                        (VOID)send_pmic_irp(state,b_epoch,ep,op_seq,file,owner,&rel,FALSE,0);
                    } else {
                        (VOID)TgGpuRetryUnresolvedRailRelease();
                    }
                }
            } else if(file && owner) {
                /* A05 revoke: hardware isolation was revoked before compensation
                 * ran; retain unresolved accountability with isolated=FALSE! */
                notify_provider_handoff_via_file(file,owner,tok?tok:ep,addr,TRUE,FALSE);
            } else {
                notify_provider_handoff_standalone(tok?tok:ep,addr,TRUE,FALSE);
            }
        }
    }
    if(wi)IoFreeWorkItem(wi);
    if(owner)ObfDereferenceObject(owner);
    if(file)ObfDereferenceObject(file);
    release_rail_state(state);
}

/* Single-owner terminal receipt reconciliation under state->Lock (A05, V7-01):
 * handles normal, synchronous, cancel-time, suppressed-after-timeout, and
 * post-close completions without ever touching TG_GPU_PLATFORM memory when
 * state->Closed is TRUE (N1). */
static PIO_WORKITEM reconcile_rail_completion_locked(TG_PLATFORM_RAIL_STATE *state,TG_PLATFORM_IRP_CTX *ctx)
{
    const BOOLEAN suppressed=(InterlockedCompareExchange(&ctx->SuppressedAfterTimeout,0,0)!=0);
    const BOOLEAN ok=!suppressed && NT_SUCCESS(ctx->Status) && ctx->Information==sizeof(ctx->Buffer);
    ULONG provider_tok=state->ProviderToken;
    PIO_WORKITEM ret_wi=NULL;

    if(state->InFlightClaimSeq==ctx->RailOpSeq)
        state->InFlightClaimSeq=0;

    if(ok && ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL && (ctx->Buffer.address & TG_GPU_RAIL_STATE_ACK)) {
        provider_tok=ctx->Buffer.address & 0xffffu;
        InterlockedExchange(&g_ProviderTokenAware,1);
        InterlockedExchange(&g_ProviderRailSynced,1);
        advance_claim_epoch_floor(provider_tok);
    }

    if(state->LastRailOpSeq!=ctx->RailOpSeq) {
        if(!state->InFlightClaimSeq && !state->DeferReleasePending)
            ret_wi=schedule_compensate_locked(state,FALSE);
        goto out_claim_cnt;
    }

    if(state->Closed) {
        if(ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL) {
            if(ok) {
                state->ProviderToken=provider_tok;
                state->LastRailOpState=TgRailOpSucceeded;
                state->EmcRailOwner=TRUE;
                state->RailClaimed=TRUE;
                state->RailOwnershipUncertain=FALSE;
                record_unresolved_rail_epoch_ex(ctx->ClaimEpoch,provider_tok,
                    state->LastPmicAddress,state->ClosedGpuIsolated,TRUE,TRUE,STATUS_UNSUCCESSFUL);
                if(state->DeferReleasePending) {
                    state->DeferReleaseToken=provider_tok;
                    ret_wi=schedule_compensate_locked(state,TRUE);
                } else {
                    ret_wi=schedule_compensate_locked(state,FALSE);
                }
            } else if(!state->RailClaimed) {
                state->LastRailOpState=TgRailOpFailed;
                state->RailOwnershipUncertain=FALSE;
                clear_unresolved_rail_epoch(ctx->ClaimEpoch);
                ret_wi=schedule_compensate_locked(state,FALSE);
            } else {
                state->LastRailOpState=TgRailOpFailed;
                if(state->DeferReleasePending) {
                    state->DeferReleaseToken=state->ProviderToken;
                    ret_wi=schedule_compensate_locked(state,TRUE);
                } else {
                    ret_wi=schedule_compensate_locked(state,FALSE);
                }
            }
        } else if(ctx->Reg==TG_GPU_PMIC_RELEASE_RAIL && ok) {
            state->LastRailOpState=TgRailOpSucceeded;
            state->RailClaimed=FALSE;
            state->RailOwnershipUncertain=FALSE;
            state->ProviderToken=0;
            clear_unresolved_rail_epoch(ctx->ClaimEpoch);
            ret_wi=schedule_compensate_locked(state,FALSE);
        }
        goto out_claim_cnt;
    }

    if(state->ActiveFile!=ctx->FileObject || state->BridgeEpoch!=ctx->BridgeEpoch) {
        ret_wi=schedule_compensate_locked(state,FALSE);
        goto out_claim_cnt;
    }

    if(ok) {
        state->LastRailOpState=TgRailOpSucceeded;
        if(ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL) {
            if(ctx->Buffer.address & TG_GPU_RAIL_STATE_ACK)state->RailAbi=TgRailAbiToken;
            state->ClaimEpoch=ctx->ClaimEpoch;
            state->ProviderToken=provider_tok;
            state->EmcRailOwner=TRUE;
            state->RailClaimed=TRUE;
            state->NeedReclaim=FALSE;
            if(state->DeferReleasePending) {
                state->RailOwnershipUncertain=TRUE;
                state->DeferReleaseToken=provider_tok;
                mirror_rail_state_locked(state);
                record_unresolved_rail_epoch_ex(ctx->ClaimEpoch,provider_tok,
                    state->DeferReleaseAddress?state->DeferReleaseAddress:state->LastPmicAddress,
                    state->ClosedGpuIsolated,FALSE,TRUE,STATUS_IO_TIMEOUT);
                ret_wi=schedule_compensate_locked(state,TRUE);
                goto out_claim_cnt;
            }
            state->RailOwnershipUncertain=FALSE;
            supersede_older_unresolved_rail(ctx->ClaimEpoch);
            ret_wi=schedule_compensate_locked(state,FALSE);
        } else {
            state->RailClaimed=FALSE;
            state->RailOwnershipUncertain=FALSE;
            state->NeedReclaim=FALSE;
            state->ProviderToken=0;
            clear_unresolved_rail_epoch(ctx->ClaimEpoch);
            state->ClaimEpoch=0;
            ret_wi=schedule_compensate_locked(state,FALSE);
        }
        mirror_rail_state_locked(state);
    } else {
        state->LastRailOpState=TgRailOpFailed;
        if(ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL) {
            if(!state->RailClaimed) {
                state->RailOwnershipUncertain=FALSE;
                clear_unresolved_rail_epoch(ctx->ClaimEpoch);
                mirror_rail_state_locked(state);
                ret_wi=schedule_compensate_locked(state,FALSE);
            } else if(state->DeferReleasePending) {
                state->RailOwnershipUncertain=TRUE;
                state->DeferReleaseToken=state->ProviderToken;
                mirror_rail_state_locked(state);
                ret_wi=schedule_compensate_locked(state,TRUE);
            } else {
                state->RailOwnershipUncertain=FALSE;
                mirror_rail_state_locked(state);
                ret_wi=schedule_compensate_locked(state,FALSE);
            }
        } else if(ctx->Reg==TG_GPU_PMIC_RELEASE_RAIL && state->EmcRailOwner &&
           (state->RailClaimed || state->RailOwnershipUncertain)) {
            state->RailOwnershipUncertain=TRUE;
            mirror_rail_state_locked(state);
        }
    }

out_claim_cnt:
    /* Decrement AFTER supersede_older_unresolved_rail so zero window exists
     * between g_InFlightNewClaims > 0 and updated EpochWatermark! */
    if(InterlockedExchange(&ctx->ClaimCounted,0)==1)
        InterlockedDecrement(&g_InFlightNewClaims);
    return ret_wi;
}

static NTSTATUS NTAPI platform_irp_complete(PDEVICE_OBJECT Device,PIRP Irp,PVOID Context)
{
    TG_PLATFORM_IRP_CTX *ctx=Context;
    TG_PLATFORM_RAIL_STATE *state=ctx->RailState;
    PIO_WORKITEM comp_wi=NULL;
    UNREFERENCED_PARAMETER(Device);
    InterlockedExchange(&ctx->Completed,1);
    ctx->Status=Irp->IoStatus.Status;
    ctx->Information=Irp->IoStatus.Information;
    /* Single-owner handoff with send_pmic_irp under state->Lock for CLAIM/RELEASE
     * so timeout uncertain-marking and completion receipts never overwrite each other (A05). */
    if(state && (ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL || ctx->Reg==TG_GPU_PMIC_RELEASE_RAIL)) {
        KIRQL irql;
        KeAcquireSpinLock(&state->Lock,&irql);
        if(ctx->OwnerState==TgPlatformIrpInFlight)
            ctx->OwnerState=TgPlatformIrpCompletedSync;
        comp_wi=reconcile_rail_completion_locked(state,ctx);
        KeReleaseSpinLock(&state->Lock,irql);
        if(comp_wi)
            IoQueueWorkItem(comp_wi,platform_compensate_work,DelayedWorkQueue,state);
    } else {
        (VOID)InterlockedCompareExchange(&ctx->OwnerState,TgPlatformIrpCompletedSync,TgPlatformIrpInFlight);
    }
    KeSetEvent(&ctx->Done,IO_NO_INCREMENT,FALSE);
    release_irp_ctx(ctx);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID abort_undispatched_rail_op(TG_PLATFORM_RAIL_STATE *state,ULONG epoch,
                                       ULONG rail_op_seq,PFILE_OBJECT file,ULONG reg)
{
    KIRQL irql;
    PIO_WORKITEM comp_wi=NULL;
    if(!state || !rail_op_seq)return;
    KeAcquireSpinLock(&state->Lock,&irql);
    if(state->InFlightClaimSeq==rail_op_seq)
        state->InFlightClaimSeq=0;
    if(!state->Closed && state->ActiveFile==file && state->BridgeEpoch==epoch &&
       state->LastRailOpSeq==rail_op_seq && state->LastRailOpState==TgRailOpInFlight) {
        state->LastRailOpState=TgRailOpFailed;
        if(reg==TG_GPU_PMIC_CLAIM_RAIL && !state->RailClaimed) {
            state->RailOwnershipUncertain=FALSE;
            clear_unresolved_rail_epoch(state->ClaimEpoch);
            mirror_rail_state_locked(state);
            comp_wi=schedule_compensate_locked(state,FALSE);
        } else if(reg==TG_GPU_PMIC_CLAIM_RAIL && state->DeferReleasePending) {
            state->DeferReleaseToken=state->ProviderToken;
            comp_wi=schedule_compensate_locked(state,TRUE);
        } else if(reg==TG_GPU_PMIC_RELEASE_RAIL && state->EmcRailOwner &&
           (state->RailClaimed || state->RailOwnershipUncertain)) {
            state->RailOwnershipUncertain=TRUE;
            mirror_rail_state_locked(state);
        }
    } else if(!state->InFlightClaimSeq) {
        comp_wi=schedule_compensate_locked(state,state->RailClaimed && state->DeferReleasePending);
    }
    KeReleaseSpinLock(&state->Lock,irql);
    if(comp_wi)
        IoQueueWorkItem(comp_wi,platform_compensate_work,DelayedWorkQueue,state);
}

/* Independent execution context for deadline-bounded IRPs (A04, V7-01):
 * dispatches IoCallDriver off the caller's thread so even a synchronous SPB/PMIC
 * stall inside IoCallDriver can never delay TgGpuPlatformPreflight past 600 ms.
 * DispatchState machine:
 *   0 -> queued (caller timeout transitions 0 -> 2 to abort before registration)
 *   1 -> worker registering IoSetCompletionRoutineEx (caller timeout transitions
 *        1 -> 4; worker's CAS 1 -> 3 then fails and suppresses CLAIM/RELEASE
 *        by substituting a side-effect-free QUERY_ADDRESS so 0 out-of-order rail
 *        commands ever reach the provider after timeout/AbandonRail!)
 *   3 -> committed to IoCallDriver (caller timeout transitions 3 -> 4)
 *   5 -> IoCallDriver returned */
static VOID NTAPI platform_dispatch_work(PDEVICE_OBJECT DeviceObject,PVOID Context)
{
    TG_PLATFORM_IRP_CTX *ctx=Context;
    PIO_WORKITEM work_item=ctx->WorkItem;
    PDEVICE_OBJECT owner_dev=ctx->OwnerDevice;
    UNREFERENCED_PARAMETER(DeviceObject);
    ctx->WorkItem=NULL;
    ctx->OwnerDevice=NULL;
    if(InterlockedCompareExchange(&ctx->DispatchState,1,0)!=0) {
        /* Caller timed out before dispatch started; never call IoCallDriver. */
        IoFreeWorkItem(work_item);
        if(owner_dev)ObfDereferenceObject(owner_dev);
        release_irp_ctx(ctx);
        release_irp_ctx(ctx);
        return;
    }
    NTSTATUS st=IoSetCompletionRoutineEx(owner_dev,ctx->Irp,
        platform_irp_complete,ctx,TRUE,TRUE,TRUE);
    if(!NT_SUCCESS(st)) {
        PIO_WORKITEM comp_wi=NULL;
        InterlockedExchange(&ctx->Completed,1);
        ctx->Status=st;
        ctx->Information=0;
        if(ctx->RailState && (ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL || ctx->Reg==TG_GPU_PMIC_RELEASE_RAIL)) {
            KIRQL irql;
            KeAcquireSpinLock(&ctx->RailState->Lock,&irql);
            if(ctx->OwnerState==TgPlatformIrpInFlight)
                ctx->OwnerState=TgPlatformIrpCompletedSync;
            comp_wi=reconcile_rail_completion_locked(ctx->RailState,ctx);
            KeReleaseSpinLock(&ctx->RailState->Lock,irql);
            if(comp_wi)
                IoQueueWorkItem(comp_wi,platform_compensate_work,DelayedWorkQueue,ctx->RailState);
        } else {
            (VOID)InterlockedCompareExchange(&ctx->OwnerState,TgPlatformIrpCompletedSync,TgPlatformIrpInFlight);
        }
        (VOID)InterlockedExchange(&ctx->DispatchState,5);
        KeSetEvent(&ctx->Done,IO_NO_INCREMENT,FALSE);
        IoFreeWorkItem(work_item);
        if(owner_dev)ObfDereferenceObject(owner_dev);
        release_irp_ctx(ctx);
        release_irp_ctx(ctx);
        return;
    }

    if(ctx->RailState && (ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL || ctx->Reg==TG_GPU_PMIC_RELEASE_RAIL)) {
        KIRQL irql;
        KeAcquireSpinLock(&ctx->RailState->Lock,&irql);
        if(InterlockedCompareExchange(&ctx->DispatchState,3,1)!=1 ||
           ctx->RailState->Closed ||
           ctx->RailState->ActiveFile!=ctx->FileObject ||
           ctx->RailState->BridgeEpoch!=ctx->BridgeEpoch ||
           ctx->RailState->LastRailOpSeq!=ctx->RailOpSeq ||
           ctx->RailState->LastRailOpState!=TgRailOpInFlight) {
            /* V7-01: Caller already timed out or superseded this op before
             * IoCallDriver! Suppress state-changing rail command atomically. */
            InterlockedExchange(&ctx->SuppressedAfterTimeout,1);
            ctx->Buffer=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_QUERY_ADDRESS,0,0};
        } else if(ctx->Reg==TG_GPU_PMIC_CLAIM_RAIL) {
            ctx->RailState->InFlightClaimSeq=ctx->RailOpSeq;
        }
        KeReleaseSpinLock(&ctx->RailState->Lock,irql);
    } else if(InterlockedCompareExchange(&ctx->DispatchState,3,1)!=1) {
        InterlockedExchange(&ctx->SuppressedAfterTimeout,1);
    }

    /* V7-01: Keep InFlightClaimSeq armed across STATUS_PENDING and IoCancelIrp
     * until platform_irp_complete actually reconciles the IRP's terminal state! */
    (VOID)IoCallDriver(ctx->TargetDevice,ctx->Irp);
    if(InterlockedExchange(&ctx->DispatchState,5)==4 &&
       InterlockedCompareExchange(&ctx->Completed,0,0)==0) {
        (VOID)IoCancelIrp(ctx->Irp);
    }
    IoFreeWorkItem(work_item);
    if(owner_dev)ObfDereferenceObject(owner_dev);
    release_irp_ctx(ctx);
}

static NTSTATUS send_pmic_irp(TG_PLATFORM_RAIL_STATE *state,ULONG epoch,
                              ULONG claim_epoch,ULONG rail_op_seq,
                              PFILE_OBJECT file,PDEVICE_OBJECT owner_dev,
                              struct tg_gpu_pmic_request *op,
                              BOOLEAN has_deadline,ULONGLONG deadline)
{
    PDEVICE_OBJECT target_dev;TG_PLATFORM_IRP_CTX *ctx;PIO_STACK_LOCATION stack;
    LARGE_INTEGER timeout;NTSTATUS status,wait;ULONG_PTR info;
    const BOOLEAN rail_op=(op->reg==TG_GPU_PMIC_CLAIM_RAIL || op->reg==TG_GPU_PMIC_RELEASE_RAIL);
    const BOOLEAN count_claim=(state!=NULL && op->reg==TG_GPU_PMIC_CLAIM_RAIL);
    if(count_claim)InterlockedIncrement(&g_InFlightNewClaims);
    if(has_deadline && KeQueryInterruptTime()>=deadline) {
        if(count_claim)InterlockedDecrement(&g_InFlightNewClaims);
        if(rail_op)abort_undispatched_rail_op(state,epoch,rail_op_seq,file,op->reg);
        return STATUS_IO_TIMEOUT;
    }
    /* A05 retry_new_claim: final pre-dispatch gate for standalone compensation
     * RELEASE_RAIL so a concurrent new CLAIM_RAIL or revoked isolation aborts
     * before IoCallDriver is ever invoked! */
    if(!state && op->reg==TG_GPU_PMIC_RELEASE_RAIL && claim_epoch!=0) {
        if(InterlockedCompareExchange(&g_InFlightNewClaims,0,0)>0)
            return STATUS_DEVICE_BUSY;
        ULONG chk_epoch=0;BOOLEAN chk_iso=FALSE;
        const BOOLEAN chk_active=query_unresolved_rail_ex(&chk_epoch,NULL,&chk_iso,NULL,NULL,NULL);
        if(!chk_active || chk_epoch!=claim_epoch)
            return STATUS_SUCCESS;
        if(!chk_iso)
            return STATUS_DEVICE_NOT_READY;
    }
    target_dev=IoGetRelatedDeviceObject(file);
    if(!target_dev || !owner_dev) {
        if(count_claim)InterlockedDecrement(&g_InFlightNewClaims);
        if(rail_op)abort_undispatched_rail_op(state,epoch,rail_op_seq,file,op->reg);
        return STATUS_INVALID_DEVICE_STATE;
    }
    ctx=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*ctx),TG_PLATFORM_TAG);
    if(!ctx) {
        if(count_claim)InterlockedDecrement(&g_InFlightNewClaims);
        if(rail_op)abort_undispatched_rail_op(state,epoch,rail_op_seq,file,op->reg);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ctx->Irp=IoAllocateIrp(target_dev->StackSize,FALSE);
    if(!ctx->Irp){
        ExFreePoolWithTag(ctx,TG_PLATFORM_TAG);
        if(count_claim)InterlockedDecrement(&g_InFlightNewClaims);
        if(rail_op)abort_undispatched_rail_op(state,epoch,rail_op_seq,file,op->reg);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ObfReferenceObject(file);
    if(has_deadline)ObfReferenceObject(owner_dev);
    if(state)InterlockedIncrement(&state->RefCount);
    ctx->FileObject=file;
    ctx->TargetDevice=target_dev;
    ctx->OwnerDevice=has_deadline ? owner_dev : NULL;
    ctx->WorkItem=NULL;
    ctx->RailState=state;
    ctx->BridgeEpoch=epoch;
    ctx->ClaimEpoch=claim_epoch;
    ctx->Reg=op->reg;
    ctx->RailOpSeq=rail_op_seq;
    ctx->RefCount=has_deadline ? 3 : 2;
    ctx->OwnerState=TgPlatformIrpInFlight;
    ctx->DispatchState=has_deadline ? 0 : 3;
    ctx->Completed=0;
    ctx->SuppressedAfterTimeout=0;
    ctx->ClaimCounted=count_claim ? 1 : 0;
    ctx->Status=STATUS_UNSUCCESSFUL;
    ctx->Information=0;
    ctx->Buffer=*op;
    KeInitializeEvent(&ctx->Done,NotificationEvent,FALSE);
    ctx->Irp->RequestorMode=KernelMode;
    ctx->Irp->AssociatedIrp.SystemBuffer=&ctx->Buffer;
    ctx->Irp->IoStatus.Status=STATUS_NOT_SUPPORTED;
    ctx->Irp->IoStatus.Information=0;
    stack=IoGetNextIrpStackLocation(ctx->Irp);
    stack->MajorFunction=IRP_MJ_DEVICE_CONTROL;
    stack->FileObject=file;
    stack->Parameters.DeviceIoControl.IoControlCode=TG_GPU_IOCTL_PMIC;
    stack->Parameters.DeviceIoControl.InputBufferLength=sizeof(ctx->Buffer);
    stack->Parameters.DeviceIoControl.OutputBufferLength=sizeof(ctx->Buffer);

    BOOLEAN deadline_exceeded=FALSE;
    if(has_deadline) {
        ctx->WorkItem=IoAllocateWorkItem(owner_dev);
        if(!ctx->WorkItem) {
            if(InterlockedExchange(&ctx->ClaimCounted,0)==1)
                InterlockedDecrement(&g_InFlightNewClaims);
            IoFreeIrp(ctx->Irp);
            ObfDereferenceObject(owner_dev);
            ObfDereferenceObject(file);
            release_rail_state(state);
            ExFreePoolWithTag(ctx,TG_PLATFORM_TAG);
            if(rail_op)abort_undispatched_rail_op(state,epoch,rail_op_seq,file,op->reg);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        IoQueueWorkItem(ctx->WorkItem,platform_dispatch_work,DelayedWorkQueue,ctx);
        ULONGLONG now=KeQueryInterruptTime();
        if(now>=deadline) {
            deadline_exceeded=TRUE;
            wait=STATUS_TIMEOUT;
        } else {
            ULONGLONG rem=deadline-now;
            if(rem>(ULONGLONG)(-TG_PLATFORM_TIMEOUT_100NS))
                rem=(ULONGLONG)(-TG_PLATFORM_TIMEOUT_100NS);
            timeout.QuadPart=-(LONGLONG)rem;
            wait=KeWaitForSingleObject(&ctx->Done,Executive,KernelMode,FALSE,&timeout);
            /* Re-read monotonic clock immediately on wake so both completion-before-timeout
             * and timeout-before-completion report STATUS_IO_TIMEOUT past budget (A04). */
            if(KeQueryInterruptTime()>=deadline)
                deadline_exceeded=TRUE;
        }
    } else {
        status=IoSetCompletionRoutineEx(owner_dev,ctx->Irp,platform_irp_complete,ctx,TRUE,TRUE,TRUE);
        if(!NT_SUCCESS(status)) {
            if(InterlockedExchange(&ctx->ClaimCounted,0)==1)
                InterlockedDecrement(&g_InFlightNewClaims);
            IoFreeIrp(ctx->Irp);
            ObfDereferenceObject(file);
            release_rail_state(state);
            ExFreePoolWithTag(ctx,TG_PLATFORM_TAG);
            if(rail_op)abort_undispatched_rail_op(state,epoch,rail_op_seq,file,op->reg);
            return status;
        }
        /* The ordinary reconnect/reclaim path can also return STATUS_PENDING.
         * Track the claim until its terminal receipt, just like the worker path. */
        if(state && rail_op) {
            KIRQL irql;
            KeAcquireSpinLock(&state->Lock,&irql);
            if(state->Closed || state->ActiveFile!=file || state->BridgeEpoch!=epoch ||
               state->LastRailOpSeq!=rail_op_seq || state->LastRailOpState!=TgRailOpInFlight) {
                InterlockedExchange(&ctx->SuppressedAfterTimeout,1);
                ctx->Buffer=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_QUERY_ADDRESS,0,0};
            } else if(op->reg==TG_GPU_PMIC_CLAIM_RAIL) {
                state->InFlightClaimSeq=rail_op_seq;
            }
            KeReleaseSpinLock(&state->Lock,irql);
        }
        (VOID)IoCallDriver(target_dev,ctx->Irp);
        timeout.QuadPart=TG_PLATFORM_TIMEOUT_100NS;
        wait=KeWaitForSingleObject(&ctx->Done,Executive,KernelMode,FALSE,&timeout);
    }
    if(wait!=STATUS_SUCCESS) {
        if(has_deadline && InterlockedCompareExchange(&ctx->DispatchState,2,0)==0) {
            if(InterlockedExchange(&ctx->ClaimCounted,0)==1)
                InterlockedDecrement(&g_InFlightNewClaims);
            if(rail_op)abort_undispatched_rail_op(state,epoch,rail_op_seq,file,op->reg);
            release_irp_ctx(ctx);
            return (wait==STATUS_TIMEOUT || NT_SUCCESS(wait))?STATUS_IO_TIMEOUT:wait;
        }
        BOOLEAN suppressed_pre_call=FALSE;
        BOOLEAN claimed_timeout=FALSE;
        if(state && rail_op) {
            KIRQL irql;
            PIO_WORKITEM pre_comp_wi=NULL;
            KeAcquireSpinLock(&state->Lock,&irql);
            if(has_deadline && InterlockedCompareExchange(&ctx->DispatchState,4,1)==1) {
                /* V7-01: Worker has NOT called IoCallDriver yet and will suppress
                 * the rail op under state->Lock upon seeing DispatchState==4! */
                suppressed_pre_call=TRUE;
                if(state->InFlightClaimSeq==rail_op_seq)
                    state->InFlightClaimSeq=0;
                if(!state->Closed && state->ActiveFile==file && state->BridgeEpoch==epoch &&
                   state->LastRailOpSeq==rail_op_seq && state->LastRailOpState==TgRailOpInFlight) {
                    state->LastRailOpState=TgRailOpFailed;
                    if(op->reg==TG_GPU_PMIC_CLAIM_RAIL && !state->RailClaimed) {
                        state->RailOwnershipUncertain=FALSE;
                        clear_unresolved_rail_epoch(state->ClaimEpoch);
                        mirror_rail_state_locked(state);
                        pre_comp_wi=schedule_compensate_locked(state,FALSE);
                    } else if(op->reg==TG_GPU_PMIC_CLAIM_RAIL && state->DeferReleasePending) {
                        state->DeferReleaseToken=state->ProviderToken;
                        pre_comp_wi=schedule_compensate_locked(state,TRUE);
                    } else if(op->reg==TG_GPU_PMIC_RELEASE_RAIL && state->EmcRailOwner &&
                       (state->RailClaimed || state->RailOwnershipUncertain)) {
                        state->RailOwnershipUncertain=TRUE;
                        mirror_rail_state_locked(state);
                    }
                }
            } else if(ctx->OwnerState==TgPlatformIrpInFlight) {
                ctx->OwnerState=TgPlatformIrpTimedOut;
                claimed_timeout=TRUE;
                if(!state->Closed && state->ActiveFile==file &&
                   state->BridgeEpoch==epoch && state->LastRailOpSeq==rail_op_seq &&
                   state->LastRailOpState==TgRailOpInFlight) {
                    if(op->reg==TG_GPU_PMIC_CLAIM_RAIL) {
                        state->ClaimEpoch=claim_epoch;
                        state->EmcRailOwner=TRUE;
                    }
                    state->RailOwnershipUncertain=TRUE;
                    mirror_rail_state_locked(state);
                }
            }
            KeReleaseSpinLock(&state->Lock,irql);
            if(pre_comp_wi)
                IoQueueWorkItem(pre_comp_wi,platform_compensate_work,DelayedWorkQueue,state);
            if(suppressed_pre_call) {
                if(InterlockedExchange(&ctx->ClaimCounted,0)==1)
                    InterlockedDecrement(&g_InFlightNewClaims);
                release_irp_ctx(ctx);
                return (wait==STATUS_TIMEOUT || NT_SUCCESS(wait))?STATUS_IO_TIMEOUT:wait;
            }
        } else {
            if(has_deadline && InterlockedCompareExchange(&ctx->DispatchState,4,1)==1) {
                release_irp_ctx(ctx);
                return (wait==STATUS_TIMEOUT || NT_SUCCESS(wait))?STATUS_IO_TIMEOUT:wait;
            }
            claimed_timeout=(InterlockedCompareExchange(&ctx->OwnerState,
                TgPlatformIrpTimedOut,TgPlatformIrpInFlight)==TgPlatformIrpInFlight);
        }
        if(claimed_timeout) {
            if(!has_deadline) {
                (VOID)IoCancelIrp(ctx->Irp);
            } else {
                LONG prev_disp=InterlockedCompareExchange(&ctx->DispatchState,4,3);
                if(prev_disp==5 && InterlockedCompareExchange(&ctx->Completed,0,0)==0)
                    (VOID)IoCancelIrp(ctx->Irp);
            }
            release_irp_ctx(ctx);
            return (wait==STATUS_TIMEOUT || NT_SUCCESS(wait))?STATUS_IO_TIMEOUT:wait;
        }
        if(deadline_exceeded || (has_deadline && KeQueryInterruptTime()>=deadline)) {
            release_irp_ctx(ctx);
            return STATUS_IO_TIMEOUT;
        }
    } else if(deadline_exceeded || (has_deadline && KeQueryInterruptTime()>=deadline)) {
        if(NT_SUCCESS(ctx->Status) && ctx->Information==sizeof(*op))*op=ctx->Buffer;
        release_irp_ctx(ctx);
        return STATUS_IO_TIMEOUT;
    }
    status=ctx->Status;
    info=ctx->Information;
    if(NT_SUCCESS(status) && info==sizeof(*op))*op=ctx->Buffer;
    release_irp_ctx(ctx);
    if(status!=STATUS_SUCCESS && NT_SUCCESS(status))status=STATUS_DEVICE_PROTOCOL_ERROR;
    if(NT_SUCCESS(status) && info!=sizeof(*op))status=STATUS_DEVICE_PROTOCOL_ERROR;
    return status;
}

static NTSTATUS platform_request_deadline(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op,
                                          BOOLEAN optional,ULONG *vote_generation,
                                          BOOLEAN has_deadline,ULONGLONG deadline)
{
    PFILE_OBJECT file=NULL;PDEVICE_OBJECT owner_dev=NULL;
    TG_PLATFORM_RAIL_STATE *state=NULL;
    BOOLEAN reclaim=FALSE;KIRQL irql,state_irql;NTSTATUS status;
    ULONG epoch=0,generation=0,rail_op_seq=0;
    ULONG reg;
    PAGED_CODE();
    if(!p || !op || !p->Resources)return STATUS_INVALID_PARAMETER;
    if(has_deadline && KeQueryInterruptTime()>=deadline)
        return optional?STATUS_IO_TIMEOUT:failure(p,STATUS_IO_TIMEOUT);
    reg=op->reg;
    KeAcquireSpinLock(&p->HandleLock,&irql);
    file=(PFILE_OBJECT)p->FileObject;
    owner_dev=(PDEVICE_OBJECT)p->OwnerDevice;
    state=(TG_PLATFORM_RAIL_STATE *)p->RailState;
    if(!p->Closing && p->Bridge && file && state) {
        epoch=p->BridgeEpoch;
        generation=p->ProviderGeneration;
        ObfReferenceObject(file);
        if(owner_dev)ObfReferenceObject(owner_dev);
        InterlockedIncrement(&state->RefCount);
    } else {
        file=NULL;owner_dev=NULL;state=NULL;
    }
    KeReleaseSpinLock(&p->HandleLock,irql);

    if(!file) {
        status=has_deadline ? open_bridge_deadline(p,deadline) : open_bridge_internal(p);
        if(!NT_SUCCESS(status))return optional?status:failure(p,status);
        if(has_deadline && KeQueryInterruptTime()>=deadline)
            return optional?STATUS_IO_TIMEOUT:failure(p,STATUS_IO_TIMEOUT);
        KeAcquireSpinLock(&p->HandleLock,&irql);
        file=(PFILE_OBJECT)p->FileObject;
        owner_dev=(PDEVICE_OBJECT)p->OwnerDevice;
        state=(TG_PLATFORM_RAIL_STATE *)p->RailState;
        if(!p->Closing && p->Bridge && file && state) {
            epoch=p->BridgeEpoch;
            generation=p->ProviderGeneration;
            ObfReferenceObject(file);
            if(owner_dev)ObfReferenceObject(owner_dev);
            InterlockedIncrement(&state->RefCount);
        } else {
            file=NULL;owner_dev=NULL;state=NULL;
        }
        KeReleaseSpinLock(&p->HandleLock,irql);
        if(!file)return optional?STATUS_DEVICE_NOT_READY:failure(p,STATUS_DEVICE_NOT_READY);
    }

    KeAcquireSpinLock(&state->Lock,&state_irql);
    if(state->Closed || state->ActiveFile!=file || state->BridgeEpoch!=epoch) {
        KeReleaseSpinLock(&state->Lock,state_irql);
        status=STATUS_DEVICE_NOT_READY;
        goto finished;
    }
    /* Do not replace a pending claim with a second reclaim or overtake it
     * with RELEASE. In particular AbandonRail's PMIC reads must not start a
     * fresh reclaim while the previous ordinary request is still pending. */
    if(state->InFlightClaimSeq && (state->NeedReclaim ||
       reg==TG_GPU_PMIC_CLAIM_RAIL || reg==TG_GPU_PMIC_RELEASE_RAIL)) {
        KeReleaseSpinLock(&state->Lock,state_irql);
        status=STATUS_DEVICE_BUSY;
        goto finished;
    }
    reclaim=state->NeedReclaim && reg!=TG_GPU_PMIC_CLAIM_RAIL && reg!=TG_GPU_PMIC_RELEASE_RAIL;
    KeReleaseSpinLock(&state->Lock,state_irql);

    /* file and generation describe the same referenced endpoint, even if PnP
     * replaces the device immediately after HandleLock was released. */
    if(vote_generation && reg!=TG_GPU_PMIC_QUERY_EMC_VOTE && *vote_generation!=generation) {
        status=STATUS_DEVICE_NOT_READY;
        goto finished;
    }
    if(reclaim) {
        ULONG reclaim_seq=0,reclaim_claim_epoch=0;
        BOOLEAN reclaim_busy=FALSE;
        KeAcquireSpinLock(&state->Lock,&state_irql);
        if(!state->Closed && state->ActiveFile==file && state->BridgeEpoch==epoch) {
            if(state->InFlightClaimSeq) {
                reclaim_busy=TRUE;
            } else {
                reclaim_seq=++state->LastRailOpSeq;
                reclaim_claim_epoch=next_rail_claim_epoch();
                state->InFlightClaimSeq=reclaim_seq;
                state->ClaimEpoch=reclaim_claim_epoch;
                state->ClosedGpuIsolated=FALSE;
                if(state->Owner)state->Owner->PreservedGpuIsolated=FALSE;
                state->LastRailOpState=TgRailOpInFlight;
                mirror_rail_state_locked(state);
            }
        }
        KeReleaseSpinLock(&state->Lock,state_irql);
        if(reclaim_busy){status=STATUS_DEVICE_BUSY;goto finished;}
        if(!reclaim_seq){status=STATUS_DEVICE_NOT_READY;goto finished;}
        ULONG req_tok=reclaim_claim_epoch & 0xffffu;
        if(!req_tok)req_tok=1u;
        struct tg_gpu_pmic_request claim={req_tok,TG_GPU_PMIC_CLAIM_RAIL,1,TG_GPU_EMC_VOTE_ABI};
        status=send_pmic_irp(state,epoch,reclaim_claim_epoch,reclaim_seq,file,owner_dev,&claim,has_deadline,deadline);
        if(NT_SUCCESS(status)) {
            KeAcquireSpinLock(&state->Lock,&state_irql);
            if(state->Closed || state->ActiveFile!=file || state->BridgeEpoch!=epoch)
                status=STATUS_DEVICE_NOT_READY;
            KeReleaseSpinLock(&state->Lock,state_irql);
        }
        if(!NT_SUCCESS(status))goto finished;
    }
    ULONG op_claim_epoch=0;
    struct tg_gpu_pmic_request local_op=*op;
    BOOLEAN legacy_swbatt=FALSE;
    if(reg==TG_GPU_PMIC_CLAIM_RAIL || reg==TG_GPU_PMIC_RELEASE_RAIL) {
        ULONG state_tok=0;
        BOOLEAN op_busy=FALSE;
        KeAcquireSpinLock(&state->Lock,&state_irql);
        if(!state->Closed && state->ActiveFile==file && state->BridgeEpoch==epoch) {
            if(state->InFlightClaimSeq) {
                op_busy=TRUE;
            } else {
                legacy_swbatt=state->RailAbi==TgRailAbiSwBattLegacy;
                rail_op_seq=++state->LastRailOpSeq;
                if(reg==TG_GPU_PMIC_CLAIM_RAIL) {
                    op_claim_epoch=next_rail_claim_epoch();
                    state->InFlightClaimSeq=rail_op_seq;
                    state->ClaimEpoch=op_claim_epoch;
                    state->DeferReleasePending=FALSE;
                    state->DeferReleaseValueValid=FALSE;
                    state->ClosedGpuIsolated=FALSE;
                    if(state->Owner)state->Owner->PreservedGpuIsolated=FALSE;
                    mirror_rail_state_locked(state);
                } else {
                    if(!state->ClaimEpoch) {
                        state->ClaimEpoch=next_rail_claim_epoch();
                        mirror_rail_state_locked(state);
                    }
                    op_claim_epoch=state->ClaimEpoch;
                    state_tok=state->ProviderToken;
                }
                state->LastRailOpState=TgRailOpInFlight;
            }
        }
        KeReleaseSpinLock(&state->Lock,state_irql);
        if(op_busy){status=STATUS_DEVICE_BUSY;goto finished;}
        if(!rail_op_seq){status=STATUS_DEVICE_NOT_READY;goto finished;}
        if(reg==TG_GPU_PMIC_CLAIM_RAIL) {
            if(!local_op.address && !legacy_swbatt) {
                local_op.address=op_claim_epoch & 0xffffu;
                if(!local_op.address)local_op.address=1u;
            }
        } else if(InterlockedCompareExchange(&g_ProviderTokenAware,0,0) &&
                  state_tok && (local_op.value>>16)==0u) {
            local_op.value=((UINT32)(state_tok&0xffffu)<<16)|(local_op.value&0xffffu);
        }
    }
    const BOOLEAN rail_query=reg==TG_GPU_PMIC_QUERY_ADDRESS &&
        !op->write && op->address==TG_GPU_RAIL_QUERY_MAGIC;
    BOOLEAN legacy_reply=FALSE;
    if(rail_query)
        status=query_provider_address(state,epoch,file,owner_dev,&local_op,has_deadline,deadline,&legacy_reply);
    else
        status=send_pmic_irp(state,epoch,op_claim_epoch,rail_op_seq,file,owner_dev,&local_op,has_deadline,deadline);
    if(NT_SUCCESS(status)) {
        *op=local_op;
        KeAcquireSpinLock(&state->Lock,&state_irql);
        /* A reply from an instance removed during this request cannot update
         * the new instance's ownership. Completion receipts for CLAIM/RELEASE
         * are reconciled exclusively inside platform_irp_complete (A05). */
        if(state->Closed || state->ActiveFile!=file || state->BridgeEpoch!=epoch) {
            status=STATUS_DEVICE_NOT_READY;
        } else if(legacy_reply) {
            /* Never downgrade a token-aware endpoint or a live/unresolved claim. */
            if(state->RailAbi==TgRailAbiToken || state->EmcRailOwner || state->RailOwnershipUncertain ||
               state->InFlightClaimSeq || InterlockedCompareExchange64(&g_UnresolvedRail.State64,0,0)!=0)
                status=STATUS_INVALID_DEVICE_STATE;
            else
                state->RailAbi=TgRailAbiSwBattLegacy;
        } else if(rail_query && (local_op.address & TG_GPU_RAIL_STATE_ACK)) {
            state->RailAbi=TgRailAbiToken;
        } else if(vote_generation && reg==TG_GPU_PMIC_QUERY_EMC_VOTE) {
            *vote_generation=generation;
        }
        KeReleaseSpinLock(&state->Lock,state_irql);
    }
finished:
    if(state)release_rail_state(state);
    if(owner_dev)ObfDereferenceObject(owner_dev);
    ObfDereferenceObject(file);
    return optional?status:failure(p,status);
}

NTSTATUS TgGpuPlatformRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op)
{ return platform_request_deadline(p,op,FALSE,NULL,FALSE,0); }
NTSTATUS TgGpuPlatformTryRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op)
{ return platform_request_deadline(p,op,TRUE,NULL,FALSE,0); }
NTSTATUS TgGpuPlatformTryVoteRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op,ULONG *generation)
{
    if(!generation || !op || (op->reg!=TG_GPU_PMIC_QUERY_EMC_VOTE &&
       op->reg!=TG_GPU_PMIC_REPORT_EMC_ACK && op->reg!=TG_GPU_PMIC_REPORT_EMC_FAILURE))
        return STATUS_INVALID_PARAMETER;
    return platform_request_deadline(p,op,TRUE,generation,FALSE,0);
}
static void delay(void *context,UINT32 us);

static NTSTATUS preflight_query_ready(TG_GPU_PLATFORM *p,ULONG reg,ULONG *value,
                                      ULONGLONG deadline,UINT *remaining_retries)
{
    NTSTATUS status=STATUS_DEVICE_NOT_READY;
    for(;;) {
        KIRQL irql;BOOLEAN abort;
        if(KeQueryInterruptTime()>=deadline){status=STATUS_IO_TIMEOUT;break;}
        struct tg_gpu_pmic_request request={
            (reg==TG_GPU_PMIC_QUERY_ADDRESS)?TG_GPU_RAIL_QUERY_MAGIC:0u,
            reg,0,0
        };
        status=platform_request_deadline(p,&request,TRUE,NULL,TRUE,deadline);
        if(NT_SUCCESS(status)) {
            if(reg==TG_GPU_PMIC_QUERY_ADDRESS)
                ingest_provider_rail_query(&request);
            *value=request.value;
            return STATUS_SUCCESS;
        }
        if(status!=STATUS_DEVICE_NOT_READY || !*remaining_retries)break;
        ULONGLONG now=KeQueryInterruptTime();
        if(now>=deadline){status=STATUS_IO_TIMEOUT;break;}
        KeAcquireSpinLock(&p->HandleLock,&irql);
        abort=p->Closing || p->QueryRemove || !p->Bridge || !p->FileObject;
        KeReleaseSpinLock(&p->HandleLock,irql);
        if(abort)break;
        ULONGLONG step_100ns=(ULONGLONG)TG_PLATFORM_READY_RETRY_US*10ULL;
        ULONGLONG left=deadline-now;
        if(left<step_100ns)step_100ns=left;
        if(step_100ns<10000ULL){status=STATUS_IO_TIMEOUT;break;}
        --(*remaining_retries);
        delay(p,(UINT32)(step_100ns/10ULL));
    }
    return failure(p,status);
}

NTSTATUS TgGpuPlatformPreflight(TG_GPU_PLATFORM *p,UCHAR *address)
{
    struct tg_gpu_pmic_request request;ULONG value=0;NTSTATUS status;
    ULONGLONG deadline;UINT remaining_retries=TG_PLATFORM_READY_RETRIES;
    if(!address)return STATUS_INVALID_PARAMETER;
    deadline=KeQueryInterruptTime()+TG_PLATFORM_PREFLIGHT_BUDGET_100NS;
    status=preflight_query_ready(p,TG_GPU_PMIC_QUERY_ADDRESS,&value,deadline,&remaining_retries);
    if(!NT_SUCCESS(status))return status;
    if(!value || value>0x7f)return failure(p,STATUS_DEVICE_PROTOCOL_ERROR);
    UCHAR candidate=(UCHAR)value;
    /* Record verified PMIC address immediately so AbandonRail can unwind a
     * timed-out CLAIM_RAIL even when the caller's out-parameter is still 0. */
    {
        KIRQL irql,state_irql;
        TG_PLATFORM_RAIL_STATE *state=NULL;
        KeAcquireSpinLock(&p->HandleLock,&irql);
        state=(TG_PLATFORM_RAIL_STATE *)p->RailState;
        if(state)InterlockedIncrement(&state->RefCount);
        KeReleaseSpinLock(&p->HandleLock,irql);
        if(state) {
            KeAcquireSpinLock(&state->Lock,&state_irql);
            if(!state->Closed) {
                state->LastPmicAddress=candidate;
                mirror_rail_state_locked(state);
            }
            KeReleaseSpinLock(&state->Lock,state_irql);
            release_rail_state(state);
        }
    }
    status=preflight_query_ready(p,TG_GPU_PMIC_QUERY_TEMP,&value,deadline,&remaining_retries);
    if(!NT_SUCCESS(status))return status;
    if(value>=80)return failure(p,STATUS_DEVICE_NOT_READY);
    request=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_CLAIM_RAIL,1,TG_GPU_EMC_VOTE_ABI};
    status=platform_request_deadline(p,&request,TRUE,NULL,TRUE,deadline);
    if(!NT_SUCCESS(status) && status!=STATUS_INVALID_PARAMETER && status!=STATUS_NOT_SUPPORTED &&
            status!=STATUS_INVALID_DEVICE_REQUEST)return failure(p,status);
    if(KeQueryInterruptTime()>=deadline)return failure(p,STATUS_IO_TIMEOUT);
    *address=candidate;return STATUS_SUCCESS;
}

NTSTATUS TgGpuRetryUnresolvedRailRelease(VOID)
{
    ULONG target_epoch=0,target_tok=0;
    BOOLEAN isolated=FALSE;UCHAR address=0;
    OBJECT_ATTRIBUTES attributes;IO_STATUS_BLOCK iosb={0};
    HANDLE handle=NULL;PFILE_OBJECT file=NULL;PDEVICE_OBJECT owner_dev=NULL;
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\SwitchBacklight");
    PAGED_CODE();
    sync_provider_rail_once();
    if(!query_unresolved_rail_ex(&target_epoch,&target_tok,&isolated,&address,NULL,NULL))
        return STATUS_SUCCESS;
    (VOID)InterlockedIncrement(&g_UnresolvedRail.RetryCount);
    if(InterlockedCompareExchange(&g_InFlightNewClaims,0,0)>0) {
        InterlockedExchange(&g_UnresolvedRail.LastStatus,STATUS_DEVICE_BUSY);
        return STATUS_DEVICE_BUSY;
    }
    if(!isolated || !address || !g_PlatformDriverObject || !g_PlatformDriverObject->DeviceObject) {
        InterlockedExchange(&g_UnresolvedRail.LastStatus,STATUS_DEVICE_NOT_READY);
        return STATUS_DEVICE_NOT_READY;
    }
    owner_dev=g_PlatformDriverObject->DeviceObject;
    InitializeObjectAttributes(&attributes,(PUNICODE_STRING)&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    NTSTATUS status=ZwCreateFile(&handle,GENERIC_READ|GENERIC_WRITE,&attributes,&iosb,NULL,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,NULL,0);
    if(!NT_SUCCESS(status)) {
        InterlockedExchange(&g_UnresolvedRail.LastStatus,(LONG)status);
        return status;
    }
    status=ObReferenceObjectByHandle(handle,FILE_READ_DATA|FILE_WRITE_DATA,*IoFileObjectType,
        KernelMode,(PVOID *)&file,NULL);
    if(!NT_SUCCESS(status)) {
        (VOID)ZwClose(handle);
        InterlockedExchange(&g_UnresolvedRail.LastStatus,(LONG)status);
        return status;
    }
    ObfReferenceObject(owner_dev);
    struct tg_gpu_pmic_request r_en={address,0x06u,0,0};
    struct tg_gpu_pmic_request r_vo={address,0x23u,0,0};
    ULONG chk_epoch=0;BOOLEAN chk_iso=FALSE;
    if(InterlockedCompareExchange(&g_InFlightNewClaims,0,0)>0) {
        status=STATUS_DEVICE_BUSY;
        goto done_retry;
    }
    if(!query_unresolved_rail_ex(&chk_epoch,NULL,&chk_iso,NULL,NULL,NULL) || chk_epoch!=target_epoch) {
        status=STATUS_SUCCESS;
        goto done_retry;
    }
    if(!chk_iso) {
        status=STATUS_DEVICE_NOT_READY;
        goto done_retry;
    }
    status=send_pmic_irp(NULL,0,0,0,file,owner_dev,&r_en,FALSE,0);
    if(NT_SUCCESS(status)) {
        if(InterlockedCompareExchange(&g_InFlightNewClaims,0,0)>0) {
            status=STATUS_DEVICE_BUSY;
            goto done_retry;
        }
        if(!query_unresolved_rail_ex(&chk_epoch,NULL,&chk_iso,NULL,NULL,NULL) || chk_epoch!=target_epoch) {
            status=STATUS_SUCCESS;
            goto done_retry;
        }
        if(!chk_iso) {
            status=STATUS_DEVICE_NOT_READY;
            goto done_retry;
        }
        status=send_pmic_irp(NULL,0,0,0,file,owner_dev,&r_vo,FALSE,0);
    }
    if(NT_SUCCESS(status)) {
        if(InterlockedCompareExchange(&g_InFlightNewClaims,0,0)>0) {
            status=STATUS_DEVICE_BUSY;
            goto done_retry;
        }
        if(!query_unresolved_rail_ex(&chk_epoch,&target_tok,&chk_iso,NULL,NULL,NULL) || chk_epoch!=target_epoch) {
            status=STATUS_SUCCESS;
            goto done_retry;
        }
        if(!chk_iso) {
            status=STATUS_DEVICE_NOT_READY;
            goto done_retry;
        }
        const UINT32 rel_val16=(((UINT32)(UINT8)r_en.value)<<8)|(UINT8)r_vo.value;
        const UINT32 full_val=(InterlockedCompareExchange(&g_ProviderTokenAware,0,0) && target_tok) ?
            (((UINT32)(target_tok&0xffffu)<<16)|rel_val16) : rel_val16;
        struct tg_gpu_pmic_request rel={address,TG_GPU_PMIC_RELEASE_RAIL,1,full_val};
        status=send_pmic_irp(NULL,0,target_epoch,0,file,owner_dev,&rel,FALSE,0);
    }
    if(NT_SUCCESS(status)) {
        clear_unresolved_rail_epoch(target_epoch);
    } else if(status!=STATUS_DEVICE_BUSY) {
        notify_provider_handoff_via_file(file,owner_dev,target_tok?target_tok:target_epoch,address,TRUE,isolated);
    }
done_retry:
    ObfDereferenceObject(owner_dev);
    ObfDereferenceObject(file);
    (VOID)ZwClose(handle);
    InterlockedExchange(&g_UnresolvedRail.LastStatus,(LONG)status);
    return status;
}
static PULONG location(TG_GPU_PLATFORM *p,UINT32 address,BOOLEAN write)
{
    const TG_GPU_RESOURCES *r=p->Resources;
    if(!r || (address&3))return NULL;
    if(r->LicMapping && address>=T214_GPU_LIC_BASE && address-T214_GPU_LIC_BASE<=T214_GPU_LIC_SIZE-4)
        return (PULONG)((PUCHAR)r->LicMapping+address-T214_GPU_LIC_BASE);
    for(UINT i=0;i<TgGpuApertureCount;++i) {
        UINT64 base=(UINT64)r->Memory[i].u.Memory.Start.QuadPart;
        UINT length=r->Memory[i].u.Memory.Length;
        if(r->Mapping[i] && length>=4 && address>=base && (UINT64)address-base<=length-4) {
            /* CPU/DDR/ASID ownership stays with its existing driver. Fuses
             * are read-only; no write to the PLLP query aperture is mapped. */
            if(write && (i==TgGpuMcAsid || i>=TgGpuHidrev))return NULL;
            return (PULONG)((PUCHAR)r->Mapping[i]+(UINT64)address-base);
        }
    }
    return NULL;
}
static __forceinline VOID mmio_dsb(VOID)
{
#if defined(_M_ARM64) || defined(_ARM64_)
    _DataSynchronizationBarrier();
#else
    KeMemoryBarrier();
#endif
}
static UINT32 read32(void *context,UINT32 address)
{
    TG_GPU_PLATFORM *p=context;PULONG reg;UINT32 value;
    if(address==0x600060a0) {
        struct tg_gpu_pmic_request op={0,TG_GPU_PMIC_QUERY_PLLP,0,0};
        return NT_SUCCESS(TgGpuPlatformRequest(p,&op))?op.value:UINT32_MAX;
    }
    reg=location(p,address,FALSE);
    if(!reg){failure(p,STATUS_DEVICE_CONFIGURATION_ERROR);return UINT32_MAX;}
    value=READ_REGISTER_ULONG(reg);
    mmio_dsb();
    return value;
}
static void write32(void *context,UINT32 address,UINT32 value)
{
    TG_GPU_PLATFORM *p=context;PULONG reg=location(p,address,TRUE);
    if(!reg){failure(p,STATUS_DEVICE_CONFIGURATION_ERROR);return;}
    /* WDK WRITE_REGISTER_ULONG on ARM64 already issues a leading DSB. */
#if !(defined(_M_ARM64) || defined(_ARM64_))
    KeMemoryBarrier();
#endif
    WRITE_REGISTER_ULONG(reg,value);
    mmio_dsb();
}
static bool read_pmic(void *context,UINT8 address,UINT8 reg,UINT8 *value)
{
    struct tg_gpu_pmic_request op={address,reg,0,0};
    if(!NT_SUCCESS(TgGpuPlatformRequest(context,&op)))return false;
    *value=(UINT8)op.value;return true;
}
static bool write_pmic(void *context,UINT8 address,UINT8 reg,UINT8 value)
{
    struct tg_gpu_pmic_request op={address,reg,1,value};
    return NT_SUCCESS(TgGpuPlatformRequest(context,&op));
}
static void delay(void *context,UINT32 us)
{
    LARGE_INTEGER interval;UNREFERENCED_PARAMETER(context);
    if(us<1000){KeStallExecutionProcessor(us);return;}
    interval.QuadPart=-(LONGLONG)us*10;(VOID)KeDelayExecutionThread(KernelMode,FALSE,&interval);
}
static bool release_pmic(void *context,UINT8 address,UINT8 enable,UINT8 voltage)
{
    TG_GPU_PLATFORM *p=context;
    UINT8 live,check;
    if(p->EmcRailOwner) {
        struct tg_gpu_pmic_request op={address,TG_GPU_PMIC_RELEASE_RAIL,1,
                                      ((UINT32)enable<<8)|voltage};
        return NT_SUCCESS(TgGpuPlatformRequest(p,&op));
    }
    /* Legacy SwitchFan has no EMC owner. Never use this fallback after a
     * supported owner's release failed: its remaining rail vote is binding. */
    if(!read_pmic(context,address,0x06u,&live))return false;
    live=(UINT8)((live&~1u)|(enable&1u));
    return write_pmic(context,address,0x06u,live) &&
           read_pmic(context,address,0x06u,&check) && check==live &&
           write_pmic(context,address,0x23u,voltage) &&
           read_pmic(context,address,0x23u,&check) && check==voltage;
}
struct tg_gpu_power_io TgGpuPlatformIo(TG_GPU_PLATFORM *p)
{ return (struct tg_gpu_power_io){p,read32,write32,read_pmic,write_pmic,delay,release_pmic}; }
