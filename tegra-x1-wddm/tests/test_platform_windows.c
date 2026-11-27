/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include <wdmguid.h>
#include "../windows/platform_io.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>
#include "platform_unicode_fixture.h"
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
KUSER_SHARED_DATA ki_user_shared_data;
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
static DEVICE_OBJECT fake_target_device={.StackSize=1,.DeviceType=FILE_DEVICE_UNKNOWN};
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
static UCHAR workitem_slots[16][64];
static BOOLEAN workitem_used[16];
static BOOLEAN workitem_queued[16];

static struct {
    NTSTATUS open,close,io,wait,completion,claim,release,completion_registration;
    ULONG_PTR bytes;
    ULONG address,temp,opens,closes,requests,waits,cancels,stalls,sleeps;
    ULONG claims,releases,raw_writes,pnp_regs,pnp_unregs,safe_completions;
    BOOLEAN swbatt_wire;
    ULONG tagged_queries,plain_queries,last_claim_address;
    ULONG addr_not_ready_polls,temp_not_ready_polls;
    LONG file_refs,owner_refs,live_pool,live_irps,live_workitems;
    KIRQL irql;
    BOOLEAN defer_complete;
    BOOLEAN complete_in_wait;
    BOOLEAN complete_in_cancel;
    BOOLEAN remove_during_register;
    BOOLEAN query_remove_during_register_once;
    BOOLEAN remove_during_claim;
    BOOLEAN in_workitem_dispatch;
    BOOLEAN defer_workitem_once;
    BOOLEAN defer_compensate_once;
    BOOLEAN fail_pool_once;
    BOOLEAN fail_workitem_once;
    BOOLEAN pending_claim_once;
    BOOLEAN pending_release_once;
    BOOLEAN token_aware_provider;
    BOOLEAN claim_during_retry_read06;
    BOOLEAN provider_claimed;
    BOOLEAN provider_handoff_active;
    BOOLEAN provider_handoff_isolated;
    UCHAR provider_handoff_addr;
    ULONG provider_claim_seq;
    ULONG provider_claim_token;
    UINT unlock_hook_mode;
    UINT unlock_hook_arm_after;
    ULONGLONG sync_delay_100ns;
    ULONGLONG wait_delay_100ns;
    ULONGLONG inflight_sync_remaining_100ns;
    ULONGLONG open_delay_100ns;
    ULONGLONG pnp_reg_delay_100ns;
    ULONGLONG pnp_unreg_delay_100ns;
    ULONGLONG deferred_wi_remaining_100ns;
    PIO_WORKITEM deferred_wi_item;
    PIO_WORKITEM_ROUTINE deferred_wi_routine;
    PVOID deferred_wi_ctx;
    PIO_WORKITEM deferred_comp_item;
    PIO_WORKITEM_ROUTINE deferred_comp_routine;
    PVOID deferred_comp_ctx;
    TG_GPU_PLATFORM *concurrent_claim_platform;
    PIRP deferred_irp;
    PIRP pending_claim_irp;
    PIRP pending_release_irp;
    PIRP inflight_sync_irp;
    NTSTATUS inflight_sync_status;
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE pnp_cb;
    PVOID pnp_ctx;
    UCHAR enable,voltage;
} model;

VOID Img2_TgGpuSetPlatformDriverObject(PVOID DriverObject);
NTSTATUS Img2_TgGpuOpenPlatform(TG_GPU_PLATFORM *,const TG_GPU_RESOURCES *);
NTSTATUS Img2_TgGpuClosePlatform(TG_GPU_PLATFORM *);
NTSTATUS Img2_TgGpuPlatformPreflight(TG_GPU_PLATFORM *,UCHAR *PmicAddress);
NTSTATUS Img2_TgGpuPlatformAbandonRail(TG_GPU_PLATFORM *,UCHAR PmicAddress);
VOID Img2_TgGpuPlatformRefreshIsolation(TG_GPU_PLATFORM *,const TG_GPU_RESOURCES *);
VOID Img2_TgGpuHandOffUnresolvedRail(TG_GPU_PLATFORM *,BOOLEAN GpuIsolated);
NTSTATUS Img2_TgGpuRetryUnresolvedRailRelease(VOID);
BOOLEAN Img2_TgGpuQueryUnresolvedRail(BOOLEAN *GpuIsolated,UCHAR *PmicAddress,
                                      ULONG *RetryCount,NTSTATUS *LastStatus);

static PKSPIN_LOCK spin_stack[2];
static UINT spin_depth;
static VOID NTAPI InitSpin(PKSPIN_LOCK l){*l=0;}
static KIRQL NTAPI LockSpin(PKSPIN_LOCK l)
{
    KIRQL old=model.irql;
    /* KeAcquireSpinLock permits callers already at DISPATCH_LEVEL. Keep
     * recursion, depth and LIFO checks while modeling the publication pair. */
    CHECK(!*l && old<=DISPATCH_LEVEL && spin_depth<RTL_NUMBER_OF(spin_stack));
    spin_stack[spin_depth++]=l;*l=1;model.irql=DISPATCH_LEVEL;return old;
}
static VOID run_unlock_hook(PKSPIN_LOCK l);
static VOID NTAPI UnlockSpin(PKSPIN_LOCK l,KIRQL old)
{
    CHECK(*l && model.irql==DISPATCH_LEVEL && spin_depth && spin_stack[spin_depth-1]==l);
    --spin_depth;*l=0;model.irql=old;
    if(!spin_depth && model.unlock_hook_mode && l!=&platform.HandleLock)
        run_unlock_hook(l);
}
static PVOID NTAPI AllocPool2(POOL_FLAGS flags,SIZE_T bytes,ULONG tag)
{
    CHECK(model.irql==PASSIVE_LEVEL && flags==POOL_FLAG_NON_PAGED && bytes<=sizeof(pool_buf[0]) && tag==0x50674754u);
    if(model.fail_pool_once){model.fail_pool_once=FALSE;return NULL;}
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
    if(model.open_delay_100ns)
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.open_delay_100ns;
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
static PIO_WORKITEM NTAPI AllocWorkItem(PDEVICE_OBJECT dev)
{
    CHECK(model.irql==PASSIVE_LEVEL && dev==&fake_owner_device);
    if(model.fail_workitem_once){model.fail_workitem_once=FALSE;return NULL;}
    for(UINT i=0;i<16;++i)if(!workitem_used[i]){
        workitem_used[i]=TRUE;workitem_queued[i]=FALSE;++model.live_workitems;++model.owner_refs;
        memset(workitem_slots[i],0,sizeof(workitem_slots[i]));
        return (PIO_WORKITEM)workitem_slots[i];
    }
    return NULL;
}
static VOID NTAPI FreeWorkItem(PIO_WORKITEM wi)
{
    for(UINT i=0;i<16;++i)if((PVOID)wi==workitem_slots[i] && workitem_used[i]){
        CHECK(!workitem_queued[i]);
        workitem_used[i]=FALSE;--model.live_workitems;
        CHECK(model.owner_refs>0);--model.owner_refs;
        return;
    }
    ExitProcess(__LINE__);
}
static UINT find_workitem_slot(PIO_WORKITEM wi)
{
    for(UINT i=0;i<16;++i)if((PVOID)wi==workitem_slots[i] && workitem_used[i])return i;
    ExitProcess(__LINE__);
}
static VOID NTAPI QueueWorkItem(PIO_WORKITEM wi,PIO_WORKITEM_ROUTINE routine,WORK_QUEUE_TYPE qtype,PVOID ctx)
{
    CHECK(wi && routine && qtype==DelayedWorkQueue);
    UINT slot=find_workitem_slot(wi);
    /* WDM contract (GC-V9F-02): queuing an already-queued IO_WORKITEM corrupts
     * Executive worker LIST_ENTRY! Assert !workitem_queued[slot] unconditionally. */
    CHECK(!workitem_queued[slot]);
    if(model.defer_compensate_once) {
        model.defer_compensate_once=FALSE;
        workitem_queued[slot]=TRUE;
        model.deferred_comp_item=wi;
        model.deferred_comp_routine=routine;
        model.deferred_comp_ctx=ctx;
        return;
    }
    if(model.defer_workitem_once) {
        model.defer_workitem_once=FALSE;
        workitem_queued[slot]=TRUE;
        model.deferred_wi_item=wi;
        model.deferred_wi_routine=routine;
        model.deferred_wi_ctx=ctx;
        return;
    }
    workitem_queued[slot]=TRUE;
    workitem_queued[slot]=FALSE;
    BOOLEAN prev=model.in_workitem_dispatch;
    model.in_workitem_dispatch=TRUE;
    routine(&fake_owner_device,ctx);
    model.in_workitem_dispatch=prev;
}
static VOID run_deferred_workitem(VOID)
{
    if(model.deferred_wi_routine) {
        PIO_WORKITEM wi=model.deferred_wi_item;
        PIO_WORKITEM_ROUTINE r=model.deferred_wi_routine;
        PVOID c=model.deferred_wi_ctx;
        ULONGLONG saved_open_delay=model.open_delay_100ns;
        UINT slot=find_workitem_slot(wi);
        CHECK(workitem_queued[slot]);
        workitem_queued[slot]=FALSE;
        model.deferred_wi_item=NULL;
        model.deferred_wi_routine=NULL;
        model.deferred_wi_ctx=NULL;
        if(model.deferred_wi_remaining_100ns) {
            *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.deferred_wi_remaining_100ns;
            model.deferred_wi_remaining_100ns=0;
            model.open_delay_100ns=0;
        }
        BOOLEAN prev=model.in_workitem_dispatch;
        model.in_workitem_dispatch=TRUE;
        r(&fake_owner_device,c);
        model.in_workitem_dispatch=prev;
        model.open_delay_100ns=saved_open_delay;
    }
}
static VOID run_deferred_compensate(VOID)
{
    CHECK(model.deferred_comp_routine && model.deferred_comp_item);
    UINT slot=find_workitem_slot(model.deferred_comp_item);
    CHECK(workitem_queued[slot]);
    workitem_queued[slot]=FALSE;
    PIO_WORKITEM_ROUTINE r=model.deferred_comp_routine;
    PVOID c=model.deferred_comp_ctx;
    model.deferred_comp_item=NULL;
    model.deferred_comp_routine=NULL;
    model.deferred_comp_ctx=NULL;
    BOOLEAN prev=model.in_workitem_dispatch;
    model.in_workitem_dispatch=TRUE;
    r(&fake_owner_device,c);
    model.in_workitem_dispatch=prev;
}
static VOID finish_inflight_sync_irp(VOID)
{
    if(model.inflight_sync_irp) {
        PIRP irp=model.inflight_sync_irp;
        NTSTATUS st=model.inflight_sync_status;
        model.inflight_sync_irp=NULL;
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.inflight_sync_remaining_100ns;
        model.inflight_sync_remaining_100ns=0;
        irp->IoStatus.Status=st;
        irp->IoStatus.Information=NT_SUCCESS(st)?model.bytes:0;
        complete_irp(irp);
    }
}
static NTSTATUS NTAPI WaitObj(PVOID obj,KWAIT_REASON r,KPROCESSOR_MODE m,BOOLEAN alert,PLARGE_INTEGER timeout)
{
    PRKEVENT ev=obj;
    CHECK(model.irql==PASSIVE_LEVEL && ev && r==Executive && m==KernelMode && !alert &&
          timeout && timeout->QuadPart<0 && timeout->QuadPart>=-10000000LL);
    ++model.waits;
    if(model.deferred_wi_routine) {
        ULONGLONG wait_limit=(ULONGLONG)(-timeout->QuadPart);
        if(model.open_delay_100ns>=wait_limit) {
            *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=wait_limit;
            model.deferred_wi_remaining_100ns=model.open_delay_100ns-wait_limit;
            return STATUS_TIMEOUT;
        }
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=wait_limit;
        return STATUS_TIMEOUT;
    }
    if(model.pending_claim_irp || model.pending_release_irp) {
        ULONGLONG wait_limit=(ULONGLONG)(-timeout->QuadPart);
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=wait_limit;
        return STATUS_TIMEOUT;
    }
    if(model.inflight_sync_irp) {
        ULONGLONG wait_limit=(ULONGLONG)(-timeout->QuadPart);
        if(model.sync_delay_100ns<wait_limit) {
            PIRP irp=model.inflight_sync_irp;
            NTSTATUS st=model.inflight_sync_status;
            model.inflight_sync_irp=NULL;
            *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.sync_delay_100ns;
            irp->IoStatus.Status=st;
            irp->IoStatus.Information=NT_SUCCESS(st)?model.bytes:0;
            complete_irp(irp);
            CHECK(ev->Header.SignalState==1);
            return STATUS_SUCCESS;
        }
        /* Slow synchronous IoCallDriver on the worker thread exceeds caller's
         * remaining deadline: caller wakes at exact deadline (<= 600 ms) while
         * provider finishes later on the independent worker context (A04)! */
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=wait_limit;
        model.inflight_sync_remaining_100ns=model.sync_delay_100ns-wait_limit;
        return STATUS_TIMEOUT;
    }
    if(model.wait_delay_100ns)
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.wait_delay_100ns;
    if(model.complete_in_wait && model.deferred_irp) {
        PIRP irp=model.deferred_irp;
        model.deferred_irp=NULL;
        irp->IoStatus.Status=STATUS_SUCCESS;
        irp->IoStatus.Information=model.bytes;
        complete_irp(irp);
        return model.wait;
    }
    if(model.defer_complete)return model.wait;
    CHECK(ev->Header.SignalState==1);
    return model.wait;
}
static BOOLEAN NTAPI CancelIrp(PIRP irp)
{
    CHECK(irp==model.deferred_irp || irp==model.inflight_sync_irp ||
          irp==model.pending_claim_irp || irp==model.pending_release_irp);
    ++model.cancels;
    irp->Cancel=TRUE;
    if(irp==model.pending_claim_irp || irp==model.pending_release_irp)
        return FALSE;
    if(irp==model.deferred_irp && model.complete_in_cancel) {
        model.deferred_irp=NULL;
        irp->IoStatus.Status=STATUS_SUCCESS;
        irp->IoStatus.Information=model.bytes;
        complete_irp(irp);
    }
    return TRUE;
}
static NTSTATUS FASTCALL CallDriver(PDEVICE_OBJECT dev,PIRP irp)
{
    PIO_STACK_LOCATION sp=IoGetNextIrpStackLocation(irp);
    struct tg_gpu_pmic_request *op=irp->AssociatedIrp.SystemBuffer;
    CHECK(model.irql==PASSIVE_LEVEL && dev==&fake_target_device && irp->RequestorMode==KernelMode && sp->MajorFunction==IRP_MJ_DEVICE_CONTROL);
    CHECK(sp->FileObject==&fake_file && sp->Parameters.DeviceIoControl.IoControlCode==TG_GPU_IOCTL_PMIC);
    CHECK(sp->Parameters.DeviceIoControl.InputBufferLength==sizeof(*op) && sp->Parameters.DeviceIoControl.OutputBufferLength==sizeof(*op));
    if(model.pending_claim_once && op->reg==TG_GPU_PMIC_CLAIM_RAIL) {
        model.pending_claim_once=FALSE;
        model.pending_claim_irp=irp;
        return STATUS_PENDING;
    }
    if(model.pending_release_once && op->reg==TG_GPU_PMIC_RELEASE_RAIL) {
        model.pending_release_once=FALSE;
        model.pending_release_irp=irp;
        return STATUS_PENDING;
    }
    ++model.requests;
    NTSTATUS st=model.completion;
    if(model.swbatt_wire && op->reg==TG_GPU_PMIC_QUERY_ADDRESS) {
        if(op->address)++model.tagged_queries;
        else ++model.plain_queries;
    }
    if(model.swbatt_wire && !model.token_aware_provider && op->address &&
       op->reg>=TG_GPU_PMIC_QUERY_ADDRESS) {
        st=STATUS_ACCESS_DENIED;
    } else if(op->reg==TG_GPU_PMIC_QUERY_ADDRESS){
        const BOOLEAN q_rail=(op->address==TG_GPU_RAIL_QUERY_MAGIC);
        if(model.addr_not_ready_polls){--model.addr_not_ready_polls;st=STATUS_DEVICE_NOT_READY;}
        else {
            op->value=model.address;
            if(model.token_aware_provider && q_rail) {
                op->address=TG_GPU_RAIL_STATE_ACK |
                    (model.provider_claimed?TG_GPU_RAIL_STATE_CLAIMED:0u) |
                    (model.provider_handoff_active?TG_GPU_RAIL_STATE_HANDOFF:0u) |
                    (model.provider_handoff_isolated?TG_GPU_RAIL_STATE_ISOLATED:0u);
                if(model.provider_handoff_active && model.provider_handoff_addr)
                    op->value=model.provider_handoff_addr;
                op->write=((model.provider_claim_seq&0xffffu)<<16)|(model.provider_claim_token&0xffffu);
            }
        }
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
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_STATUS){
        CHECK(op->write==1);
        if(model.token_aware_provider && (op->address & TG_GPU_RAIL_HANDOFF_MASK)==TG_GPU_RAIL_HANDOFF_MAGIC) {
            ULONG tok=(op->value>>16)&0xffffu;
            UCHAR addr=(UCHAR)(op->address&0xffu);
            BOOLEAN act=(op->value&TG_GPU_RAIL_STATE_HANDOFF)!=0u;
            BOOLEAN iso=(op->value&TG_GPU_RAIL_STATE_ISOLATED)!=0u;
            if(model.provider_claimed && (!model.provider_claim_token || tok==model.provider_claim_token)) {
                model.provider_handoff_active=act;
                model.provider_handoff_isolated=act?iso:FALSE;
                if(addr)model.provider_handoff_addr=addr;
            } else if(!act) {
                model.provider_handoff_active=FALSE;
                model.provider_handoff_isolated=FALSE;
            }
        }
    }
    else if(op->reg==TG_GPU_PMIC_CLAIM_RAIL){
        CHECK(op->write==1 && op->value==TG_GPU_EMC_VOTE_ABI);
        ++model.claims;
        model.last_claim_address=op->address;
        if(!NT_SUCCESS(model.claim))st=model.claim;
        else if(model.swbatt_wire && !model.token_aware_provider)st=STATUS_INVALID_PARAMETER;
        else if(model.token_aware_provider) {
            ULONG next_seq=(model.provider_claim_seq+1u)&0xffffu;
            if(!next_seq)next_seq=1u;
            if(op->address>next_seq && op->address<=0xffffu)next_seq=op->address;
            model.provider_claim_seq=next_seq;
            model.provider_claimed=TRUE;
            model.provider_handoff_active=FALSE;
            model.provider_handoff_isolated=FALSE;
            model.provider_handoff_addr=(UCHAR)model.address;
            if(op->address!=0u) {
                model.provider_claim_token=next_seq;
                op->address=TG_GPU_RAIL_STATE_ACK|next_seq;
            } else {
                model.provider_claim_token=0u;
            }
        } else {
            model.provider_claimed=TRUE;
        }
        if(model.remove_during_claim){
            TARGET_DEVICE_REMOVAL_NOTIFICATION n={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
            CHECK(model.pnp_cb(&n,model.pnp_ctx)==STATUS_SUCCESS);
            n.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
            CHECK(model.pnp_cb(&n,model.pnp_ctx)==STATUS_SUCCESS);
        }
    } else if(op->reg==TG_GPU_PMIC_RELEASE_RAIL){
        ULONG req_tok=(op->value>>16)&0xffffu;
        ULONG rel_val=op->value&0xffffu;
        CHECK(op->write==1 && op->address==0x33 && rel_val==0x5446u);
        if(!model.token_aware_provider)CHECK(req_tok==0u);
        if(model.token_aware_provider &&
           ((model.provider_claim_token!=0u && req_tok!=model.provider_claim_token) ||
            (req_tok!=0u && !model.provider_claimed))) {
            st=STATUS_INVALID_DEVICE_STATE;
        } else {
            ++model.releases;
            if(!NT_SUCCESS(model.release))st=model.release;
            else {
                model.provider_claimed=FALSE;
                model.provider_claim_token=0u;
                model.provider_handoff_active=FALSE;
                model.provider_handoff_isolated=FALSE;
            }
        }
    } else {
        CHECK(op->address==0x33 && (op->reg==0x23 || op->reg==0x06));
        UCHAR *reg=op->reg==0x23?&model.voltage:&model.enable;
        if(op->write){*reg=(UCHAR)op->value;++model.raw_writes;}
        else {
            op->value=*reg;
#ifndef TG_PLATFORM_FIXTURE_ONLY
            if(op->reg==0x06 && model.claim_during_retry_read06 && model.concurrent_claim_platform) {
                UCHAR c_addr=0;
                model.claim_during_retry_read06=FALSE;
                CHECK(TgGpuOpenPlatform(model.concurrent_claim_platform,&resources)==STATUS_SUCCESS);
                CHECK(TgGpuPlatformPreflight(model.concurrent_claim_platform,&c_addr)==STATUS_SUCCESS && c_addr==0x33);
            }
#endif
        }
    }
    if(model.defer_complete){model.deferred_irp=irp;return STATUS_PENDING;}
    if(model.in_workitem_dispatch && model.sync_delay_100ns) {
        model.inflight_sync_irp=irp;
        model.inflight_sync_status=st;
        return STATUS_PENDING;
    }
    if(model.sync_delay_100ns)
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.sync_delay_100ns;
    irp->IoStatus.Status=st;irp->IoStatus.Information=NT_SUCCESS(st)?model.bytes:0;
    complete_irp(irp);
    return model.io;
}
static NTSTATUS NTAPI RegPnp(IO_NOTIFICATION_EVENT_CATEGORY cat,ULONG flags,PVOID data,PDRIVER_OBJECT drv,
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE cb,PVOID ctx,PVOID *entry)
{
    CHECK(model.irql==PASSIVE_LEVEL && cat==EventCategoryTargetDeviceChange && !flags && data==&fake_file && drv==&fake_driver && cb && entry);
    ++model.pnp_regs;model.pnp_cb=cb;model.pnp_ctx=ctx;*entry=(PVOID)0x777;
    if(model.pnp_reg_delay_100ns)
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.pnp_reg_delay_100ns;
    if(model.query_remove_during_register_once){
        model.query_remove_during_register_once=FALSE;
        TARGET_DEVICE_REMOVAL_NOTIFICATION n={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
        CHECK(cb(&n,ctx)==STATUS_SUCCESS);
    }
    if(model.remove_during_register){
        TARGET_DEVICE_REMOVAL_NOTIFICATION n={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
        CHECK(cb(&n,ctx)==STATUS_SUCCESS);
        n.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
        CHECK(cb(&n,ctx)==STATUS_SUCCESS);
    }
    return STATUS_SUCCESS;
}
static VOID run_unlock_hook(PKSPIN_LOCK l)
{
    UNREFERENCED_PARAMETER(l);
#ifndef TG_PLATFORM_FIXTURE_ONLY
    if(!model.unlock_hook_mode || !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL))
        return;
    UINT mode=model.unlock_hook_mode;
    model.unlock_hook_mode=0;
    ++model.unlock_hook_arm_after;
    if(mode==1) {
        PIRP late_c=model.pending_claim_irp;
        CHECK(late_c!=NULL);
        model.pending_claim_irp=NULL;
        (VOID)CallDriver(&fake_target_device,late_c);
    } else if(mode==2) {
        for(UINT i=0;i<TgGpuApertureCount;++i){
            if(bases[i]==0x60006280u)registers[i][3]=0;
            if(bases[i]==0x7000e6d4u)registers[i][0]=0;
        }
        TgGpuPlatformRefreshIsolation(&platform,&resources);
    }
#endif
}
static NTSTATUS NTAPI UnregPnp(PVOID entry)
{
    CHECK(model.irql==PASSIVE_LEVEL && entry==(PVOID)0x777);
    ++model.pnp_unregs;model.pnp_cb=NULL;
    if(model.pnp_unreg_delay_100ns)
        *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=model.pnp_unreg_delay_100ns;
    return STATUS_SUCCESS;
}
static VOID NTAPI Stall(ULONG us){CHECK(us==999);++model.stalls;}
static NTSTATUS NTAPI Sleep(KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER interval)
{
    CHECK(mode==KernelMode && !alertable &&
          interval->QuadPart<=-10000 && interval->QuadPart>=-500000);
    *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=(ULONGLONG)(-interval->QuadPart);
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
PIO_WORKITEM (NTAPI * const __imp_IoAllocateWorkItem)(PDEVICE_OBJECT)=AllocWorkItem;
VOID (NTAPI * const __imp_IoFreeWorkItem)(PIO_WORKITEM)=FreeWorkItem;
VOID (NTAPI * const __imp_IoQueueWorkItem)(PIO_WORKITEM,PIO_WORKITEM_ROUTINE,WORK_QUEUE_TYPE,PVOID)=QueueWorkItem;
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
    CHECK(!model.live_pool && !model.live_irps && !model.live_workitems && !model.file_refs && !model.owner_refs);
    memset(&platform,0,sizeof(platform));memset(&model,0,sizeof(model));
    model.bytes=sizeof(struct tg_gpu_pmic_request);model.address=0x33;model.temp=42;
    model.enable=0x55;model.voltage=80;
}
#ifndef TG_PLATFORM_FIXTURE_ONLY
static void TestSwBattControl(void)
{
    DECLARE_CONST_UNICODE_STRING(swbatt,L"\\Driver\\SwBatt");
    DECLARE_CONST_UNICODE_STRING(other,L"\\Driver\\SwBattTest");
    DRIVER_OBJECT provider={0};
    UCHAR address=0;

    Setup();
    fake_target_device.DriverObject=&provider;
    provider.DriverName=swbatt;
    model.swbatt_wire=TRUE;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS && !model.pnp_regs);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && address==0x33);
    CHECK(model.tagged_queries==1 && model.plain_queries==1 && model.claims==1 && !model.last_claim_address);
    CHECK(!platform.RailClaimed && !platform.EmcRailOwner && !model.raw_writes);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS && !model.pnp_unregs);

    /* Genuine read/write denials and an unknown temperature remain fatal. */
    Setup();model.swbatt_wire=TRUE;model.completion=STATUS_ACCESS_DENIED;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_ACCESS_DENIED);
    CHECK(model.tagged_queries==1 && model.plain_queries==1 && !model.claims && !model.raw_writes);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    Setup();model.swbatt_wire=TRUE;model.claim=STATUS_ACCESS_DENIED;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_ACCESS_DENIED && model.claims==1);
    CHECK(!platform.RailClaimed && !model.raw_writes);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    Setup();model.swbatt_wire=TRUE;model.temp=255;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_NOT_READY && !model.claims);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);

    /* No fallback for a different provider. Reopen the named endpoint as
     * SwBatt on the deadline worker and check it gets its own wire protocol. */
    Setup();model.swbatt_wire=TRUE;provider.DriverName=other;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS && model.pnp_regs==1);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_ACCESS_DENIED && !model.plain_queries);
    TARGET_DEVICE_REMOVAL_NOTIFICATION rem={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
    PDRIVER_NOTIFICATION_CALLBACK_ROUTINE notify=model.pnp_cb;
    PVOID context=model.pnp_ctx;
    CHECK(notify(&rem,context)==STATUS_SUCCESS);
    rem.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
    CHECK(notify(&rem,context)==STATUS_SUCCESS && !platform.FileObject);
    provider.DriverName=swbatt;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.pnp_regs==1);
    CHECK(model.plain_queries==1 && !platform.RailClaimed && !model.raw_writes);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS && model.pnp_unregs==1);
    fake_target_device.DriverObject=NULL;
}

static void TestSwBattVersioned(void)
{
    DECLARE_CONST_UNICODE_STRING(name,L"\\Driver\\swbatt");
    DRIVER_OBJECT provider={.DriverName=name};
    UCHAR address=0;
    Setup();
    fake_target_device.DriverObject=&provider;
    model.swbatt_wire=TRUE;model.token_aware_provider=TRUE;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS && !model.pnp_regs);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && address==0x33);
    CHECK(model.tagged_queries==1 && !model.plain_queries && model.last_claim_address);
    CHECK(platform.RailClaimed && platform.EmcRailOwner && model.provider_claimed && model.provider_claim_token);

    model.token_aware_provider=FALSE;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_INVALID_DEVICE_STATE);
    CHECK(model.claims==1 && platform.RailClaimed && !model.raw_writes);
    model.token_aware_provider=TRUE;
    struct tg_gpu_power_io io=TgGpuPlatformIo(&platform);
    CHECK(io.release_pmic(io.context,0x33,0x54,0x46) && !platform.RailClaimed && model.releases==1);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    fake_target_device.DriverObject=NULL;
}
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
    TestSwBattControl();
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
    CHECK(model.requests==before_requests && model.live_pool==1 && !model.live_irps);
    model.completion_registration=STATUS_SUCCESS;

    /* Delayed completion after timeout + TgGpuClosePlatform: OwnerDevice stays
     * pinned (owner_refs > 0) until platform_irp_complete returns! */
    platform.IoStatus=STATUS_SUCCESS;model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT && model.cancels==1);
    CHECK(model.live_pool==2 && model.live_irps==1 && model.file_refs==2 && model.owner_refs==2);
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
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT &&
              platform.IoStatus==STATUS_IO_TIMEOUT &&
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

        /* Shared monotonic 600 ms budget across QUERY_ADDRESS + QUERY_TEMP:
         * 7 address retries + 10 temp retries must exhaust the 600 ms budget at
         * 12 sleeps and return STATUS_IO_TIMEOUT (M4/A04), never falling back to 1 s! */
        sleeps_before=model.sleeps;
        ULONG waits_before=model.waits;
        model.addr_not_ready_polls=7;
        model.temp_not_ready_polls=10;
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT &&
              platform.IoStatus==STATUS_IO_TIMEOUT &&
              model.sleeps==sleeps_before+12);
        CHECK(model.waits==waits_before+13);
        model.addr_not_ready_polls=0;
        model.temp_not_ready_polls=0;
        platform.IoStatus=STATUS_SUCCESS;

        /* A04 / T6a: synchronous 700 ms delay inside CLAIM_RAIL must return
         * STATUS_IO_TIMEOUT while truthfully recording RailClaimed for AbandonRail! */
        for(UINT i=0;i<TgGpuApertureCount;++i){
            if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
            if(bases[i]==0x7000e6d4u)registers[i][0]=1;
        }
        model.enable=0x54;model.voltage=0x46;
        platform.RailClaimed=FALSE;platform.RailOwnershipUncertain=FALSE;
        model.sync_delay_100ns=7000000ULL; /* 700 ms per sync IRP */
        /* Advance past QUERY_ADDRESS/QUERY_TEMP without delay, delay only on CLAIM: */
        model.sync_delay_100ns=0;
        {
            ULONGLONG base_t=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime;
            struct tg_gpu_pmic_request claim_req={0,TG_GPU_PMIC_CLAIM_RAIL,1,TG_GPU_EMC_VOTE_ABI};
            CHECK(TgGpuPlatformTryRequest(&platform,&claim_req)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformAbandonRail(&platform,0x33)==STATUS_SUCCESS);
            *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime=base_t;
        }
        /* Inject 250 ms per sync IRP (3 IRPs = 750 ms provider time > 600 ms budget):
         * A04 v6: Preflight MUST return to caller at exact 600 ms (<= 600 ms budget!)
         * with Uncertain=TRUE, while provider finishes on worker at 750 ms and
         * reconciles RailClaimed=TRUE so AbandonRail(0) releases 0x33! */
        {
            ULONGLONG t0=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime;
            model.sync_delay_100ns=2500000ULL;
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT &&
                  platform.IoStatus==STATUS_IO_TIMEOUT &&
                  platform.EmcRailOwner && platform.RailOwnershipUncertain);
            ULONGLONG caller_elapsed=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-t0;
            CHECK(caller_elapsed==6000000ULL);
            finish_inflight_sync_irp();
            ULONGLONG provider_elapsed=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-t0;
            CHECK(provider_elapsed==7500000ULL && platform.RailClaimed && !platform.RailOwnershipUncertain);
            model.sync_delay_100ns=0;
        }
        platform.IoStatus=STATUS_SUCCESS;
        CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS && !platform.RailClaimed);
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed);

        /* A04 v5 counterexample (T6): pending CLAIM_RAIL completes right as
         * wait advances clock to 700 ms (both STATUS_TIMEOUT and STATUS_SUCCESS
         * wake orderings): Preflight MUST report STATUS_IO_TIMEOUT while
         * truthfully reconciling RailClaimed so AbandonRail(0) releases 0x33! */
        CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_SUCCESS && !platform.RailClaimed);
        for(UINT ord=0;ord<2;++ord) {
            UCHAR out_addr=0x77;
            platform.IoStatus=STATUS_SUCCESS;
            model.defer_complete=FALSE;
            model.complete_in_wait=FALSE;
            model.wait_delay_100ns=0;
            /* Defer only on CLAIM_RAIL by running Preflight with wait_delay on 3rd wait: */
            ULONGLONG start_t=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime;
            model.defer_complete=TRUE;
            model.complete_in_wait=TRUE;
            model.wait=(ord==0)?STATUS_TIMEOUT:STATUS_SUCCESS;
            /* First 2 waits (ADDRESS, TEMP) take 250 ms each (500 ms < 600 ms);
             * 3rd wait (CLAIM_RAIL) is clamped to 100 ms remaining budget, and
             * wait_delay advances 250 ms -> 750 ms >= 600 ms! */
            model.wait_delay_100ns=2500000ULL;
            CHECK(TgGpuPlatformPreflight(&platform,&out_addr)==STATUS_IO_TIMEOUT &&
                  out_addr==0x77 && platform.IoStatus==STATUS_IO_TIMEOUT &&
                  platform.RailClaimed && !platform.RailOwnershipUncertain);
            ULONGLONG elapsed=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-start_t;
            CHECK(elapsed==7500000ULL);
            model.defer_complete=FALSE;model.complete_in_wait=FALSE;
            model.wait_delay_100ns=0;model.wait=STATUS_SUCCESS;
            platform.IoStatus=STATUS_SUCCESS;
            ULONG rel_b=model.releases;
            CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS &&
                  model.releases==rel_b+1 && !platform.RailClaimed);
        }
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed);

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
    ULONG rel_before_busy=model.releases;
    CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_DEVICE_BUSY);
    CHECK(platform.RailClaimed && platform.RailOwnershipUncertain && model.releases==rel_before_busy);
    for(UINT i=0;i<TgGpuApertureCount;++i){
        if(bases[i]==0x60006280u)registers[i][3]=UINT32_MAX;
        if(bases[i]==0x7000e6d4u)registers[i][0]=1;
    }
    CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_DEVICE_BUSY);
    CHECK(platform.RailClaimed && platform.RailOwnershipUncertain && model.releases==rel_before_busy);
    for(UINT i=0;i<TgGpuApertureCount;++i)
        if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
    /* Early CLAIM_RAIL unwind only after reset + clamp read back. */
    CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_SUCCESS);
    CHECK(!platform.RailClaimed && !platform.RailOwnershipUncertain && model.releases==rel_before_busy+1);

    /* Timed-out CLAIM_RAIL sets RailOwnershipUncertain so AbandonRail still
     * hands the rail back, and a late same-epoch completion reconciles state. */
    platform.EmcRailOwner=FALSE;platform.RailClaimed=FALSE;platform.RailOwnershipUncertain=FALSE;
    platform.IoStatus=STATUS_SUCCESS;
    {
        struct tg_gpu_pmic_request claim={0,TG_GPU_PMIC_CLAIM_RAIL,1,TG_GPU_EMC_VOTE_ABI};
        model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
        CHECK(TgGpuPlatformTryRequest(&platform,&claim)==STATUS_IO_TIMEOUT);
        CHECK(platform.EmcRailOwner && !platform.RailClaimed && platform.RailOwnershipUncertain);
        PIRP late_claim=model.deferred_irp;
        model.defer_complete=FALSE;model.wait=STATUS_SUCCESS;
        late_claim->IoStatus.Status=STATUS_SUCCESS;
        late_claim->IoStatus.Information=sizeof(claim);
        complete_irp(late_claim);
        CHECK(platform.EmcRailOwner && platform.RailClaimed && !platform.RailOwnershipUncertain);

        /* Timed-out RELEASE_RAIL leaves RailOwnershipUncertain until late completion. */
        struct tg_gpu_pmic_request rel={0x33,TG_GPU_PMIC_RELEASE_RAIL,1,0x5446u};
        model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
        CHECK(TgGpuPlatformTryRequest(&platform,&rel)==STATUS_IO_TIMEOUT);
        CHECK(platform.RailOwnershipUncertain);
        PIRP late_rel=model.deferred_irp;
        model.defer_complete=FALSE;model.wait=STATUS_SUCCESS;
        late_rel->IoStatus.Status=STATUS_SUCCESS;
        late_rel->IoStatus.Information=sizeof(rel);
        complete_irp(late_rel);
        CHECK(!platform.RailClaimed && !platform.RailOwnershipUncertain);

        /* Ordinary pending CLAIM must finish before a compensating RELEASE.
         * A zero caller address still falls back to verified LastPmicAddress. */
        model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
        CHECK(TgGpuPlatformTryRequest(&platform,&claim)==STATUS_IO_TIMEOUT);
        CHECK(platform.RailOwnershipUncertain);
        PIRP old_claim=model.deferred_irp;
        model.defer_complete=FALSE;model.wait=STATUS_SUCCESS;
        ULONG rel_before=model.releases;
        CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT);
        CHECK(model.releases==rel_before && platform.RailOwnershipUncertain);
        /* The late terminal receipt now schedules the single RELEASE. */
        old_claim->IoStatus.Status=STATUS_SUCCESS;
        old_claim->IoStatus.Information=sizeof(claim);
        complete_irp(old_claim);
        CHECK(model.releases==rel_before+1 && !platform.RailClaimed && !platform.RailOwnershipUncertain);

        /* V4-01 / T6b regression: Close -> Open -> Preflight on the SAME
         * TG_GPU_PLATFORM (without zeroing platform!) must sync BridgeEpoch
         * and succeed on the first QUERY_ADDRESS! */
        CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
        CHECK(platform.BridgeEpoch>0 && !platform.RailOwnershipUncertain);
        ULONG reqs_before_reopen=model.requests;
        address=0;
        CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS &&
              address==0x33 && model.requests==reqs_before_reopen+3);
        CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_SUCCESS);

        /* A05 / T7 regression: timed-out CLAIM_RAIL + failed RELEASE_RAIL returns
         * error, preserves RailOwnershipUncertain across Close + late CLAIM
         * completion, and allows retry AbandonRail(0) after reopen! */
        platform.IoStatus=STATUS_SUCCESS;
        model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
        CHECK(TgGpuPlatformTryRequest(&platform,&claim)==STATUS_IO_TIMEOUT);
        PIRP pending_claim=model.deferred_irp;
        model.defer_complete=FALSE;model.wait=STATUS_SUCCESS;
        model.release=STATUS_IO_DEVICE_ERROR;
        CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT);
        CHECK(platform.EmcRailOwner && platform.RailOwnershipUncertain && platform.LastPmicAddress==0x33);
        CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
        /* Unresolved ownership MUST survive Close and late CLAIM completion! */
        CHECK(platform.EmcRailOwner && platform.RailOwnershipUncertain && platform.LastPmicAddress==0x33);
        pending_claim->IoStatus.Status=STATUS_SUCCESS;
        pending_claim->IoStatus.Information=sizeof(claim);
        complete_irp(pending_claim);
        CHECK(platform.EmcRailOwner && platform.RailOwnershipUncertain && platform.LastPmicAddress==0x33);
        /* Reopen same adapter and complete the deferred rail release! */
        model.release=STATUS_SUCCESS;
        CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
        CHECK(platform.EmcRailOwner && platform.RailOwnershipUncertain);
        rel_before=model.releases;
        CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS);
        CHECK(model.releases==rel_before+1 && !platform.RailClaimed && !platform.RailOwnershipUncertain);

        /* A05 v5 counterexample 1 (T6): same-sequence RELEASE_RAIL succeeds
         * inside IoCancelIrp after wait timeout -> outer layers (TryRequest and
         * AbandonRail) MUST NOT overwrite the confirmed receipt back to uncertain=1! */
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed);
        model.defer_complete=TRUE;model.complete_in_cancel=TRUE;model.wait=STATUS_TIMEOUT;
        {
            struct tg_gpu_pmic_request rel_cancel={0x33,TG_GPU_PMIC_RELEASE_RAIL,1,0x5446u};
            CHECK(TgGpuPlatformTryRequest(&platform,&rel_cancel)==STATUS_IO_TIMEOUT);
            CHECK(!platform.RailClaimed && !platform.RailOwnershipUncertain);
        }
        model.defer_complete=FALSE;model.complete_in_cancel=FALSE;model.wait=STATUS_SUCCESS;
        CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed);
        /* Also test via AbandonRail when the RELEASE_RAIL IRP times out while 0x06/0x23 succeed: */
        {
            /* Issue CLAIM, then test AbandonRail failing -> Close TWICE -> Open -> AbandonRail(0) (A05 v5 #2)! */
            platform.IoStatus=STATUS_SUCCESS;
            model.release=STATUS_IO_DEVICE_ERROR;
            ULONG prev_fail_cnt=platform.AbandonFailureCount;
            CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_DEVICE_ERROR);
            CHECK(platform.AbandonFailureCount==prev_fail_cnt+1 &&
                  platform.LastAbandonStatus==STATUS_IO_DEVICE_ERROR &&
                  platform.EmcRailOwner && platform.RailOwnershipUncertain && platform.LastPmicAddress==0x33);
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            /* Repeated Close MUST be idempotent and preserve 0x33 + uncertain! */
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            CHECK(platform.EmcRailOwner && platform.RailOwnershipUncertain && platform.LastPmicAddress==0x33);
            BOOLEAN u_iso=FALSE;UCHAR u_addr=0;
            CHECK(TgGpuQueryUnresolvedRail(&u_iso,&u_addr,NULL,NULL) && u_iso && u_addr==0x33);
            model.release=STATUS_SUCCESS;
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(platform.EmcRailOwner && platform.RailOwnershipUncertain && platform.LastPmicAddress==0x33);
            rel_before=model.releases;
            CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS);
            CHECK(model.releases==rel_before+1 && !platform.RailClaimed &&
                  !platform.RailOwnershipUncertain && !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
        }

        /* N1 + A05 cross-RemoveDevice ledger regression: late SUCCESS completion
         * arriving AFTER TgGpuClosePlatform and adapter memory poison/free must
         * NEVER touch TG_GPU_PLATFORM bytes (N1), while driver-lifetime ledger
         * retains 0x33 and TgGpuRetryUnresolvedRailRelease returns the rail! */
        model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
        CHECK(TgGpuPlatformTryRequest(&platform,&claim)==STATUS_IO_TIMEOUT);
        PIRP after_close_claim=model.deferred_irp;
        model.defer_complete=FALSE;model.wait=STATUS_SUCCESS;
        CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
        memset(&platform,0xCC,sizeof(platform));
        after_close_claim->IoStatus.Status=STATUS_SUCCESS;
        after_close_claim->IoStatus.Information=sizeof(claim);
        complete_irp(after_close_claim);
        for(SIZE_T b=0;b<sizeof(platform);++b)CHECK(((UCHAR *)&platform)[b]==0xCC);
        {
            BOOLEAN u_iso=FALSE;UCHAR u_addr=0;ULONG u_retries=0;NTSTATUS u_last=STATUS_SUCCESS;
            CHECK(TgGpuQueryUnresolvedRail(&u_iso,&u_addr,&u_retries,&u_last) && u_iso && u_addr==0x33);
            model.release=STATUS_IO_DEVICE_ERROR;
            CHECK(TgGpuRetryUnresolvedRailRelease()==STATUS_IO_DEVICE_ERROR);
            CHECK(TgGpuQueryUnresolvedRail(&u_iso,&u_addr,&u_retries,&u_last) &&
                  u_iso && u_addr==0x33 && u_retries>=1 && u_last==STATUS_IO_DEVICE_ERROR);
            /* A05 v6 #3 (unload): final unload retry failure followed by
             * TgGpuSetPlatformDriverObject(NULL) MUST preserve the unresolved
             * ledger (Active, Isolated, Address, RetryCount, LastStatus), and
             * reattaching DriverObject can still release the rail! */
            TgGpuSetPlatformDriverObject(NULL);
            CHECK(TgGpuQueryUnresolvedRail(&u_iso,&u_addr,&u_retries,&u_last) &&
                  u_iso && u_addr==0x33 && u_retries>=1 && u_last==STATUS_IO_DEVICE_ERROR);
            TgGpuSetPlatformDriverObject(&fake_driver);
            model.release=STATUS_SUCCESS;
            rel_before=model.releases;
            CHECK(TgGpuRetryUnresolvedRailRelease()==STATUS_SUCCESS);
            CHECK(model.releases==rel_before+1 && !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
        }
        memset(&platform,0,sizeof(platform));

        /* A05 v6 #1 (cross_epoch): Instance A's RELEASE_RAIL executes at provider
         * with delayed completion receipt; Instance B reopens, CLAIMs, fails RELEASE,
         * Closes, and Retry also fails; delivering A's delayed RELEASE receipt MUST NOT
         * wipe B's unresolved claim from g_UnresolvedRail! */
        {
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed);
            struct tg_gpu_pmic_request rel_a={0x33,TG_GPU_PMIC_RELEASE_RAIL,1,0x5446u};
            model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
            CHECK(TgGpuPlatformTryRequest(&platform,&rel_a)==STATUS_IO_TIMEOUT);
            PIRP stale_rel_a=model.deferred_irp;
            model.deferred_irp=NULL;
            model.defer_complete=FALSE;model.wait=STATUS_SUCCESS;
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);

            /* Instance B reopens, claims a new epoch, then B's RELEASE & Retry fail: */
            memset(&platform,0,sizeof(platform));
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed);
            model.release=STATUS_IO_DEVICE_ERROR;
            CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_IO_DEVICE_ERROR);
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            CHECK(TgGpuRetryUnresolvedRailRelease()==STATUS_IO_DEVICE_ERROR);
            CHECK(TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));

            /* Deliver Instance A's delayed RELEASE_RAIL success receipt: */
            stale_rel_a->IoStatus.Status=STATUS_SUCCESS;
            stale_rel_a->IoStatus.Information=sizeof(rel_a);
            complete_irp(stale_rel_a);

            /* B's unresolved claim MUST remain active and retry MUST send 1 new RELEASE! */
            BOOLEAN b_iso=FALSE;UCHAR b_addr=0;
            CHECK(TgGpuQueryUnresolvedRail(&b_iso,&b_addr,NULL,NULL) && b_iso && b_addr==0x33);
            model.release=STATUS_SUCCESS;
            rel_before=model.releases;
            CHECK(TgGpuRetryUnresolvedRailRelease()==STATUS_SUCCESS);
            CHECK(model.releases==rel_before+1 && !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
            memset(&platform,0,sizeof(platform));
        }

        /* A04 v7 (pnp_deadline): After PnP QUERY_REMOVE + REMOVE_COMPLETE,
         * injecting an 800 ms ZwCreateFile MUST return STATUS_IO_TIMEOUT at
         * exact 600 ms with 0 IOCTLs, and if Close + 0xCC poison runs before
         * 800 ms, late worker completion touches 0 bytes of platform (N1)! */
        {
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && platform.RailClaimed);
            CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_SUCCESS);
            TARGET_DEVICE_REMOVAL_NOTIFICATION pnp_n={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
            PDRIVER_NOTIFICATION_CALLBACK_ROUTINE p_cb=model.pnp_cb;
            PVOID p_ctx=model.pnp_ctx;
            CHECK(p_cb(&pnp_n,p_ctx)==STATUS_SUCCESS);
            pnp_n.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
            CHECK(p_cb(&pnp_n,p_ctx)==STATUS_SUCCESS && !platform.Bridge && !platform.FileObject);

            ULONG reqs_b=model.requests;
            ULONGLONG t_pnp=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime;
            model.open_delay_100ns=8000000ULL; /* 800 ms ZwCreateFile */
            model.defer_workitem_once=TRUE;
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT &&
                  platform.IoStatus==STATUS_IO_TIMEOUT && model.requests==reqs_b);
            CHECK(*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-t_pnp==6000000ULL);
            /* Close and poison platform at 600 ms BEFORE the 800 ms open worker finishes! */
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            memset(&platform,0xCC,sizeof(platform));
            run_deferred_workitem();
            CHECK(*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-t_pnp==8000000ULL);
            for(SIZE_T b=0;b<sizeof(platform);++b)CHECK(((UCHAR *)&platform)[b]==0xCC);
            CHECK(model.requests==reqs_b && !model.live_pool && !model.live_workitems &&
                  !model.file_refs && !model.owner_refs);
            model.open_delay_100ns=0;
            memset(&platform,0,sizeof(platform));
        }

        /* V7-01 (1) queued worker + AbandonRail(0), and (2) slow deadline-bounded
         * CLAIM_RAIL + AbandonRail(0) -> provider_claimed == local_claimed == 0! */
        {
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.provider_claimed);
            model.defer_workitem_once=TRUE;
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT);
            CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS && !model.provider_claimed);
            run_deferred_workitem();
            CHECK(!model.provider_claimed && !platform.RailClaimed &&
                  !platform.RailOwnershipUncertain && !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));

            /* (2) Deadline-bounded Preflight times out on 3rd IRP (CLAIM_RAIL):
             * finish worker, run AbandonRail(0), and verify second AbandonRail(0)
             * sends 0 additional RELEASE with provider_claimed == local_claimed == 0! */
            platform.IoStatus=STATUS_SUCCESS;
            model.sync_delay_100ns=2500000ULL;
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT);
            model.sync_delay_100ns=0;
            finish_inflight_sync_irp();
            rel_before=model.releases;
            CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS);
            CHECK(model.releases==rel_before+1 && !model.provider_claimed &&
                  !platform.RailClaimed && !platform.RailOwnershipUncertain &&
                  !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
            CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS && model.releases==rel_before+1);
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            memset(&platform,0,sizeof(platform));
        }

        /* A05 v7 #2a (late_claim): old closed CLAIM_RAIL receipt arriving AFTER
         * a newer CLAIM_RAIL succeeded MUST NOT revive old unresolved ledger or
         * allow RetryUnresolvedRailRelease to drop the active generation's rail! */
        {
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            model.defer_complete=TRUE;model.wait=STATUS_TIMEOUT;
            CHECK(TgGpuPlatformTryRequest(&platform,&claim)==STATUS_IO_TIMEOUT);
            PIRP old_epoch_claim=model.deferred_irp;
            model.deferred_irp=NULL;
            model.defer_complete=FALSE;model.wait=STATUS_SUCCESS;
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            memset(&platform,0,sizeof(platform));

            /* New generation opens and succeeds CLAIM_RAIL: */
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS &&
                  platform.RailClaimed && model.provider_claimed);
            CHECK(!TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));

            /* Late completion of closed old_epoch_claim arrives now: */
            old_epoch_claim->IoStatus.Status=STATUS_SUCCESS;
            old_epoch_claim->IoStatus.Information=sizeof(claim);
            complete_irp(old_epoch_claim);

            /* Ledger MUST remain inactive and Retry MUST send 0 RELEASE_RAIL! */
            CHECK(!TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
            rel_before=model.releases;
            CHECK(TgGpuRetryUnresolvedRailRelease()==STATUS_SUCCESS);
            CHECK(model.releases==rel_before && platform.RailClaimed && model.provider_claimed);
            CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_SUCCESS && !model.provider_claimed);
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            memset(&platform,0,sizeof(platform));
        }

        /* A05 v7 #2b (retry_new_claim): TgGpuRetryUnresolvedRailRelease paused
         * between PMIC 0x06 and 0x23 while a new CLAIM_RAIL succeeds MUST abort
         * before sending RELEASE_RAIL! */
        {
            static TG_GPU_PLATFORM platform2;
            memset(&platform2,0,sizeof(platform2));
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.provider_claimed);
            model.release=STATUS_IO_DEVICE_ERROR;
            CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_IO_DEVICE_ERROR);
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            memset(&platform,0,sizeof(platform));
            CHECK(TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));

            model.release=STATUS_SUCCESS;
            model.concurrent_claim_platform=&platform2;
            model.claim_during_retry_read06=TRUE;
            rel_before=model.releases;
            CHECK(TgGpuRetryUnresolvedRailRelease()==STATUS_SUCCESS);
            model.concurrent_claim_platform=NULL;
            CHECK(!model.claim_during_retry_read06 && model.releases==rel_before &&
                  platform2.RailClaimed && model.provider_claimed &&
                  !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
            CHECK(TgGpuPlatformAbandonRail(&platform2,0x33)==STATUS_SUCCESS && !model.provider_claimed);
            CHECK(TgGpuClosePlatform(&platform2)==STATUS_SUCCESS);
            memset(&platform2,0,sizeof(platform2));
        }

        /* A05 v7 #3 (reload) + #1 (sticky 1->0 revocation across reload):
         * Switch to token-aware SwitchFan provider; Image 1 fails final unload
         * retry and detaches DriverObject; fresh Image 2 (independent static
         * storage Img2_TgGpu*) attaches to same provider, observes active=1,
         * re-verifies hardware isolation, and releases the rail! */
        {
            model.token_aware_provider=TRUE;
            CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
            CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.provider_claimed);
            model.release=STATUS_IO_DEVICE_ERROR;
            CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_IO_DEVICE_ERROR);
            CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
            CHECK(TgGpuRetryUnresolvedRailRelease()==STATUS_IO_DEVICE_ERROR);
            BOOLEAN same_iso=FALSE;UCHAR same_addr=0;ULONG same_retries=0;
            CHECK(TgGpuQueryUnresolvedRail(&same_iso,&same_addr,&same_retries,NULL) &&
                  same_iso && same_addr==0x33 && same_retries>=1 && model.provider_claimed);
            TgGpuSetPlatformDriverObject(NULL);
            memset(&platform,0,sizeof(platform));

            /* Attach fresh KMD Image 2 (zeroed independent static globals!): */
            Img2_TgGpuSetPlatformDriverObject(&fake_driver);
            BOOLEAN fresh_iso=FALSE;UCHAR fresh_addr=0;
            CHECK(Img2_TgGpuQueryUnresolvedRail(&fresh_iso,&fresh_addr,NULL,NULL) &&
                  fresh_iso && fresh_addr==0x33 && model.provider_claimed);

            /* Hardware reset/clamp drops to 0 before Image 2 refreshes isolation ->
             * sticky proof MUST be revoked (0 RELEASE_RAIL)! */
            for(UINT i=0;i<TgGpuApertureCount;++i){
                if(bases[i]==0x60006280u)registers[i][3]=0;
                if(bases[i]==0x7000e6d4u)registers[i][0]=0;
            }
            model.release=STATUS_SUCCESS;
            rel_before=model.releases;
            Img2_TgGpuPlatformRefreshIsolation(&platform,&resources);
            CHECK(Img2_TgGpuQueryUnresolvedRail(&fresh_iso,&fresh_addr,NULL,NULL) &&
                  !fresh_iso && fresh_addr==0x33);
            CHECK(Img2_TgGpuRetryUnresolvedRailRelease()==STATUS_DEVICE_NOT_READY &&
                  model.releases==rel_before && model.provider_claimed);

            /* Restore hardware isolation (reset=1<<24, clamp=1) -> Image 2
             * refreshes isolation and releases the rail cleanly! */
            for(UINT i=0;i<TgGpuApertureCount;++i){
                if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
                if(bases[i]==0x7000e6d4u)registers[i][0]=1;
            }
            Img2_TgGpuPlatformRefreshIsolation(&platform,&resources);
            CHECK(Img2_TgGpuQueryUnresolvedRail(&fresh_iso,&fresh_addr,NULL,NULL) && fresh_iso);
            CHECK(Img2_TgGpuRetryUnresolvedRailRelease()==STATUS_SUCCESS &&
                  model.releases==rel_before+1 && !model.provider_claimed &&
                  !Img2_TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
            Img2_TgGpuSetPlatformDriverObject(NULL);
            TgGpuSetPlatformDriverObject(&fake_driver);
            model.token_aware_provider=FALSE;
            memset(&platform,0,sizeof(platform));
        }

        /* v8 re-audit 6 counterexamples (V7-01 pending/pending-prior, A05 revoke,
         * and A04 deadline_handoff / deadline_pool / deadline_wi): */
        {
            model.token_aware_provider=TRUE;

            /* 1 & 2. V7-01 pending & pending-prior (both late SUCCESS and late CANCELLED): */
            for(UINT prior=0;prior<2;++prior) {
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                if(prior)
                    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.provider_claimed);
                ULONGLONG t_pend=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime;
                ULONG rel_b=model.releases;
                model.pending_claim_once=TRUE;
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT);
                CHECK(*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-t_pend==6000000ULL);
                CHECK(model.pending_claim_irp!=NULL);
                /* AbandonRail(0) MUST defer RELEASE_RAIL while pending_claim_irp is unfinished! */
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT);
                CHECK(model.releases==rel_b && platform.RailOwnershipUncertain &&
                      TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                PIRP late_irp=model.pending_claim_irp;
                model.pending_claim_irp=NULL;
                (VOID)CallDriver(&fake_target_device,late_irp);
                CHECK(model.releases==rel_b+1 && !model.provider_claimed &&
                      !platform.RailClaimed && !platform.RailOwnershipUncertain &&
                      !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS && model.releases==rel_b+1);
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* Also verify pending-prior when the second CLAIM_RAIL completes STATUS_CANCELLED:
             * prior claim MUST still be released by deferred compensation! */
            {
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.provider_claimed);
                ULONG rel_b=model.releases;
                model.pending_claim_once=TRUE;
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT && model.pending_claim_irp);
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT && model.releases==rel_b);
                PIRP canc_irp=model.pending_claim_irp;
                model.pending_claim_irp=NULL;
                canc_irp->IoStatus.Status=STATUS_CANCELLED;
                canc_irp->IoStatus.Information=0;
                complete_irp(canc_irp);
                CHECK(model.releases==rel_b+1 && !model.provider_claimed &&
                      !platform.RailClaimed && !platform.RailOwnershipUncertain &&
                      !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* 3. A05 revoke: DeferReleasePending armed while isolated=1, then
             * CAR_RESET_X / PMC_GPU_CLAMP drop to 0 + RefreshIsolation BEFORE late
             * CLAIM_RAIL completes -> 0 RELEASE_RAIL sent; re-isolating afterward
             * allows AbandonRail(0) to send 1 RELEASE_RAIL! */
            {
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                ULONG rel_b=model.releases;
                model.pending_claim_once=TRUE;
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT && model.pending_claim_irp);
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT && model.releases==rel_b);
                for(UINT i=0;i<TgGpuApertureCount;++i){
                    if(bases[i]==0x60006280u)registers[i][3]=0;
                    if(bases[i]==0x7000e6d4u)registers[i][0]=0;
                }
                TgGpuPlatformRefreshIsolation(&platform,&resources);
                BOOLEAN rev_iso=TRUE;
                CHECK(TgGpuQueryUnresolvedRail(&rev_iso,NULL,NULL,NULL) &&
                      !rev_iso && !platform.PreservedGpuIsolated);
                PIRP late_c=model.pending_claim_irp;
                model.pending_claim_irp=NULL;
                (VOID)CallDriver(&fake_target_device,late_c);
                /* Late CLAIM receipt MUST NOT resurrect isolated=1 or send RELEASE_RAIL! */
                CHECK(model.releases==rel_b && model.provider_claimed &&
                      !platform.PreservedGpuIsolated && platform.RailOwnershipUncertain &&
                      TgGpuQueryUnresolvedRail(&rev_iso,NULL,NULL,NULL) && !rev_iso);
                /* Restore hardware isolation -> AbandonRail(0) now releases cleanly! */
                for(UINT i=0;i<TgGpuApertureCount;++i){
                    if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
                    if(bases[i]==0x7000e6d4u)registers[i][0]=1;
                }
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS);
                CHECK(model.releases==rel_b+1 && !model.provider_claimed &&
                      !platform.RailClaimed && !platform.RailOwnershipUncertain &&
                      !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* 4, 5, 6. A04 deadline_handoff (600 ms register + 800 ms new unregister),
             * deadline_pool (OOM on octx + 800 ms stale unregister), and deadline_wi
             * (OOM on work item + 800 ms stale unregister): caller elapsed MUST stay <= 600 ms! */
            for(UINT dcase=0;dcase<3;++dcase) {
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                TARGET_DEVICE_REMOVAL_NOTIFICATION pnp_d={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
                PDRIVER_NOTIFICATION_CALLBACK_ROUTINE d_cb=model.pnp_cb;
                PVOID d_ctx=model.pnp_ctx;
                CHECK(d_cb(&pnp_d,d_ctx)==STATUS_SUCCESS);
                pnp_d.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
                CHECK(d_cb(&pnp_d,d_ctx)==STATUS_SUCCESS && !platform.Bridge && !platform.FileObject);
                ULONG req_b=model.requests;
                ULONG unreg_b=model.pnp_unregs;
                ULONGLONG t_dl=*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime;
                if(dcase==0) {
                    /* Stale unregister is fast (0 ms); new registration finishes at 600 ms: */
                    model.pnp_reg_delay_100ns=6000000ULL;
                    model.pnp_unreg_delay_100ns=0;
                    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT);
                    CHECK(*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-t_dl==6000000ULL);
                    CHECK(model.requests==req_b && model.pnp_unregs==unreg_b+1);
                } else {
                    model.pnp_unreg_delay_100ns=8000000ULL;
                    if(dcase==1)model.fail_pool_once=TRUE;
                    else model.fail_workitem_once=TRUE;
                    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_INSUFFICIENT_RESOURCES);
                    CHECK(*(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime-t_dl==0ULL);
                    CHECK(model.requests==req_b && model.pnp_unregs==unreg_b);
                }
                model.pnp_reg_delay_100ns=0;
                model.pnp_unreg_delay_100ns=0;
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* 7. GC-V9F-01b: PnP QUERY_REMOVE during reconnect registration rollback
             * must clear QueryRemove so the next reconnect succeeds cleanly! */
            {
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                TARGET_DEVICE_REMOVAL_NOTIFICATION pnp_q={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
                PDRIVER_NOTIFICATION_CALLBACK_ROUTINE q_cb=model.pnp_cb;
                PVOID q_ctx=model.pnp_ctx;
                CHECK(q_cb(&pnp_q,q_ctx)==STATUS_SUCCESS);
                pnp_q.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
                CHECK(q_cb(&pnp_q,q_ctx)==STATUS_SUCCESS && !platform.Bridge && !platform.FileObject);
                model.query_remove_during_register_once=TRUE;
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_NOT_READY);
                CHECK(!platform.QueryRemove && !platform.Bridge && !platform.FileObject);
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && address==0x33);
                CHECK(TgGpuPlatformAbandonRail(&platform,address)==STATUS_SUCCESS);
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* 8. GC-V9F-02: Zero double-queue of CompensateWorkItem when a newer CLAIM
             * queues compensation first and an older timed-out RELEASE completes before
             * the deferred worker starts executing! */
            {
                for(UINT i=0;i<TgGpuApertureCount;++i){
                    if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
                    if(bases[i]==0x7000e6d4u)registers[i][0]=1;
                }
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.provider_claimed);
                model.pending_release_once=TRUE;
                struct tg_gpu_pmic_request rel_p={0x33,TG_GPU_PMIC_RELEASE_RAIL,1,0x5446u};
                CHECK(TgGpuPlatformTryRequest(&platform,&rel_p)==STATUS_IO_TIMEOUT && model.pending_release_irp!=NULL);
                PIRP saved_rel=model.pending_release_irp;
                model.pending_release_irp=NULL;
                model.pending_claim_once=TRUE;
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT && model.pending_claim_irp!=NULL);
                ULONG rel_b=model.releases;
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT);
                model.defer_compensate_once=TRUE;
                PIRP saved_claim=model.pending_claim_irp;
                model.pending_claim_irp=NULL;
                (VOID)CallDriver(&fake_target_device,saved_claim);
                CHECK(model.deferred_comp_routine!=NULL);
                /* Complete older cancelled RELEASE while CompensateWorkItem is STILL queued:
                 * QueueWorkItem enforces CHECK(!workitem_queued[slot])! */
                saved_rel->IoStatus.Status=STATUS_CANCELLED;
                saved_rel->IoStatus.Information=0;
                complete_irp(saved_rel);
                run_deferred_compensate();
                CHECK(model.releases==rel_b+1 && !model.provider_claimed &&
                      !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* 9. GC-V9F-03a: Pending CLAIM completion immediately on the first state->Lock
             * unlock after DeferReleasePending arm must observe live isolation=TRUE and
             * release the rail cleanly without leaving stale unresolved accountability! */
            {
                for(UINT i=0;i<TgGpuApertureCount;++i){
                    if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
                    if(bases[i]==0x7000e6d4u)registers[i][0]=1;
                }
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                model.pending_claim_once=TRUE;
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT && model.pending_claim_irp!=NULL);
                ULONG rel_b=model.releases;
                model.unlock_hook_arm_after=0;
                model.unlock_hook_mode=1;
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT);
                CHECK(model.unlock_hook_arm_after==1 && model.releases==rel_b+1 &&
                      !model.provider_claimed && !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* 10. GC-V9F-03b: Hardware isolation revocation immediately on the first
             * state->Lock unlock after DeferReleasePending arm must never be overwritten
             * back to isolated=TRUE or trigger an unsafe RELEASE_RAIL! */
            {
                for(UINT i=0;i<TgGpuApertureCount;++i){
                    if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
                    if(bases[i]==0x7000e6d4u)registers[i][0]=1;
                }
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                model.pending_claim_once=TRUE;
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT && model.pending_claim_irp!=NULL);
                ULONG rel_b=model.releases;
                model.unlock_hook_arm_after=0;
                model.unlock_hook_mode=2;
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_IO_TIMEOUT);
                CHECK(model.unlock_hook_arm_after==1);
                PIRP late_c=model.pending_claim_irp;
                model.pending_claim_irp=NULL;
                (VOID)CallDriver(&fake_target_device,late_c);
                BOOLEAN rev_iso=TRUE;
                CHECK(model.releases==rel_b && model.provider_claimed &&
                      TgGpuQueryUnresolvedRail(&rev_iso,NULL,NULL,NULL) && !rev_iso);
                for(UINT i=0;i<TgGpuApertureCount;++i){
                    if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
                    if(bases[i]==0x7000e6d4u)registers[i][0]=1;
                }
                CHECK(TgGpuPlatformAbandonRail(&platform,0)==STATUS_SUCCESS);
                CHECK(model.releases==rel_b+1 && !model.provider_claimed &&
                      !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                memset(&platform,0,sizeof(platform));
            }

            /* 11. GC-V9F-04: Automatic reclaim pending across RemoveDevice + 0xCC poison
             * must release the rail cleanly on late completion without UAF! */
            {
                for(UINT i=0;i<TgGpuApertureCount;++i){
                    if(bases[i]==0x60006280u)registers[i][3]=1u<<24;
                    if(bases[i]==0x7000e6d4u)registers[i][0]=1;
                }
                CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
                CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.provider_claimed);
                TARGET_DEVICE_REMOVAL_NOTIFICATION pnp_r={.Event=GUID_TARGET_DEVICE_QUERY_REMOVE,.FileObject=&fake_file};
                PDRIVER_NOTIFICATION_CALLBACK_ROUTINE r_cb=model.pnp_cb;
                PVOID r_ctx=model.pnp_ctx;
                CHECK(r_cb(&pnp_r,r_ctx)==STATUS_SUCCESS);
                pnp_r.Event=GUID_TARGET_DEVICE_REMOVE_COMPLETE;
                CHECK(r_cb(&pnp_r,r_ctx)==STATUS_SUCCESS && platform.NeedReclaim);
                model.provider_claimed=FALSE;
                model.provider_claim_token=0;
                model.pending_claim_once=TRUE;
                struct tg_gpu_pmic_request rep={0,TG_GPU_PMIC_REPORT_ABI,1,TG_GPU_LIVE_COUNTER_ABI};
                CHECK(TgGpuPlatformTryRequest(&platform,&rep)==STATUS_IO_TIMEOUT && model.pending_claim_irp!=NULL);
                ULONG rel_b=model.releases;
                (VOID)TgGpuPlatformAbandonRail(&platform,0);
                CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
                TgGpuPlatformRefreshIsolation(&platform,&resources);
                TgGpuHandOffUnresolvedRail(&platform,platform.PreservedGpuIsolated);
                (VOID)TgGpuRetryUnresolvedRailRelease();
                memset(&platform,0xcc,sizeof(platform));
                PIRP late_reclaim=model.pending_claim_irp;
                model.pending_claim_irp=NULL;
                (VOID)CallDriver(&fake_target_device,late_reclaim);
                for(UINT b=0;b<sizeof(platform);++b)CHECK(((const UCHAR *)&platform)[b]==0xccu);
                CHECK(model.releases==rel_b+1 && !model.provider_claimed &&
                      !TgGpuQueryUnresolvedRail(NULL,NULL,NULL,NULL));
                memset(&platform,0,sizeof(platform));
            }

            model.token_aware_provider=FALSE;
        }
    }
    CHECK(!model.live_pool && !model.live_irps && !model.live_workitems && !model.file_refs && !model.owner_refs);
    TestSwBattVersioned();
    CHECK(!model.live_pool && !model.live_irps && !model.live_workitems && !model.file_refs && !model.owner_refs);
    CHECK(WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL));ExitProcess(0);
}
#endif
