/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include <wdmguid.h>
#include "../windows/dvfs.h"
#include "../windows/hardware.h"
#include "../windows/platform_io.h"
#include "../windows/work_queue.h"
#include "../platform/t214_resources.h"
#include "../integration/switch-lite/windows/SwitchFan/src/t214/emc_gpu_vote.h"
#include <ntimage.h>

static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
#define CHECK(x) do {if(!(x))ExitProcess(__LINE__);}while(0)
void *memset(void *p,int c,size_t n){UCHAR *d=p;while(n--)*d++=(UCHAR)c;return p;}
void *memcpy(void *p,const void *s,size_t n){UCHAR *d=p;const UCHAR *a=s;while(n--)*d++=*a++;return p;}
int memcmp(const void *a,const void *b,size_t n){const UCHAR *x=a,*y=b;for(;n--;++x,++y)if(*x!=*y)return (int)*x-(int)*y;return 0;}

static TG_GPU_RESOURCES resources;
static TG_GPU_PLATFORM platform;
static TG_GPU_HARDWARE hardware;
static TG_GPU_WORK_QUEUE queue;
static TG_GPU_CONTEXT_ENGINE engine;
static ULONG gpu_regs[0x140000/4];
static ULONG misc_regs[TgGpuApertureCount][4],lic[T214_GPU_LIC_SIZE/4];
static const ULONG bases[]={T214_GPU_REG_BASE,T214_GPU_BAR1_BASE,T214_GPU_AUX_BASE,
#define BASE(b,s) b,
    T214_GPU_EXTRA_RESOURCES(BASE)
#undef BASE
};

static struct {
    ULONGLONG qpc_ticks;
    ULONG policy,temp,emc_khz,reported_khz,reported_uv,reported_status;
    ULONG emc_vote,emc_ack,emc_failed,emc_failures;
    ULONG emc_failure_reports;
    NTSTATUS emc_report_status;
    NTSTATUS telemetry_rail_status,telemetry_report_status;
    BOOLEAN emc_report_applied_before_error;
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE provider_notify;
    PVOID provider_context;
    UCHAR rail;
    BOOLEAN counter_bad,fail_target_counter,stop_on_gate;
    UINT32 fail_khz;
    UINT gate_depth,pmic_writes;
    KIRQL level;
} model;

static NTSTATUS NTAPI OpenFile(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES attrs,
    PIO_STATUS_BLOCK iosb,PLARGE_INTEGER size,ULONG attr,ULONG share,ULONG disp,
    ULONG opts,PVOID ea,ULONG ea_size)
{
    UNREFERENCED_PARAMETER(access);UNREFERENCED_PARAMETER(attrs);
    UNREFERENCED_PARAMETER(size);UNREFERENCED_PARAMETER(attr);
    UNREFERENCED_PARAMETER(share);UNREFERENCED_PARAMETER(disp);
    UNREFERENCED_PARAMETER(opts);UNREFERENCED_PARAMETER(ea);UNREFERENCED_PARAMETER(ea_size);
    iosb->Status=STATUS_SUCCESS;*handle=(HANDLE)123;return STATUS_SUCCESS;
}
static NTSTATUS NTAPI CloseHandle(HANDLE h)
{CHECK(h==(HANDLE)123 || h==(HANDLE)456);return STATUS_SUCCESS;}
static NTSTATUS NTAPI OpenKey(PHANDLE key,ACCESS_MASK access,POBJECT_ATTRIBUTES attrs)
{
    UNREFERENCED_PARAMETER(key);UNREFERENCED_PARAMETER(access);UNREFERENCED_PARAMETER(attrs);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
static NTSTATUS NTAPI QueryValueKey(HANDLE key,PUNICODE_STRING name,KEY_VALUE_INFORMATION_CLASS cls,
    PVOID info,ULONG length,PULONG result_len)
{
    UNREFERENCED_PARAMETER(key);UNREFERENCED_PARAMETER(name);UNREFERENCED_PARAMETER(cls);
    UNREFERENCED_PARAMETER(info);UNREFERENCED_PARAMETER(length);UNREFERENCED_PARAMETER(result_len);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
static POBJECT_TYPE dvfs_file_type=(POBJECT_TYPE)(ULONG_PTR)0x46494c45u;
POBJECT_TYPE *IoFileObjectType=&dvfs_file_type;
static DEVICE_OBJECT dvfs_dev={.StackSize=1};
static DRIVER_OBJECT dvfs_driver;
static DEVICE_OBJECT dvfs_owner={.DriverObject=&dvfs_driver,.StackSize=1};
static FILE_OBJECT dvfs_file={.DeviceObject=&dvfs_dev};
static UCHAR dvfs_pool[512],dvfs_irp_buf[512];
static VOID NTAPI DvfsInitSpin(PKSPIN_LOCK l){*l=0;}
static VOID NTAPI DvfsInitEvent(PRKEVENT ev,EVENT_TYPE t,BOOLEAN state){CHECK(ev && t==NotificationEvent);ev->Header.SignalState=state?1:0;}
static PVOID NTAPI DvfsAllocPool2(POOL_FLAGS f,SIZE_T b,ULONG t){UNREFERENCED_PARAMETER(f);UNREFERENCED_PARAMETER(t);CHECK(b<=sizeof(dvfs_pool));memset(dvfs_pool,0,b);return dvfs_pool;}
static VOID NTAPI DvfsFreePool(PVOID p,ULONG t){UNREFERENCED_PARAMETER(t);CHECK(p==dvfs_pool);}
static PIRP NTAPI DvfsAllocIrp(CCHAR s,BOOLEAN q){UNREFERENCED_PARAMETER(q);PIRP irp=(PIRP)dvfs_irp_buf;memset(irp,0,sizeof(dvfs_irp_buf));irp->Type=IO_TYPE_IRP;irp->Size=(USHORT)sizeof(dvfs_irp_buf);irp->StackCount=s;irp->CurrentLocation=(CCHAR)(s+1);irp->Tail.Overlay.CurrentStackLocation=(PIO_STACK_LOCATION)(irp+1)+s;return irp;}
static VOID NTAPI DvfsFreeIrp(PIRP irp){CHECK((PVOID)irp==dvfs_irp_buf);}
static NTSTATUS NTAPI DvfsSetCompEx(PDEVICE_OBJECT d,PIRP irp,PIO_COMPLETION_ROUTINE cb,PVOID ctx,BOOLEAN s,BOOLEAN e,BOOLEAN c)
{CHECK(d==&dvfs_owner);IoSetCompletionRoutine(irp,cb,ctx,s,e,c);return STATUS_SUCCESS;}
static NTSTATUS NTAPI DvfsRefHandle(HANDLE h,ACCESS_MASK a,POBJECT_TYPE t,KPROCESSOR_MODE m,PVOID *o,POBJECT_HANDLE_INFORMATION i)
{UNREFERENCED_PARAMETER(h);UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(t);UNREFERENCED_PARAMETER(m);UNREFERENCED_PARAMETER(i);*o=&dvfs_file;return STATUS_SUCCESS;}
static LONG_PTR FASTCALL DvfsRefObj(PVOID o){CHECK(o==&dvfs_file || o==&dvfs_owner);return 1;}
static LONG_PTR FASTCALL DvfsDerefObj(PVOID o){CHECK(o==&dvfs_file || o==&dvfs_owner);return 0;}
static PDEVICE_OBJECT NTAPI DvfsRelDev(PFILE_OBJECT fo){CHECK(fo==&dvfs_file);return &dvfs_dev;}
static BOOLEAN NTAPI DvfsCancelIrp(PIRP irp){UNREFERENCED_PARAMETER(irp);return FALSE;}
static NTSTATUS NTAPI DvfsRegPnp(IO_NOTIFICATION_EVENT_CATEGORY c,ULONG f,PVOID d,PDRIVER_OBJECT drv,PDRIVER_NOTIFICATION_CALLBACK_ROUTINE cb,PVOID ctx,PVOID *e)
{UNREFERENCED_PARAMETER(c);UNREFERENCED_PARAMETER(f);UNREFERENCED_PARAMETER(d);UNREFERENCED_PARAMETER(drv);model.provider_notify=cb;model.provider_context=ctx;*e=(PVOID)1;return STATUS_SUCCESS;}
static NTSTATUS NTAPI DvfsUnregPnp(PVOID e){UNREFERENCED_PARAMETER(e);return STATUS_SUCCESS;}
static NTSTATUS FASTCALL DvfsCallDriver(PDEVICE_OBJECT dev,PIRP irp)
{
    PIO_STACK_LOCATION sp=IoGetNextIrpStackLocation(irp);
    struct tg_gpu_pmic_request *op=irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status=STATUS_SUCCESS;
    CHECK(model.level==PASSIVE_LEVEL && dev==&dvfs_dev && sp->Parameters.DeviceIoControl.IoControlCode==TG_GPU_IOCTL_PMIC);
    if(op->reg==TG_GPU_PMIC_QUERY_ADDRESS)op->value=0x33;
    else if(op->reg==TG_GPU_PMIC_QUERY_TEMP)op->value=model.temp;
    else if(op->reg==TG_GPU_PMIC_QUERY_EMC_KHZ)op->value=model.emc_khz;
    else if(op->reg==TG_GPU_PMIC_QUERY_EMC_VOTE)
        op->value=model.emc_failed==model.emc_vote?model.emc_ack:model.emc_vote;
    else if(op->reg==TG_GPU_PMIC_REPORT_EMC_ABI){CHECK(op->write==1 && op->value==TG_GPU_EMC_VOTE_ABI);}
    else if(op->reg==TG_GPU_PMIC_REPORT_EMC_ACK){
        CHECK(op->write==1 && model.gate_depth && !queue.Current.Engine && !engine.Channel.referenced);
        CHECK(op->value==model.emc_vote && hardware.Clock.running &&
              250000u+5000u*model.rail>=TG_GPU_EMC_VOTE_UV(op->value));
        model.emc_ack=op->value;
    }
    else if(op->reg==TG_GPU_PMIC_REPORT_EMC_FAILURE){
        CHECK(op->write==1 && op->value==model.emc_vote);
        if(!model.gate_depth) {
            CHECK(hardware.Dvfs.EmcFailedWord==op->value);
            CHECK(hardware.Dvfs.EmcVoteWord==hardware.Dvfs.EmcAckWord);
        }
        ++model.emc_failure_reports;
        status=model.emc_report_status;
        if(NT_SUCCESS(status) || model.emc_report_applied_before_error) {
            model.emc_failed=op->value;
            ++model.emc_failures;
        }
    }
    else if(op->reg==TG_GPU_PMIC_QUERY_DVFS_POLICY)op->value=model.policy;
    else if(op->reg==TG_GPU_PMIC_REPORT_ABI){
        CHECK(op->write==1 && op->value==TG_GPU_LIVE_COUNTER_ABI);
        status=model.telemetry_report_status;
    }
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_KHZ){
        CHECK(op->write==1);status=model.telemetry_report_status;
        if(NT_SUCCESS(status))model.reported_khz=op->value;
    }
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_UV){
        CHECK(op->write==1);status=model.telemetry_report_status;
        if(NT_SUCCESS(status))model.reported_uv=op->value;
    }
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_STATUS){
        CHECK(op->write==1);status=model.telemetry_report_status;
        if(NT_SUCCESS(status))model.reported_status=op->value;
    }
    else {
        CHECK(op->address==0x33);
        if(op->reg==0x06){CHECK(!op->write);op->value=0x41;}
        else {
            CHECK(op->reg==0x23);
            if(op->write){model.rail=(UCHAR)op->value;++model.pmic_writes;}
            else if(!NT_SUCCESS(model.telemetry_rail_status))status=model.telemetry_rail_status;
            else op->value=model.rail;
        }
    }
    irp->IoStatus.Status=status;irp->IoStatus.Information=NT_SUCCESS(status)?sizeof(*op):0;
    (VOID)sp->CompletionRoutine(dev,irp,sp->Context);
    return status;
}
static LARGE_INTEGER NTAPI QueryQpc(PLARGE_INTEGER freq)
{
    LARGE_INTEGER out;
    if(freq)freq->QuadPart=1000000LL; /* 1 tick = 1 us in controlled model */
    out.QuadPart=(LONGLONG)model.qpc_ticks;return out;
}
static NTSTATUS NTAPI WaitObject(PVOID obj,KWAIT_REASON reason,KPROCESSOR_MODE mode,
    BOOLEAN alertable,PLARGE_INTEGER timeout)
{
    CHECK(reason==Executive && mode==KernelMode && !alertable && model.level==PASSIVE_LEVEL);
    if(timeout){CHECK(timeout->QuadPart==-10000000LL);return STATUS_SUCCESS;}
    CHECK(obj==&queue.DispatchGate);
    ++model.gate_depth;
    if(model.stop_on_gate)hardware.Dvfs.Configured=0;
    return STATUS_SUCCESS;
}
static LONG NTAPI ReleaseMutex(PRKMUTEX m,BOOLEAN wait)
{CHECK(m==&queue.DispatchGate && !wait && model.gate_depth && model.level==PASSIVE_LEVEL);--model.gate_depth;return 0;}
static LONG NTAPI SetEvent(PRKEVENT e,KPRIORITY prio,BOOLEAN wait)
{CHECK((e==&queue.Wake || ((PUCHAR)e>=dvfs_pool && (PUCHAR)e<dvfs_pool+sizeof(dvfs_pool))) && !prio && !wait);e->Header.SignalState=1;return 0;}
static KIRQL NTAPI LockSpin(PKSPIN_LOCK l)
{KIRQL old=model.level;CHECK(!*l);*l=1;model.level=DISPATCH_LEVEL;return old;}
static VOID NTAPI UnlockSpin(PKSPIN_LOCK l,KIRQL old)
{CHECK(*l && model.level==DISPATCH_LEVEL);*l=0;model.level=old;}
static VOID NTAPI Stall(ULONG us){UNREFERENCED_PARAMETER(us);}
static NTSTATUS NTAPI DelayThread(KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER interval)
{UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(alertable);UNREFERENCED_PARAMETER(interval);return STATUS_SUCCESS;}

static UINT32 GpuRead32(void *ctx,UINT32 addr)
{
    UINT32 v=TgGpuPlatformIo(&platform).read32(ctx,addr);
    return v;
}
static void GpuWrite32(void *ctx,UINT32 addr,UINT32 val)
{
    TgGpuPlatformIo(&platform).write32(ctx,addr,val);
    if(addr==0x57137000u && (val&1u)) {
        gpu_regs[0x137014u/4]|=1u<<31;
        gpu_regs[0x137018u/4]|=20u<<24;
    }
    if(addr==0x57134124u && val==0x00110320u) {
        UINT32 coeff=gpu_regs[0x137004u/4];
        UINT32 m=coeff&0xffu,n=(coeff>>8)&0xffu,p=(coeff>>16)&0x3fu;
        UINT32 khz=(m && p)?(38400u*n/m/p/2u):0;
        BOOLEAN bad=model.counter_bad || (model.fail_target_counter && khz==model.fail_khz);
        gpu_regs[0x134128u/4]=bad?100u:(khz/48u);
    }
}

NTSTATUS (NTAPI * const __imp_ZwCreateFile)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,
    PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG)=OpenFile;
NTSTATUS (NTAPI * const __imp_ZwClose)(HANDLE)=CloseHandle;
NTSTATUS (NTAPI * const __imp_ZwOpenKey)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES)=OpenKey;
NTSTATUS (NTAPI * const __imp_ZwQueryValueKey)(HANDLE,PUNICODE_STRING,KEY_VALUE_INFORMATION_CLASS,
    PVOID,ULONG,PULONG)=QueryValueKey;
NTSTATUS (NTAPI * const __imp_ObReferenceObjectByHandle)(HANDLE,ACCESS_MASK,POBJECT_TYPE,KPROCESSOR_MODE,PVOID *,POBJECT_HANDLE_INFORMATION)=DvfsRefHandle;
LONG_PTR (FASTCALL * const __imp_ObfReferenceObject)(PVOID)=DvfsRefObj;
LONG_PTR (FASTCALL * const __imp_ObfDereferenceObject)(PVOID)=DvfsDerefObj;
PDEVICE_OBJECT (NTAPI * const __imp_IoGetRelatedDeviceObject)(PFILE_OBJECT)=DvfsRelDev;
VOID (NTAPI * const __imp_KeInitializeSpinLock)(PKSPIN_LOCK)=DvfsInitSpin;
VOID (NTAPI * const __imp_KeInitializeEvent)(PRKEVENT,EVENT_TYPE,BOOLEAN)=DvfsInitEvent;
PVOID (NTAPI * const __imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=DvfsAllocPool2;
VOID (NTAPI * const __imp_ExFreePoolWithTag)(PVOID,ULONG)=DvfsFreePool;
PIRP (NTAPI * const __imp_IoAllocateIrp)(CCHAR,BOOLEAN)=DvfsAllocIrp;
VOID (NTAPI * const __imp_IoFreeIrp)(PIRP)=DvfsFreeIrp;
NTSTATUS (NTAPI * const __imp_IoSetCompletionRoutineEx)(PDEVICE_OBJECT,PIRP,PIO_COMPLETION_ROUTINE,PVOID,BOOLEAN,BOOLEAN,BOOLEAN)=DvfsSetCompEx;
BOOLEAN (NTAPI * const __imp_IoCancelIrp)(PIRP)=DvfsCancelIrp;
NTSTATUS (FASTCALL * const __imp_IofCallDriver)(PDEVICE_OBJECT,PIRP)=DvfsCallDriver;
NTSTATUS (NTAPI * const __imp_IoRegisterPlugPlayNotification)(IO_NOTIFICATION_EVENT_CATEGORY,ULONG,PVOID,PDRIVER_OBJECT,
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE,PVOID,PVOID *)=DvfsRegPnp;
NTSTATUS (NTAPI * const __imp_IoUnregisterPlugPlayNotificationEx)(PVOID)=DvfsUnregPnp;
LARGE_INTEGER (NTAPI * const __imp_KeQueryPerformanceCounter)(PLARGE_INTEGER)=QueryQpc;
NTSTATUS (NTAPI * const __imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,
    BOOLEAN,PLARGE_INTEGER)=WaitObject;
LONG (NTAPI * const __imp_KeReleaseMutex)(PRKMUTEX,BOOLEAN)=ReleaseMutex;
LONG (NTAPI * const __imp_KeSetEvent)(PRKEVENT,KPRIORITY,BOOLEAN)=SetEvent;
KIRQL (NTAPI * const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=LockSpin;
VOID (NTAPI * const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=UnlockSpin;
VOID (NTAPI * const __imp_KeStallExecutionProcessor)(ULONG)=Stall;
NTSTATUS (NTAPI * const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=DelayThread;

static void fail_clock_vote(ULONG word, NTSTATUS report_status)
{
    ULONG rollbacks=hardware.Dvfs.Rollbacks;
    model.emc_vote=word;
    model.emc_report_status=report_status;
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,614400u,614400u,6);
    model.fail_target_counter=TRUE;
    model.fail_khz=614400u;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Clock.running && hardware.Dvfs.Rollbacks==rollbacks+1);
    CHECK(model.emc_ack!=word);
    model.fail_target_counter=FALSE;
}

static void test_failure_report_delivery(void)
{
    struct tg_emc_gpu_vote provider={.request=model.emc_vote,.failed=model.emc_failed,
        .acknowledged=model.emc_ack,.guard_uv=TG_GPU_EMC_VOTE_UV(model.emc_ack)};
    ULONG reports=model.emc_failure_reports;
    CHECK(tg_emc_gpu_request(&provider,740000u,FALSE));
    ULONG rejected=provider.request;
    fail_clock_vote(rejected,STATUS_DEVICE_NOT_READY);
    CHECK(model.emc_failure_reports==reports+1 && model.emc_failed!=rejected);
    CHECK(tg_emc_gpu_request(&provider,740000u,FALSE) && provider.request==rejected);

    /* Keep a channel referenced while polling. Reporting the failure does
     * not touch the rail, ACK the vote or repeat the clock operation. */
    engine.Channel.referenced=true;
    UINT writes=model.pmic_writes;
    ULONG rollbacks=hardware.Dvfs.Rollbacks;
    model.qpc_ticks+=100000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_failure_reports==reports+1);
    model.qpc_ticks+=200000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_failure_reports==reports+2);
    model.emc_report_status=STATUS_SUCCESS;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_failed==rejected);
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_failure_reports==reports+3);
    CHECK(model.pmic_writes==writes && hardware.Dvfs.Rollbacks==rollbacks && model.emc_ack!=rejected);
    engine.Channel.referenced=false;

    /* A replacement that arrives before the next poll supersedes a failed
     * notification. Only that replacement may receive the normal ACK. */
    provider.failed=model.emc_failed;
    CHECK(tg_emc_gpu_request(&provider,740000u,FALSE) && provider.request!=rejected);
    fail_clock_vote(provider.request,STATUS_DEVICE_NOT_READY);
    reports=model.emc_failure_reports;
    CHECK(tg_emc_gpu_request(&provider,590000u,TRUE));
    model.emc_vote=provider.request;
    model.emc_report_status=STATUS_SUCCESS;
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,460800u,460800u,7);
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_ack==provider.request);
    CHECK(model.emc_failure_reports==reports);

    /* If the provider already applied the failure, its vote query returns
     * the previous ACK. An unavailable completion must not duplicate it. */
    model.emc_report_applied_before_error=TRUE;
    CHECK(tg_emc_gpu_request(&provider,740000u,FALSE));
    fail_clock_vote(provider.request,STATUS_DEVICE_NOT_READY);
    model.emc_report_applied_before_error=FALSE;
    CHECK(model.emc_failed==provider.request);
    reports=model.emc_failure_reports;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_failure_reports==reports);
    provider.failed=model.emc_failed;

    /* Older bridges may reject the optional failure opcode permanently. */
    CHECK(tg_emc_gpu_request(&provider,740000u,FALSE));
    fail_clock_vote(provider.request,STATUS_INVALID_DEVICE_REQUEST);
    reports=model.emc_failure_reports;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_failure_reports==reports);
    model.emc_report_status=STATUS_SUCCESS;
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,460800u,460800u,8);
    model.qpc_ticks+=2500000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
}

static void notify_provider(BOOLEAN removed)
{
    TARGET_DEVICE_REMOVAL_NOTIFICATION n={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&dvfs_file};
    CHECK(model.provider_notify(&n,model.provider_context)==STATUS_SUCCESS);
    n.Event=removed?GUID_TARGET_DEVICE_REMOVE_COMPLETE:GUID_TARGET_DEVICE_REMOVE_CANCELLED;
    CHECK(model.provider_notify(&n,model.provider_context)==STATUS_SUCCESS);
    if(removed) {
        model.emc_vote=0;
        model.emc_ack=0;
        model.emc_failed=0;
    }
}

static void test_provider_replacement(void)
{
    ULONG old_vote=(1u<<8)|98u;
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,460800u,460800u,9);
    model.emc_vote=old_vote;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_ack==old_vote);
    CHECK(hardware.Clock.rail_uv==740000u);

    notify_provider(TRUE);
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && !model.emc_ack);
    CHECK(hardware.Clock.rail_uv==740000u); /* No request from the replacement yet. */
    model.emc_vote=old_vote;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_ack==old_vote);
    model.emc_vote=2u<<8; /* The replacement explicitly releases the floor. */
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_ack==model.emc_vote);
    CHECK(hardware.Clock.rail_uv<740000u);

    /* A cancelled removal reopens the same provider. It still needs the
     * undelivered rejection for its outstanding request. */
    ULONG rejected=(3u<<8)|98u;
    fail_clock_vote(rejected,STATUS_DEVICE_NOT_READY);
    notify_provider(FALSE);
    model.emc_report_status=STATUS_SUCCESS;
    engine.Channel.referenced=true;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_failed==rejected);
    engine.Channel.referenced=false;

    /* A new provider may instead issue the very same previously rejected
     * word. Its own transaction must be allowed to complete. */
    rejected=(4u<<8)|98u;
    fail_clock_vote(rejected,STATUS_DEVICE_NOT_READY);
    ULONG reports=model.emc_failure_reports;
    notify_provider(TRUE);
    model.emc_report_status=STATUS_SUCCESS;
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,460800u,460800u,10);
    model.emc_vote=rejected;
    model.qpc_ticks+=300000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_ack==rejected);
    CHECK(!model.emc_failed && model.emc_failure_reports==reports);
    model.emc_vote=5u<<8;
    model.qpc_ticks+=2500000;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_ack==model.emc_vote);
    CHECK(hardware.Clock.target_khz==460800u && hardware.Clock.rail_uv==590000u);
}

void start(void)
{
    static const char pass[]="PASS: WDDM HighOPT GPU DVFS quiescent gating, policy switch, thermal throttle and rollback recovery (controlled substitutes)\r\n";
    ULONG written;
    TG_GPU_QUEUE_ENTRY dummy1={0},dummy2={0};

    memset(&resources,0,sizeof(resources));
    memset(&platform,0,sizeof(platform));
    memset(&hardware,0,sizeof(hardware));
    memset(&queue,0,sizeof(queue));
    memset(&engine,0,sizeof(engine));
    memset(&model,0,sizeof(model));
    memset(gpu_regs,0,sizeof(gpu_regs));
    memset(misc_regs,0,sizeof(misc_regs));

    for(UINT i=0;i<TgGpuApertureCount;++i) {
        resources.Memory[i].Type=CmResourceTypeMemory;
        resources.Memory[i].u.Memory.Start.QuadPart=bases[i];
        if(i==0) {
            resources.Memory[i].u.Memory.Length=sizeof(gpu_regs);
            resources.Mapping[i]=gpu_regs;
        } else {
            resources.Memory[i].u.Memory.Length=sizeof(misc_regs[i]);
            resources.Mapping[i]=misc_regs[i];
            if(bases[i]==0x7000f930u)misc_regs[i][0]=1659u;
        }
    }
    resources.LicMapping=lic;
    gpu_regs[0]=0x12e000b1u;
    gpu_regs[0x137000u/4]=2u;
    gpu_regs[0x20798u/4]=0x105u;
    gpu_regs[0x20160u/4]=0x00220000u;

    model.qpc_ticks=1000000ULL;
    model.temp=42;
    model.emc_khz=1600000u;
    model.rail=110;
    model.policy=TG_GPU_DVFS_POLICY_PACK(0,460800u,460800u,0);

    dvfs_driver.DeviceObject=&dvfs_owner;
    TgGpuSetPlatformDriverObject(&dvfs_driver);
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    hardware.Io=TgGpuPlatformIo(&platform);
    hardware.Io.read32=GpuRead32;
    hardware.Io.write32=GpuWrite32;
    hardware.Power.owned=hardware.Power.ready=true;
    hardware.Power.pmic_address=0x33;
    hardware.Ready=TRUE;
    hardware.Engine=&engine;
    queue.Engine=&engine;
    InitializeListHead(&queue.Pending);
    CHECK(tg_gpu_clock_start(&hardware.Io,&hardware.Power,TG_GPU_LOW_POWER_KHZ,&hardware.Clock)==TG_GPU_CLOCK_OK);
    CHECK(hardware.Clock.target_khz==460800u && hardware.Clock.rail_uv==800000u);

    /* 1. Boot default Mode 0: stays fixed at 460.8 MHz / 800 mV. */
    TgGpuDvfsAttach(&hardware,&platform,&queue);
    CHECK(hardware.Dvfs.Configured==1 && hardware.Dvfs.Mode==TgGpuDvfsFixed);
    CHECK(model.reported_khz==460800u && model.reported_uv==800000u);
    model.qpc_ticks+=600000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,TRUE)==STATUS_SUCCESS);
    CHECK(hardware.Dvfs.Transitions==0 && hardware.Clock.target_khz==460800u && hardware.Clock.rail_uv==800000u);

    /* 2. SwitchFan enables Console Dynamic (HighOPT 153.6..768.0 MHz). */
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,153600u,768000u,1);
    model.qpc_ticks+=300000ULL;
    /* Refuses to touch clocks while Current.Engine or Channel.referenced is busy! */
    queue.Current.Engine=&engine;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Dvfs.Mode==TgGpuDvfsHighOpt && hardware.Dvfs.Transitions==0);
    queue.Current.Engine=NULL;
    engine.Channel.referenced=true;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Dvfs.Transitions==0 && hardware.Clock.target_khz==460800u);
    engine.Channel.referenced=false;

    /* 3. Quiescent idle holdoff steps down toward 153.6 MHz / 590 mV. */
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Clock.target_khz==230400u && hardware.Clock.rail_uv==590000u);
    model.qpc_ticks+=250000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Clock.target_khz==153600u && hardware.Clock.rail_uv==590000u);

    /* 4. Queued burst ramps 153.6 -> 460.8 -> 614.4 -> 768.0 MHz when quiescent. */
    InsertTailList(&queue.Pending,&dummy1.Link);
    InsertTailList(&queue.Pending,&dummy2.Link);
    model.qpc_ticks+=10000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,TRUE)==STATUS_SUCCESS);
    CHECK(hardware.Clock.target_khz==460800u && hardware.Clock.rail_uv==590000u);
    model.qpc_ticks+=10000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,TRUE)==STATUS_SUCCESS);
    CHECK(hardware.Clock.target_khz==614400u && hardware.Clock.rail_uv==590000u);
    model.qpc_ticks+=10000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,TRUE)==STATUS_SUCCESS);
    CHECK(hardware.Clock.target_khz==768000u && hardware.Clock.rail_uv==590000u);
    InitializeListHead(&queue.Pending);

    /* 5. Thermal throttle at >=80 C drops to 153.6 MHz, holds at 75 C, releases at 72 C. */
    model.temp=82;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,TRUE)==STATUS_SUCCESS);
    CHECK(hardware.Dvfs.ThermalThrottled && hardware.Clock.target_khz==153600u);
    model.temp=75;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,TRUE)==STATUS_SUCCESS);
    CHECK(hardware.Dvfs.ThermalThrottled && hardware.Clock.target_khz==153600u);
    model.temp=72;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,TRUE)==STATUS_SUCCESS);
    CHECK(!hardware.Dvfs.ThermalThrottled && hardware.Clock.target_khz==460800u);

    /* 6. SetStablePowerState pins to 460.8 MHz / 800 mV, then releases. */
    TgGpuDvfsSetStablePower(&hardware.Dvfs,TRUE);
    model.qpc_ticks+=10000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Clock.target_khz==460800u && hardware.Clock.rail_uv==800000u);
    TgGpuDvfsSetStablePower(&hardware.Dvfs,FALSE);

    /* 7. Recoverable rollback increments Rollbacks and stays STATUS_SUCCESS;
     *    double clock failure returns STATUS_DEVICE_DATA_ERROR for TDR.
     */
    model.fail_target_counter=TRUE;
    model.fail_khz=230400u;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Dvfs.Rollbacks==1 && hardware.Clock.running && hardware.Clock.target_khz==460800u);
    model.fail_target_counter=FALSE;

    /* Return to Mode 0 Default Fixed restores 460.8 MHz / 800 mV. */
    model.policy=TG_GPU_DVFS_POLICY_PACK(0,460800u,460800u,2);
    model.qpc_ticks+=2500000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(hardware.Dvfs.Mode==TgGpuDvfsFixed && hardware.Clock.target_khz==460800u && hardware.Clock.rail_uv==800000u);

    /* EMC acknowledgement is a real idle-boundary voltage transition, even
     * during a rollback cooldown or when only the request generation changes. */
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,460800u,460800u,3);
    model.emc_vote=(1u<<8)|98u; /* 740 mV */
    model.qpc_ticks+=300000ULL;
    hardware.Dvfs.CooldownUntilUs=model.qpc_ticks+2000000ULL;
    engine.Channel.referenced=true;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && !model.emc_ack);
    engine.Channel.referenced=false;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(model.emc_ack==model.emc_vote && hardware.Clock.rail_uv==740000u);
    model.emc_vote=(2u<<8)|98u;
    model.emc_khz=3200000u;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && model.emc_ack==model.emc_vote);
    CHECK(hardware.Dvfs.EmcKhz==3200000u);
    model.emc_khz=0; /* unavailable telemetry must not become 1600 MHz */
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && hardware.Dvfs.EmcKhz==3200000u);
    model.emc_khz=1600000u;
    model.emc_vote=(3u<<8)|68u;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(model.emc_ack==model.emc_vote && hardware.Clock.rail_uv==590000u);
    model.emc_vote=(4u<<8)|98u;
    model.qpc_ticks+=300000ULL;
    model.stop_on_gate=true;
    UINT writes_before_stop=model.pmic_writes;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(!hardware.Dvfs.Configured && model.emc_ack!=model.emc_vote && model.pmic_writes==writes_before_stop);
    model.stop_on_gate=false;
    hardware.Dvfs.Configured=1;
    hardware.Dvfs.CooldownUntilUs=0;
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,614400u,614400u,4);
    model.fail_target_counter=TRUE;model.fail_khz=614400u;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && hardware.Clock.running);
    CHECK(model.emc_failed==model.emc_vote && model.emc_failures==1 && model.emc_ack!=model.emc_vote);
    CHECK(hardware.Clock.rail_uv==740000u); /* failed F-up retains the raised voltage */
    model.fail_target_counter=FALSE;
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,460800u,460800u,5);
    model.qpc_ticks+=2500000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS);
    CHECK(model.emc_failures==1 && model.emc_ack!=model.emc_vote && hardware.Clock.rail_uv==590000u);

    test_failure_report_delivery();
    test_provider_replacement();

    /* Telemetry PMIC 0x23 reads and optional report IRPs must never poison
     * global Platform.IoStatus on transient I2C/IRP errors. */
    CHECK(platform.IoStatus==STATUS_SUCCESS);
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,460800u,460800u,11);
    model.telemetry_rail_status=STATUS_IO_DEVICE_ERROR;
    model.telemetry_report_status=STATUS_IO_TIMEOUT;
    model.qpc_ticks+=1100000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS && platform.IoStatus==STATUS_SUCCESS);
    model.telemetry_report_status=STATUS_SUCCESS;
    model.reported_uv=0xdeadbeefu;
    model.qpc_ticks+=1100000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS &&
          platform.IoStatus==STATUS_SUCCESS && model.reported_uv==0u);
    model.telemetry_rail_status=STATUS_IO_TIMEOUT;
    model.reported_uv=0xdeadbeefu;
    TgGpuDvfsAttach(&hardware,&platform,&queue);
    CHECK(platform.IoStatus==STATUS_SUCCESS && model.reported_uv==0u);
    model.telemetry_rail_status=STATUS_SUCCESS;
    model.qpc_ticks+=1100000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_SUCCESS &&
          platform.IoStatus==STATUS_SUCCESS && model.reported_uv==250000u+5000u*model.rail);

    /* Force fatal double counter loss -> STATUS_DEVICE_DATA_ERROR. */
    model.policy=TG_GPU_DVFS_POLICY_PACK(1,153600u,460800u,3);
    model.counter_bad=TRUE;
    model.qpc_ticks+=300000ULL;
    CHECK(TgGpuDvfsStep(&hardware,&queue,FALSE)==STATUS_DEVICE_DATA_ERROR && !hardware.Clock.running);

    CHECK(!model.gate_depth);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    CHECK(WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL));
    ExitProcess(0);
}
