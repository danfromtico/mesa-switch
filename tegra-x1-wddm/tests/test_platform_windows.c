/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include <wdmguid.h>
#include "../windows/platform_io.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>
static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
#define CHECK(x) do {if(!(x))ExitProcess(__LINE__);}while(0)
void *memset(void *p,int c,size_t n){UCHAR *d=p;while(n--)*d++=(UCHAR)c;return p;}
void *memcpy(void *p,const void *s,size_t n){UCHAR *d=p;const UCHAR *a=s;while(n--)*d++=*a++;return p;}
int memcmp(const void *a,const void *b,size_t n){const UCHAR *x=a,*y=b;for(;n--;++x,++y)if(*x!=*y)return (int)*x-(int)*y;return 0;}

static TG_GPU_RESOURCES resources;
static TG_GPU_PLATFORM platform;
static ULONG registers[TgGpuApertureCount][4],lic[T214_GPU_LIC_SIZE/4];
static const ULONG bases[]={T214_GPU_REG_BASE,T214_GPU_BAR1_BASE,T214_GPU_AUX_BASE,
#define BASE(b,s) b,
    T214_GPU_EXTRA_RESOURCES(BASE)
#undef BASE
};
static POBJECT_TYPE fake_file_type=(POBJECT_TYPE)(ULONG_PTR)0x46494c45u;
POBJECT_TYPE *IoFileObjectType=&fake_file_type;
static DRIVER_OBJECT fake_driver;
static DEVICE_OBJECT fake_owner_device={.DriverObject=&fake_driver,.StackSize=1};
static DEVICE_OBJECT fake_target_device={.StackSize=1};
static FILE_OBJECT fake_file={.DeviceObject=&fake_target_device};

typedef struct {
    UCHAR irp_storage[512];
    PDEVICE_OBJECT safe_owner;
    PIO_COMPLETION_ROUTINE safe_cb;
    PVOID safe_ctx;
} IRP_SLOT;
static UCHAR pool_buf[16][512];
static BOOLEAN pool_used[16];
static IRP_SLOT irp_slots[16];
static BOOLEAN irp_used[16];

static struct {
    NTSTATUS open,close,io,wait,completion,claim,release,completion_registration;
    ULONG_PTR bytes;
    ULONG address,temp,opens,closes,requests,waits,cancels,stalls,sleeps;
    ULONG claims,releases,raw_writes,pnp_regs,pnp_unregs,safe_completions;
    ULONG addr_not_ready_polls,temp_not_ready_polls;
    LONG file_refs,owner_refs,live_pool,live_irps;
    KIRQL irql;
    BOOLEAN defer_complete;
    BOOLEAN remove_during_register;
    BOOLEAN remove_during_claim;
    PIRP deferred_irp;
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE pnp_cb;
    PVOID pnp_ctx;
    UCHAR enable,voltage;
} model;

static VOID NTAPI InitSpin(PKSPIN_LOCK l){*l=0;}
static KIRQL NTAPI LockSpin(PKSPIN_LOCK l)
{KIRQL old=model.irql;CHECK(!*l && old==PASSIVE_LEVEL);*l=1;model.irql=DISPATCH_LEVEL;return old;}
static VOID NTAPI UnlockSpin(PKSPIN_LOCK l,KIRQL old)
{CHECK(*l && model.irql==DISPATCH_LEVEL);*l=0;model.irql=old;}
static PVOID NTAPI AllocPool2(POOL_FLAGS flags,SIZE_T bytes,ULONG tag)
{
    CHECK(model.irql==PASSIVE_LEVEL && flags==POOL_FLAG_NON_PAGED && bytes<=sizeof(pool_buf[0]) && tag==0x50674754u);
    for(UINT i=0;i<16;++i)if(!pool_used[i]){pool_used[i]=TRUE;++model.live_pool;memset(pool_buf[i],0,bytes);return pool_buf[i];}
    return NULL;
}
static VOID NTAPI FreePoolTag(PVOID p,ULONG tag)
{
    CHECK(tag==0x50674754u);
    for(UINT i=0;i<16;++i)if(p==pool_buf[i] && pool_used[i]){pool_used[i]=FALSE;--model.live_pool;memset(pool_buf[i],0xdd,sizeof(pool_buf[i]));return;}
    ExitProcess(__LINE__);
}
static PIRP NTAPI AllocIrp(CCHAR stack_size,BOOLEAN charge_quota)
{
    CHECK(stack_size==1 && !charge_quota);
    for(UINT i=0;i<16;++i)if(!irp_used[i]){
        PIRP irp=(PIRP)irp_slots[i].irp_storage;
        irp_used[i]=TRUE;++model.live_irps;
        memset(&irp_slots[i],0,sizeof(irp_slots[i]));
        irp->Type=IO_TYPE_IRP;irp->Size=(USHORT)sizeof(irp_slots[i].irp_storage);
        irp->StackCount=stack_size;irp->CurrentLocation=(CCHAR)(stack_size+1);
        irp->Tail.Overlay.CurrentStackLocation=(PIO_STACK_LOCATION)(irp+1)+stack_size;
        return irp;
    }
    return NULL;
}
static VOID NTAPI FreeIrp(PIRP irp)
{
    for(UINT i=0;i<16;++i)if((PVOID)irp==irp_slots[i].irp_storage && irp_used[i]){
        irp_used[i]=FALSE;--model.live_irps;memset(irp_slots[i].irp_storage,0xee,sizeof(irp_slots[i].irp_storage));return;
    }
    ExitProcess(__LINE__);
}
static NTSTATUS NTAPI SetCompEx(PDEVICE_OBJECT dev,PIRP irp,PIO_COMPLETION_ROUTINE cb,PVOID ctx,BOOLEAN s,BOOLEAN e,BOOLEAN c)
{
    CHECK(dev==&fake_owner_device && dev->DriverObject==&fake_driver && cb && s && e && c);
    if(!NT_SUCCESS(model.completion_registration))return model.completion_registration;
    for(UINT i=0;i<16;++i)if((PVOID)irp==irp_slots[i].irp_storage && irp_used[i]){
        ++model.owner_refs;++model.safe_completions;
        irp_slots[i].safe_owner=dev;irp_slots[i].safe_cb=cb;irp_slots[i].safe_ctx=ctx;
        return STATUS_SUCCESS;
    }
    ExitProcess(__LINE__);
}
static VOID complete_irp(PIRP irp)
{
    for(UINT i=0;i<16;++i)if((PVOID)irp==irp_slots[i].irp_storage && irp_used[i]){
        PDEVICE_OBJECT owner=irp_slots[i].safe_owner;
        PIO_COMPLETION_ROUTINE cb=irp_slots[i].safe_cb;
        PVOID ctx=irp_slots[i].safe_ctx;
        CHECK(owner==&fake_owner_device && cb && model.owner_refs>0);
        CHECK(cb(&fake_target_device,irp,ctx)==STATUS_MORE_PROCESSING_REQUIRED);
        /* Simulated IopUnloadSafeCompletion drops owner reference AFTER routine returns. */
        --model.owner_refs;
        return;
    }
    ExitProcess(__LINE__);
}
static NTSTATUS NTAPI Open(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES attrs,
    PIO_STATUS_BLOCK iosb,PLARGE_INTEGER size,ULONG attr,ULONG share,ULONG disposition,
    ULONG options,PVOID ea,ULONG ea_size)
{
    static const WCHAR name[]=L"\\Device\\SwitchBacklight";
    CHECK(model.irql==PASSIVE_LEVEL && handle && iosb && !size && attr==FILE_ATTRIBUTE_NORMAL && !ea && !ea_size);
    CHECK(access==(GENERIC_READ|GENERIC_WRITE) && share==(FILE_SHARE_READ|FILE_SHARE_WRITE));
    CHECK(disposition==FILE_OPEN && options==FILE_NON_DIRECTORY_FILE);
    CHECK(attrs->Attributes==(OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE) && !attrs->RootDirectory);
    CHECK(attrs->ObjectName->Length==sizeof(name)-sizeof(WCHAR));
    for(UINT i=0;i<attrs->ObjectName->Length/sizeof(WCHAR);++i)CHECK(attrs->ObjectName->Buffer[i]==name[i]);
    ++model.opens;if(NT_SUCCESS(model.open))*handle=(HANDLE)123;return model.open;
}
static NTSTATUS NTAPI Close(HANDLE handle)
{CHECK(model.irql==PASSIVE_LEVEL && handle==(HANDLE)123);++model.closes;return model.close;}
static NTSTATUS NTAPI RefHandle(HANDLE h,ACCESS_MASK access,POBJECT_TYPE type,KPROCESSOR_MODE mode,PVOID *obj,POBJECT_HANDLE_INFORMATION info)
{
    CHECK(model.irql==PASSIVE_LEVEL && h==(HANDLE)123 && access==(FILE_READ_DATA|FILE_WRITE_DATA) && type==fake_file_type && mode==KernelMode && !info);
    ++model.file_refs;*obj=&fake_file;return STATUS_SUCCESS;
}
static LONG_PTR FASTCALL RefObj(PVOID obj)
{
    if(obj==&fake_file)return ++model.file_refs;
    if(obj==&fake_owner_device)return ++model.owner_refs;
    ExitProcess(__LINE__);
}
static LONG_PTR FASTCALL DerefObj(PVOID obj)
{
    if(obj==&fake_file){CHECK(model.file_refs>0);return --model.file_refs;}
    if(obj==&fake_owner_device){CHECK(model.owner_refs>0);return --model.owner_refs;}
    ExitProcess(__LINE__);
}
static PDEVICE_OBJECT NTAPI RelatedDev(PFILE_OBJECT fo){CHECK(fo==&fake_file);return &fake_target_device;}
static VOID NTAPI InitEvent(PRKEVENT ev,EVENT_TYPE t,BOOLEAN state){CHECK(ev && t==NotificationEvent);ev->Header.SignalState=state?1:0;}
static LONG NTAPI SetEvent(PRKEVENT ev,KPRIORITY inc,BOOLEAN wait){CHECK(ev && !inc && !wait);LONG prev=ev->Header.SignalState;ev->Header.SignalState=1;return prev;}
static NTSTATUS NTAPI WaitObj(PVOID obj,KWAIT_REASON r,KPROCESSOR_MODE m,BOOLEAN alert,PLARGE_INTEGER timeout)
{
    PRKEVENT ev=obj;
    CHECK(model.irql==PASSIVE_LEVEL && ev && r==Executive && m==KernelMode && !alert && timeout && timeout->QuadPart==-10000000LL);
    ++model.waits;
    if(model.defer_complete)return model.wait;
    CHECK(ev->Header.SignalState==1);
    return model.wait;
}
static BOOLEAN NTAPI CancelIrp(PIRP irp){CHECK(irp==model.deferred_irp);++model.cancels;irp->Cancel=TRUE;return TRUE;}
static NTSTATUS FASTCALL CallDriver(PDEVICE_OBJECT dev,PIRP irp)
{
    PIO_STACK_LOCATION sp=IoGetNextIrpStackLocation(irp);
    struct tg_gpu_pmic_request *op=irp->AssociatedIrp.SystemBuffer;
    CHECK(model.irql==PASSIVE_LEVEL && dev==&fake_target_device && irp->RequestorMode==KernelMode && sp->MajorFunction==IRP_MJ_DEVICE_CONTROL);
    CHECK(sp->FileObject==&fake_file && sp->Parameters.DeviceIoControl.IoControlCode==TG_GPU_IOCTL_PMIC);
    CHECK(sp->Parameters.DeviceIoControl.InputBufferLength==sizeof(*op) && sp->Parameters.DeviceIoControl.OutputBufferLength==sizeof(*op));
    ++model.requests;
    NTSTATUS st=model.completion;
    if(op->reg==TG_GPU_PMIC_QUERY_ADDRESS){
        if(model.addr_not_ready_polls){--model.addr_not_ready_polls;st=STATUS_DEVICE_NOT_READY;}
        else op->value=model.address;
    } else if(op->reg==TG_GPU_PMIC_QUERY_TEMP){
        if(model.temp_not_ready_polls){--model.temp_not_ready_polls;st=STATUS_DEVICE_NOT_READY;}
        else op->value=model.temp;
    }
    else if(op->reg==TG_GPU_PMIC_QUERY_PLLP)op->value=0x12345678;
    else if(op->reg==TG_GPU_PMIC_QUERY_DVFS_POLICY)op->value=TG_GPU_DVFS_POLICY_PACK(1,153600,460800,2);
    else if(op->reg==TG_GPU_PMIC_QUERY_EMC_KHZ)op->value=1600000;
    else if(op->reg==TG_GPU_PMIC_QUERY_EMC_VOTE)op->value=(1u<<8)|98u;
    else if(op->reg==TG_GPU_PMIC_REPORT_EMC_FAILURE || op->reg==TG_GPU_PMIC_REPORT_EMC_ACK)
        CHECK(op->write && op->value==((1u<<8)|98u));
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_STATUS)CHECK(op->write==1);
    else if(op->reg==TG_GPU_PMIC_CLAIM_RAIL){
        CHECK(op->write==1 && op->value==TG_GPU_EMC_VOTE_ABI);
        ++model.claims;
        if(!NT_SUCCESS(model.claim))st=model.claim;
        if(model.remove_during_claim){
            TARGET_DEVICE_REMOVAL_NOTIFICATION n={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
            CHECK(model.pnp_cb(&n,model.pnp_ctx)==STATUS_SUCCESS);
            n.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
            CHECK(model.pnp_cb(&n,model.pnp_ctx)==STATUS_SUCCESS);
        }
    } else if(op->reg==TG_GPU_PMIC_RELEASE_RAIL){
        CHECK(op->write==1 && op->address==0x33 && op->value==0x5446u);
        ++model.releases;
        if(!NT_SUCCESS(model.release))st=model.release;
    } else {
        CHECK(op->address==0x33 && (op->reg==0x23 || op->reg==0x06));
        UCHAR *reg=op->reg==0x23?&model.voltage:&model.enable;
        if(op->write){*reg=(UCHAR)op->value;++model.raw_writes;}else op->value=*reg;
    }
    if(model.defer_complete){model.deferred_irp=irp;return STATUS_PENDING;}
    irp->IoStatus.Status=st;irp->IoStatus.Information=NT_SUCCESS(st)?model.bytes:0;
    complete_irp(irp);
    return model.io;
}
static NTSTATUS NTAPI RegPnp(IO_NOTIFICATION_EVENT_CATEGORY cat,ULONG flags,PVOID data,PDRIVER_OBJECT drv,
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE cb,PVOID ctx,PVOID *entry)
{
    CHECK(model.irql==PASSIVE_LEVEL && cat==EventCategoryTargetDeviceChange && !flags && data==&fake_file && drv==&fake_driver && cb && entry);
    ++model.pnp_regs;model.pnp_cb=cb;model.pnp_ctx=ctx;*entry=(PVOID)0x777;
    if(model.remove_during_register){
        TARGET_DEVICE_REMOVAL_NOTIFICATION n={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
        CHECK(cb(&n,ctx)==STATUS_SUCCESS);
        n.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
        CHECK(cb(&n,ctx)==STATUS_SUCCESS);
    }
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI UnregPnp(PVOID entry){CHECK(model.irql==PASSIVE_LEVEL && entry==(PVOID)0x777);++model.pnp_unregs;model.pnp_cb=NULL;return STATUS_SUCCESS;}
static VOID NTAPI Stall(ULONG us){CHECK(us==999);++model.stalls;}
static NTSTATUS NTAPI Sleep(KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER interval)
{
    CHECK(mode==KernelMode && !alertable &&
          (interval->QuadPart==-10000 || interval->QuadPart==-500000));
    ++model.sleeps;return STATUS_SUCCESS;
}

VOID (NTAPI * const __imp_KeInitializeSpinLock)(PKSPIN_LOCK)=InitSpin;
KIRQL (NTAPI * const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=LockSpin;
VOID (NTAPI * const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=UnlockSpin;
NTSTATUS (NTAPI * const __imp_ZwCreateFile)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,
    PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG)=Open;
NTSTATUS (NTAPI * const __imp_ZwClose)(HANDLE)=Close;
NTSTATUS (NTAPI * const __imp_ObReferenceObjectByHandle)(HANDLE,ACCESS_MASK,POBJECT_TYPE,KPROCESSOR_MODE,PVOID *,POBJECT_HANDLE_INFORMATION)=RefHandle;
LONG_PTR (FASTCALL * const __imp_ObfReferenceObject)(PVOID)=RefObj;
LONG_PTR (FASTCALL * const __imp_ObfDereferenceObject)(PVOID)=DerefObj;
PDEVICE_OBJECT (NTAPI * const __imp_IoGetRelatedDeviceObject)(PFILE_OBJECT)=RelatedDev;
PVOID (NTAPI * const __imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=AllocPool2;
VOID (NTAPI * const __imp_ExFreePoolWithTag)(PVOID,ULONG)=FreePoolTag;
PIRP (NTAPI * const __imp_IoAllocateIrp)(CCHAR,BOOLEAN)=AllocIrp;
VOID (NTAPI * const __imp_IoFreeIrp)(PIRP)=FreeIrp;
NTSTATUS (NTAPI * const __imp_IoSetCompletionRoutineEx)(PDEVICE_OBJECT,PIRP,PIO_COMPLETION_ROUTINE,PVOID,BOOLEAN,BOOLEAN,BOOLEAN)=SetCompEx;
VOID (NTAPI * const __imp_KeInitializeEvent)(PRKEVENT,EVENT_TYPE,BOOLEAN)=InitEvent;
LONG (NTAPI * const __imp_KeSetEvent)(PRKEVENT,KPRIORITY,BOOLEAN)=SetEvent;
NTSTATUS (NTAPI * const __imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=WaitObj;
BOOLEAN (NTAPI * const __imp_IoCancelIrp)(PIRP)=CancelIrp;
NTSTATUS (FASTCALL * const __imp_IofCallDriver)(PDEVICE_OBJECT,PIRP)=CallDriver;
NTSTATUS (NTAPI * const __imp_IoRegisterPlugPlayNotification)(IO_NOTIFICATION_EVENT_CATEGORY,ULONG,PVOID,PDRIVER_OBJECT,
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE,PVOID,PVOID *)=RegPnp;
NTSTATUS (NTAPI * const __imp_IoUnregisterPlugPlayNotificationEx)(PVOID)=UnregPnp;
VOID (NTAPI * const __imp_KeStallExecutionProcessor)(ULONG)=Stall;
NTSTATUS (NTAPI * const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=Sleep;

static void Setup(void)
{
    CHECK(!platform.Bridge && !platform.FileObject && !platform.Resources);
    CHECK(!model.live_pool && !model.live_irps && !model.file_refs && !model.owner_refs);
    memset(&platform,0,sizeof(platform));memset(&model,0,sizeof(model));
    model.bytes=sizeof(struct tg_gpu_pmic_request);model.address=0x33;model.temp=42;
    model.enable=0x55;model.voltage=80;
}
#ifndef TG_PLATFORM_FIXTURE_ONLY
void start(void)
{
    static const char pass[]="PASS: WDM HandleLock + IoSetCompletionRoutineEx unload-safe async IRP bridge, PnP QueryRemove/RemoveComplete reconnect and rail abandon\r\n";
    ULONG written;UCHAR address=0,value=0;struct tg_gpu_power_io io;
    fake_driver.DeviceObject=&fake_owner_device;
    TgGpuSetPlatformDriverObject(&fake_driver);
    for(UINT i=0;i<TgGpuApertureCount;++i) {
        resources.Memory[i].Type=CmResourceTypeMemory;
        resources.Memory[i].u.Memory.Start.QuadPart=bases[i];
        resources.Memory[i].u.Memory.Length=sizeof(registers[i]);resources.Mapping[i]=registers[i];
    }
    resources.LicMapping=lic;
    Setup();CHECK(TgGpuOpenPlatform(NULL,&resources)==STATUS_INVALID_PARAMETER);
    resources.Mapping[4]=NULL;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_DEVICE_CONFIGURATION_ERROR && !model.opens);
    resources.Mapping[4]=registers[4];model.open=STATUS_ACCESS_DENIED;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_ACCESS_DENIED && !platform.Bridge && !platform.Resources && !model.owner_refs);
    model.open=STATUS_SUCCESS;CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    CHECK(platform.Resources==&resources && platform.IoStatus==STATUS_SUCCESS && model.pnp_regs==1 && model.owner_refs==1);
    {
        PFILE_OBJECT file = NULL; PDEVICE_OBJECT owner = NULL;
        LONG files = model.file_refs, owners = model.owner_refs;
        CHECK(TgGpuReferencePlatformBridge(&platform,&file,&owner)==STATUS_SUCCESS);
        CHECK(file==&fake_file && owner==&fake_owner_device &&
              model.file_refs==files+1 && model.owner_refs==owners+1);
        (VOID)DerefObj(file); (VOID)DerefObj(owner);
        CHECK(model.file_refs==files && model.owner_refs==owners);
    }
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_INVALID_PARAMETER);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && address==0x33 && model.safe_completions==3);
    io=TgGpuPlatformIo(&platform);CHECK(io.context==&platform);
    CHECK(platform.EmcRailOwner && platform.RailClaimed && io.release_pmic);

    /* Failure to pin the completion's driver must not dispatch or leak. */
    model.completion_registration=STATUS_INSUFFICIENT_RESOURCES;
    ULONG before_requests=model.requests;
    CHECK(!io.read_pmic(io.context,0x33,0x23,&value));
    CHECK(model.requests==before_requests && !model.live_pool && !model.live_irps);
    model.completion_registration=STATUS_SUCCESS;

    /* Delayed completion after timeout + TgGpuClosePlatform: OwnerDevice stays
     * pinned (owner_refs > 0) until platform_irp_complete returns! */
    platform.IoStatus=STATUS_SUCCESS;model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT && model.cancels==1);
    CHECK(model.live_pool==1 && model.live_irps==1 && model.file_refs==2 && model.owner_refs==2);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    /* Close dropped platform's own ref, but IoSetCompletionRoutineEx STILL holds owner_refs==1! */
    CHECK(model.owner_refs==1 && model.file_refs==1 && model.live_irps==1);
    model.deferred_irp->IoStatus.Status=STATUS_CANCELLED;
    complete_irp(model.deferred_irp);
    CHECK(model.owner_refs==0 && model.file_refs==0 && !model.live_irps && !model.live_pool);

    /* QueryRemove + RemoveComplete during registration must invalidate the
     * unpublished handle even though QueryRemove has already become FALSE. */
    Setup();model.remove_during_register=TRUE;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_DEVICE_NOT_READY);
    CHECK(!platform.Bridge && !platform.FileObject && !model.file_refs && !model.owner_refs);
    CHECK(model.closes==1 && model.pnp_unregs==1 && !platform.Opening);

    /* A successful old-instance CLAIM reply after removal is not ownership
     * of the replacement SwitchFan. Only a fresh claim may publish it. */
    Setup();CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    model.remove_during_claim=TRUE;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_NOT_READY);
    CHECK(!platform.RailClaimed && !platform.EmcRailOwner);
    model.remove_during_claim=FALSE;platform.IoStatus=STATUS_SUCCESS;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS);
    CHECK(platform.RailClaimed && platform.EmcRailOwner && model.claims==2);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);

    /* Reopen and test QueryRemove -> RemoveComplete -> new SwitchFan reconnect + CLAIM_RAIL. */
    Setup();CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed && model.claims==1);
    io=TgGpuPlatformIo(&platform);
    TARGET_DEVICE_REMOVAL_NOTIFICATION rem={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE saved_cb=model.pnp_cb;
    PVOID saved_ctx=model.pnp_ctx;
    ULONG original_provider=~0u;
    struct tg_gpu_pmic_request vote={0,TG_GPU_PMIC_QUERY_EMC_VOTE,0,0};
    CHECK(TgGpuPlatformTryVoteRequest(&platform,&vote,&original_provider)==STATUS_SUCCESS);
    CHECK(saved_cb(&rem,saved_ctx)==STATUS_SUCCESS && !platform.Bridge && !platform.FileObject && model.file_refs==0);
    CHECK(!io.read_pmic(io.context,0x33,0x23,&value)); /* blocked during QueryRemove */
    rem.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;platform.IoStatus=STATUS_SUCCESS;
    CHECK(saved_cb(&rem,saved_ctx)==STATUS_SUCCESS && !platform.QueryRemove && platform.NeedReclaim);
    CHECK(io.read_pmic(io.context,0x33,0x23,&value) && value==80 && model.opens==2 && model.pnp_unregs==1 && model.pnp_regs==2 && model.claims==2);

    /* A replacement can start with exactly the same vote word. Rejection and
     * ACK belonging to the detached instance must never reach its IRP stack. */
    ULONG requests=model.requests;
    vote.reg=TG_GPU_PMIC_REPORT_EMC_FAILURE;vote.write=1;
    CHECK(TgGpuPlatformTryVoteRequest(&platform,&vote,&original_provider)==STATUS_DEVICE_NOT_READY);
    vote.reg=TG_GPU_PMIC_REPORT_EMC_ACK;
    CHECK(TgGpuPlatformTryVoteRequest(&platform,&vote,&original_provider)==STATUS_DEVICE_NOT_READY);
    CHECK(model.requests==requests);
    ULONG new_provider=original_provider;
    vote=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_QUERY_EMC_VOTE,0,0};
    CHECK(TgGpuPlatformTryVoteRequest(&platform,&vote,&new_provider)==STATUS_SUCCESS);
    CHECK(new_provider!=original_provider && vote.value==((1u<<8)|98u));
    vote.reg=TG_GPU_PMIC_REPORT_EMC_ACK;vote.write=1;
    CHECK(TgGpuPlatformTryVoteRequest(&platform,&vote,&new_provider)==STATUS_SUCCESS);

    rem.Event=GUID_TARGET_DEVICE_QUERY_REMOVE;
    CHECK(saved_cb(&rem,saved_ctx)==STATUS_SUCCESS);
    rem.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
    CHECK(saved_cb(&rem,saved_ctx)==STATUS_SUCCESS);
    model.claim=STATUS_DEVICE_NOT_READY;before_requests=model.requests;
    CHECK(!io.read_pmic(io.context,0x33,0x23,&value));
    CHECK(platform.NeedReclaim && model.requests==before_requests+1);
    model.claim=STATUS_SUCCESS;platform.IoStatus=STATUS_SUCCESS;
    CHECK(io.read_pmic(io.context,0x33,0x23,&value) && !platform.NeedReclaim);

    /* First-error IoStatus latch persists across subsequent healthy requests
     * until the restart epoch explicitly resets IoStatus. */
    io.write32(io.context,0x57fffffcu,0u);
    CHECK(platform.IoStatus==STATUS_DEVICE_CONFIGURATION_ERROR);
    model.completion=STATUS_IO_TIMEOUT;
    CHECK(!io.read_pmic(io.context,0x33,0x23,&value));
    CHECK(platform.IoStatus==STATUS_DEVICE_CONFIGURATION_ERROR); /* retains first error */
    model.completion=STATUS_SUCCESS;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS &&
          platform.IoStatus==STATUS_DEVICE_CONFIGURATION_ERROR);
    platform.IoStatus=STATUS_SUCCESS;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS &&
          platform.IoStatus==STATUS_SUCCESS);
    {
        struct tg_gpu_pmic_request opt={0x33,0x23,0,0};
        model.completion=STATUS_IO_DEVICE_ERROR;
        CHECK(TgGpuPlatformTryRequest(&platform,&opt)==STATUS_IO_DEVICE_ERROR &&
              platform.IoStatus==STATUS_SUCCESS);
        model.completion=STATUS_IO_TIMEOUT;
        CHECK(TgGpuPlatformTryRequest(&platform,&opt)==STATUS_IO_TIMEOUT &&
              platform.IoStatus==STATUS_SUCCESS);
        model.completion=STATUS_SUCCESS;
    }

    /* SwitchFan D0Entry / SelfManagedIoRestart window: bounded passive retry
     * recovers without poisoning IoStatus, caps at 12 retries, and never
     * retries genuine >=80 C over-temperature rejections. */
    {
        ULONG sleeps_before=model.sleeps;
        model.addr_not_ready_polls=2;
        model.temp_not_ready_polls=3;
        address=0;
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS &&
              address==0x33 && platform.IoStatus==STATUS_SUCCESS &&
              model.sleeps==sleeps_before+5);

        sleeps_before=model.sleeps;
        model.temp_not_ready_polls=20;
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_NOT_READY &&
              platform.IoStatus==STATUS_DEVICE_NOT_READY &&
              model.sleeps==sleeps_before+12);
        model.temp_not_ready_polls=0;
        platform.IoStatus=STATUS_SUCCESS;

        sleeps_before=model.sleeps;
        model.temp=80;
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_NOT_READY &&
              platform.IoStatus==STATUS_DEVICE_NOT_READY &&
              model.sleeps==sleeps_before);
        model.temp=42;
        platform.IoStatus=STATUS_SUCCESS;

        /* QueryRemove/Closing aborts preflight_query_ready immediately with 0 sleeps. */
        sleeps_before=model.sleeps;
        platform.QueryRemove=TRUE;
        model.addr_not_ready_polls=5;
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_NOT_READY &&
              platform.IoStatus==STATUS_DEVICE_NOT_READY &&
              model.sleeps==sleeps_before);
        platform.QueryRemove=FALSE;
        model.addr_not_ready_polls=0;
        platform.IoStatus=STATUS_SUCCESS;
    }

    /* A non-owned but running adapter must not change its rail. */
    registers[0][0]=0;
    for(UINT i=0;i<TgGpuApertureCount;++i){
        if(bases[i]==0x60006280u)registers[i][3]=0;
        if(bases[i]==0x7000e6d4u)registers[i][0]=0;
    }
    model.enable=0x54;model.voltage=0x46;
    TgGpuPlatformAbandonRail(&platform,address);
    CHECK(platform.RailClaimed && !model.releases);
    for(UINT i=0;i<TgGpuApertureCount;++i){
        if(bases[i]==0x60006280u)registers[i][3]=UINT32_MAX;
        if(bases[i]==0x7000e6d4u)registers[i][0]=1;
    }
    TgGpuPlatformAbandonRail(&platform,address);
    CHECK(platform.RailClaimed && !model.releases);
    for(UINT i=0;i<TgGpuApertureCount;++i)
        if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
    /* Early CLAIM_RAIL unwind only after reset + clamp read back. */
    TgGpuPlatformAbandonRail(&platform,address);
    CHECK(!platform.RailClaimed && model.releases==1);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    CHECK(!model.live_pool && !model.live_irps && !model.file_refs && !model.owner_refs);
    CHECK(WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL));ExitProcess(0);
}
#endif
