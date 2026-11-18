/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include <initguid.h>
#include <wdmguid.h>
#include "platform_io.h"
#include "../platform/t214_resources.h"

#define TG_PLATFORM_TAG 0x50674754u
#define TG_PLATFORM_TIMEOUT_100NS (-10000000LL) /* pending IRP wait: 1 s */

typedef struct _TG_PLATFORM_IRP_CTX {
    PIRP Irp;
    PFILE_OBJECT FileObject;
    KEVENT Done;
    volatile LONG RefCount;
    NTSTATUS Status;
    ULONG_PTR Information;
    struct tg_gpu_pmic_request Buffer;
} TG_PLATFORM_IRP_CTX;

static PDRIVER_OBJECT g_PlatformDriverObject;

VOID TgGpuSetPlatformDriverObject(PVOID DriverObject)
{ g_PlatformDriverObject=(PDRIVER_OBJECT)DriverObject; }

static NTSTATUS failure(TG_GPU_PLATFORM *p,NTSTATUS status)
{
    if(p && !NT_SUCCESS(status) && NT_SUCCESS(p->IoStatus))p->IoStatus=status;
    return status;
}

static VOID detach_bridge_handles(TG_GPU_PLATFORM *p,BOOLEAN query_remove,BOOLEAN mark_reclaim)
{
    PFILE_OBJECT file;HANDLE handle;KIRQL irql;
    KeAcquireSpinLock(&p->HandleLock,&irql);
    file=(PFILE_OBJECT)p->FileObject;
    handle=p->Bridge;
    p->FileObject=NULL;
    p->Bridge=NULL;
    p->QueryRemove=query_remove;
    ++p->BridgeEpoch;
    if(mark_reclaim) {
        ++p->ProviderGeneration;
        if(p->RailClaimed)p->NeedReclaim=TRUE;
    }
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(file)ObfDereferenceObject(file);
    if(handle)(VOID)ZwClose(handle);
}

static NTSTATUS NTAPI platform_pnp_notify(PVOID NotificationStructure,PVOID Context)
{
    TARGET_DEVICE_REMOVAL_NOTIFICATION *n=NotificationStructure;
    TG_GPU_PLATFORM *p=Context;
    KIRQL irql;
    if(!n || !p)return STATUS_SUCCESS;
    if(IsEqualGUID(&n->Event,&GUID_TARGET_DEVICE_QUERY_REMOVE)) {
        detach_bridge_handles(p,TRUE,FALSE);
    } else if(IsEqualGUID(&n->Event,&GUID_TARGET_DEVICE_REMOVE_COMPLETE)) {
        /* Old SwitchBacklight FDO is gone; allow transparent reopen once a new
         * SwitchFan instance recreates \Device\SwitchBacklight. */
        detach_bridge_handles(p,FALSE,TRUE);
    } else if(IsEqualGUID(&n->Event,&GUID_TARGET_DEVICE_REMOVE_CANCELLED)) {
        KeAcquireSpinLock(&p->HandleLock,&irql);
        p->QueryRemove=FALSE;
        ++p->BridgeEpoch;
        KeReleaseSpinLock(&p->HandleLock,irql);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS open_bridge_internal(TG_GPU_PLATFORM *p)
{
    OBJECT_ATTRIBUTES attributes;IO_STATUS_BLOCK iosb={0};
    HANDLE handle=NULL;PFILE_OBJECT file=NULL;PVOID notify=NULL,stale_notify=NULL;
    KIRQL irql;NTSTATUS status;ULONG epoch;
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\SwitchBacklight");
    KeAcquireSpinLock(&p->HandleLock,&irql);
    if(p->Closing || p->QueryRemove || p->Opening){KeReleaseSpinLock(&p->HandleLock,irql);return STATUS_DEVICE_NOT_READY;}
    if(p->Bridge && p->FileObject){KeReleaseSpinLock(&p->HandleLock,irql);return STATUS_SUCCESS;}
    p->Opening=TRUE;
    epoch=p->BridgeEpoch;
    stale_notify=p->PnpNotify;
    p->PnpNotify=NULL;
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(stale_notify)(VOID)IoUnregisterPlugPlayNotificationEx(stale_notify);

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
    if(g_PlatformDriverObject) {
        status=IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,0,file,
            g_PlatformDriverObject,platform_pnp_notify,p,&notify);
        if(!NT_SUCCESS(status))goto done;
    }
    KeAcquireSpinLock(&p->HandleLock,&irql);
    if(p->Closing || p->QueryRemove || p->BridgeEpoch!=epoch || p->Bridge || p->FileObject) {
        KeReleaseSpinLock(&p->HandleLock,irql);
        status=STATUS_DEVICE_NOT_READY;
        goto done;
    }
    p->Bridge=handle;
    p->FileObject=file;
    p->PnpNotify=notify;
    handle=NULL;file=NULL;notify=NULL;
    KeReleaseSpinLock(&p->HandleLock,irql);
    status=STATUS_SUCCESS;
done:
    if(notify)(VOID)IoUnregisterPlugPlayNotificationEx(notify);
    if(file)ObfDereferenceObject(file);
    if(handle)(VOID)ZwClose(handle);
    KeAcquireSpinLock(&p->HandleLock,&irql);
    p->Opening=FALSE;
    KeReleaseSpinLock(&p->HandleLock,irql);
    return status;
}

NTSTATUS TgGpuOpenPlatform(TG_GPU_PLATFORM *p,const TG_GPU_RESOURCES *resources)
{
    PDEVICE_OBJECT owner_dev=NULL;NTSTATUS status;
    PAGED_CODE();
    if(!p || !resources || p->Bridge || p->FileObject || p->Resources)return STATUS_INVALID_PARAMETER;
    for(UINT i=0;i<TgGpuApertureCount;++i)
        if(!resources->Mapping[i] || resources->Memory[i].Type!=CmResourceTypeMemory ||
           resources->Memory[i].u.Memory.Length<4)return STATUS_DEVICE_CONFIGURATION_ERROR;
    if(!g_PlatformDriverObject)return STATUS_INVALID_DEVICE_STATE;
    owner_dev=g_PlatformDriverObject->DeviceObject;
    if(!owner_dev || owner_dev->DriverObject!=g_PlatformDriverObject)return STATUS_INVALID_DEVICE_STATE;
    ObfReferenceObject(owner_dev);
    KeInitializeSpinLock(&p->HandleLock);
    p->Initialized=TRUE;
    p->Opening=p->Closing=FALSE;
    p->OwnerDevice=owner_dev;
    p->IoStatus=STATUS_SUCCESS;
    p->QueryRemove=FALSE;
    p->NeedReclaim=FALSE;
    status=open_bridge_internal(p);
    if(!NT_SUCCESS(status)) {
        if(p->OwnerDevice){ObfDereferenceObject(p->OwnerDevice);p->OwnerDevice=NULL;}
        return failure(p,status);
    }
    p->Resources=resources;return STATUS_SUCCESS;
}

static bool read_pmic(void *context,UINT8 address,UINT8 reg,UINT8 *value);
static UINT32 read32(void *context,UINT32 address);

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

VOID TgGpuPlatformAbandonRail(TG_GPU_PLATFORM *p,UCHAR address)
{
    UINT8 enable=0,voltage=0;UINT32 reset,clamp;
    if(!p || !p->EmcRailOwner || !p->RailClaimed)return;
    /* !Power.owned can mean that another channel was already running.
     * Only an actually reset and clamped GPU may hand the rail to EMC. */
    reset=read32(p,0x6000628cu);clamp=read32(p,0x7000e6d4u);
    if(reset==UINT32_MAX || clamp==UINT32_MAX || !(reset&(1u<<24)) || !(clamp&1u))return;
    if(address && read_pmic(p,address,0x06u,&enable) && read_pmic(p,address,0x23u,&voltage)) {
        struct tg_gpu_pmic_request op={address,TG_GPU_PMIC_RELEASE_RAIL,1,
                                       ((UINT32)enable<<8)|voltage};
        (VOID)TgGpuPlatformTryRequest(p,&op);
    }
}

NTSTATUS TgGpuClosePlatform(TG_GPU_PLATFORM *p)
{
    PVOID notify=NULL,owner=NULL;HANDLE handle=NULL;PFILE_OBJECT file=NULL;
    KIRQL irql;NTSTATUS status=STATUS_SUCCESS;
    PAGED_CODE();
    if(!p)return STATUS_INVALID_PARAMETER;
    if(!p->Initialized)return STATUS_SUCCESS;
    KeAcquireSpinLock(&p->HandleLock,&irql);
    p->Closing=TRUE;
    ++p->BridgeEpoch;
    ++p->ProviderGeneration;
    notify=p->PnpNotify;p->PnpNotify=NULL;
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(notify)(VOID)IoUnregisterPlugPlayNotificationEx(notify);

    KeAcquireSpinLock(&p->HandleLock,&irql);
    handle=p->Bridge;file=(PFILE_OBJECT)p->FileObject;
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(handle) {
        status=ZwClose(handle);
        if(!NT_SUCCESS(status))return failure(p,status);
    }
    KeAcquireSpinLock(&p->HandleLock,&irql);
    p->Bridge=NULL;p->FileObject=NULL;
    owner=p->OwnerDevice;p->OwnerDevice=NULL;
    p->QueryRemove=FALSE;p->NeedReclaim=FALSE;
    KeReleaseSpinLock(&p->HandleLock,irql);
    if(file)ObfDereferenceObject(file);
    if(owner)ObfDereferenceObject(owner);
    p->Resources=NULL;p->EmcRailOwner=FALSE;p->RailClaimed=FALSE;
    return STATUS_SUCCESS;
}

static VOID release_irp_ctx(TG_PLATFORM_IRP_CTX *ctx)
{
    if(InterlockedDecrement(&ctx->RefCount)==0) {
        IoFreeIrp(ctx->Irp);
        ObfDereferenceObject(ctx->FileObject);
        ExFreePoolWithTag(ctx,TG_PLATFORM_TAG);
    }
}

static NTSTATUS NTAPI platform_irp_complete(PDEVICE_OBJECT Device,PIRP Irp,PVOID Context)
{
    TG_PLATFORM_IRP_CTX *ctx=Context;
    UNREFERENCED_PARAMETER(Device);
    ctx->Status=Irp->IoStatus.Status;
    ctx->Information=Irp->IoStatus.Information;
    KeSetEvent(&ctx->Done,IO_NO_INCREMENT,FALSE);
    release_irp_ctx(ctx);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS send_pmic_irp(PFILE_OBJECT file,PDEVICE_OBJECT owner_dev,
                              struct tg_gpu_pmic_request *op)
{
    PDEVICE_OBJECT target_dev;TG_PLATFORM_IRP_CTX *ctx;PIO_STACK_LOCATION stack;
    LARGE_INTEGER timeout;NTSTATUS status,wait;ULONG_PTR info;
    target_dev=IoGetRelatedDeviceObject(file);
    if(!target_dev)return STATUS_INVALID_DEVICE_STATE;
    ctx=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*ctx),TG_PLATFORM_TAG);
    if(!ctx)return STATUS_INSUFFICIENT_RESOURCES;
    ctx->Irp=IoAllocateIrp(target_dev->StackSize,FALSE);
    if(!ctx->Irp){ExFreePoolWithTag(ctx,TG_PLATFORM_TAG);return STATUS_INSUFFICIENT_RESOURCES;}
    ObfReferenceObject(file);
    ctx->FileObject=file;
    ctx->RefCount=2;
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
    status=owner_dev ? IoSetCompletionRoutineEx(owner_dev,ctx->Irp,platform_irp_complete,ctx,TRUE,TRUE,TRUE) : STATUS_INVALID_DEVICE_STATE;
    if(!NT_SUCCESS(status)) {
        IoFreeIrp(ctx->Irp);
        ObfDereferenceObject(ctx->FileObject);
        ExFreePoolWithTag(ctx,TG_PLATFORM_TAG);
        return status;
    }
    (VOID)IoCallDriver(target_dev,ctx->Irp);
    timeout.QuadPart=TG_PLATFORM_TIMEOUT_100NS;
    wait=KeWaitForSingleObject(&ctx->Done,Executive,KernelMode,FALSE,&timeout);
    if(wait!=STATUS_SUCCESS) {
        (VOID)IoCancelIrp(ctx->Irp);
        release_irp_ctx(ctx);
        return (wait==STATUS_TIMEOUT || NT_SUCCESS(wait))?STATUS_IO_TIMEOUT:wait;
    }
    status=ctx->Status;
    info=ctx->Information;
    if(NT_SUCCESS(status) && info==sizeof(*op))*op=ctx->Buffer;
    release_irp_ctx(ctx);
    if(status!=STATUS_SUCCESS && NT_SUCCESS(status))status=STATUS_DEVICE_PROTOCOL_ERROR;
    if(NT_SUCCESS(status) && info!=sizeof(*op))status=STATUS_DEVICE_PROTOCOL_ERROR;
    return status;
}

static NTSTATUS platform_request(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op,BOOLEAN optional,
                                 ULONG *vote_generation)
{
    PFILE_OBJECT file=NULL;PDEVICE_OBJECT owner_dev=NULL;
    BOOLEAN reclaim=FALSE;KIRQL irql;NTSTATUS status;ULONG epoch=0,generation=0;
    ULONG reg;
    PAGED_CODE();
    if(!p || !op || !p->Resources)return STATUS_INVALID_PARAMETER;
    reg=op->reg;
    KeAcquireSpinLock(&p->HandleLock,&irql);
    file=(PFILE_OBJECT)p->FileObject;
    owner_dev=(PDEVICE_OBJECT)p->OwnerDevice;
    if(!p->Closing && p->Bridge && file) {
        epoch=p->BridgeEpoch;
        generation=p->ProviderGeneration;
        ObfReferenceObject(file);
        if(owner_dev)ObfReferenceObject(owner_dev);
        reclaim=p->NeedReclaim && reg!=TG_GPU_PMIC_CLAIM_RAIL && reg!=TG_GPU_PMIC_RELEASE_RAIL;
    } else {
        file=NULL;owner_dev=NULL;
    }
    KeReleaseSpinLock(&p->HandleLock,irql);

    if(!file) {
        status=open_bridge_internal(p);
        if(!NT_SUCCESS(status))return optional?status:failure(p,status);
        KeAcquireSpinLock(&p->HandleLock,&irql);
        file=(PFILE_OBJECT)p->FileObject;
        owner_dev=(PDEVICE_OBJECT)p->OwnerDevice;
        if(!p->Closing && p->Bridge && file) {
            epoch=p->BridgeEpoch;
            generation=p->ProviderGeneration;
            ObfReferenceObject(file);
            if(owner_dev)ObfReferenceObject(owner_dev);
            reclaim=p->NeedReclaim && reg!=TG_GPU_PMIC_CLAIM_RAIL && reg!=TG_GPU_PMIC_RELEASE_RAIL;
        } else {
            file=NULL;owner_dev=NULL;
        }
        KeReleaseSpinLock(&p->HandleLock,irql);
        if(!file)return optional?STATUS_DEVICE_NOT_READY:failure(p,STATUS_DEVICE_NOT_READY);
    }

    /* file and generation describe the same referenced endpoint, even if PnP
     * replaces the device immediately after HandleLock was released. */
    if(vote_generation && reg!=TG_GPU_PMIC_QUERY_EMC_VOTE && *vote_generation!=generation) {
        status=STATUS_DEVICE_NOT_READY;
        goto finished;
    }
    if(reclaim) {
        struct tg_gpu_pmic_request claim={0,TG_GPU_PMIC_CLAIM_RAIL,1,TG_GPU_EMC_VOTE_ABI};
        status=send_pmic_irp(file,owner_dev,&claim);
        if(NT_SUCCESS(status)) {
            KeAcquireSpinLock(&p->HandleLock,&irql);
            if(p->FileObject==file && p->BridgeEpoch==epoch)p->NeedReclaim=FALSE;
            else status=STATUS_DEVICE_NOT_READY;
            KeReleaseSpinLock(&p->HandleLock,irql);
        }
        if(!NT_SUCCESS(status))goto finished;
    }
    status=send_pmic_irp(file,owner_dev,op);
    if(NT_SUCCESS(status)) {
        KeAcquireSpinLock(&p->HandleLock,&irql);
        /* A reply from an instance removed during this request cannot update
         * the new instance's ownership. Publish while holding the same lock
         * as the PnP callback, not later in Preflight/release_pmic. */
        if(p->Closing || p->FileObject!=file || p->BridgeEpoch!=epoch) {
            status=STATUS_DEVICE_NOT_READY;
        } else if(vote_generation && reg==TG_GPU_PMIC_QUERY_EMC_VOTE) {
            *vote_generation=generation;
        } else if(reg==TG_GPU_PMIC_CLAIM_RAIL) {
            p->EmcRailOwner=TRUE;p->RailClaimed=TRUE;p->NeedReclaim=FALSE;
        } else if(reg==TG_GPU_PMIC_RELEASE_RAIL) {
            p->RailClaimed=FALSE;p->NeedReclaim=FALSE;
        }
        KeReleaseSpinLock(&p->HandleLock,irql);
    }
finished:
    if(owner_dev)ObfDereferenceObject(owner_dev);
    ObfDereferenceObject(file);
    if(optional && (status==STATUS_INVALID_PARAMETER || status==STATUS_NOT_SUPPORTED ||
                    status==STATUS_INVALID_DEVICE_REQUEST || status==STATUS_DEVICE_NOT_READY))
        return status;
    return failure(p,status);
}

NTSTATUS TgGpuPlatformRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op)
{ return platform_request(p,op,FALSE,NULL); }
NTSTATUS TgGpuPlatformTryRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op)
{ return platform_request(p,op,TRUE,NULL); }
NTSTATUS TgGpuPlatformTryVoteRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op,ULONG *generation)
{
    if(!generation || !op || (op->reg!=TG_GPU_PMIC_QUERY_EMC_VOTE &&
       op->reg!=TG_GPU_PMIC_REPORT_EMC_ACK && op->reg!=TG_GPU_PMIC_REPORT_EMC_FAILURE))
        return STATUS_INVALID_PARAMETER;
    return platform_request(p,op,TRUE,generation);
}
NTSTATUS TgGpuPlatformPreflight(TG_GPU_PLATFORM *p,UCHAR *address)
{
    struct tg_gpu_pmic_request request={0,TG_GPU_PMIC_QUERY_ADDRESS,0,0};NTSTATUS status;
    if(!address)return STATUS_INVALID_PARAMETER;
    status=TgGpuPlatformRequest(p,&request);if(!NT_SUCCESS(status))return status;
    if(!request.value || request.value>0x7f)return failure(p,STATUS_DEVICE_PROTOCOL_ERROR);
    UCHAR candidate=(UCHAR)request.value;
    request=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_QUERY_TEMP,0,0};
    status=TgGpuPlatformRequest(p,&request);if(!NT_SUCCESS(status))return status;
    if(request.value>=80)return failure(p,STATUS_DEVICE_NOT_READY);
    request=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_CLAIM_RAIL,1,TG_GPU_EMC_VOTE_ABI};
    status=TgGpuPlatformTryRequest(p,&request);
    if(!NT_SUCCESS(status) && status!=STATUS_INVALID_PARAMETER && status!=STATUS_NOT_SUPPORTED &&
            status!=STATUS_INVALID_DEVICE_REQUEST)return failure(p,status);
    *address=candidate;return STATUS_SUCCESS;
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
static UINT32 read32(void *context,UINT32 address)
{
    TG_GPU_PLATFORM *p=context;PULONG reg;
    if(address==0x600060a0) {
        struct tg_gpu_pmic_request op={0,TG_GPU_PMIC_QUERY_PLLP,0,0};
        return NT_SUCCESS(TgGpuPlatformRequest(p,&op))?op.value:UINT32_MAX;
    }
    reg=location(p,address,FALSE);
    if(!reg){failure(p,STATUS_DEVICE_CONFIGURATION_ERROR);return UINT32_MAX;}
    return READ_REGISTER_ULONG(reg);
}
static void write32(void *context,UINT32 address,UINT32 value)
{
    TG_GPU_PLATFORM *p=context;PULONG reg=location(p,address,TRUE);
    if(!reg){failure(p,STATUS_DEVICE_CONFIGURATION_ERROR);return;}
    KeMemoryBarrier();WRITE_REGISTER_ULONG(reg,value);KeMemoryBarrier();
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
