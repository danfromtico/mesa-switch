/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include "../windows/dvfs.h"
#include "../windows/hardware.h"
#include "../windows/platform_io.h"
#include "../windows/work_queue.h"
#include "../platform/t214_resources.h"
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
    UCHAR rail;
    BOOLEAN counter_bad,fail_target_counter;
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
static NTSTATUS NTAPI Ioctl(HANDLE file,HANDLE event,PIO_APC_ROUTINE apc,PVOID ctx,
    PIO_STATUS_BLOCK iosb,ULONG code,PVOID in,ULONG in_bytes,PVOID out,ULONG out_bytes)
{
    struct tg_gpu_pmic_request *op=in;
    CHECK(file==(HANDLE)123 && !event && !apc && !ctx && code==TG_GPU_IOCTL_PMIC);
    CHECK(in==out && in_bytes==sizeof(*op) && out_bytes==sizeof(*op));
    if(op->reg==TG_GPU_PMIC_QUERY_ADDRESS)op->value=0x33;
    else if(op->reg==TG_GPU_PMIC_QUERY_TEMP)op->value=model.temp;
    else if(op->reg==TG_GPU_PMIC_QUERY_EMC_KHZ)op->value=model.emc_khz;
    else if(op->reg==TG_GPU_PMIC_QUERY_DVFS_POLICY)op->value=model.policy;
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_KHZ){CHECK(op->write==1);model.reported_khz=op->value;}
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_UV){CHECK(op->write==1);model.reported_uv=op->value;}
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_STATUS){CHECK(op->write==1);model.reported_status=op->value;}
    else {
        CHECK(op->address==0x33);
        if(op->reg==0x06){CHECK(!op->write);op->value=0x41;}
        else {
            CHECK(op->reg==0x23);
            if(op->write){model.rail=(UCHAR)op->value;++model.pmic_writes;}
            else op->value=model.rail;
        }
    }
    iosb->Status=STATUS_SUCCESS;iosb->Information=sizeof(*op);return STATUS_SUCCESS;
}
static NTSTATUS NTAPI WaitFile(HANDLE h,BOOLEAN alertable,PLARGE_INTEGER timeout)
{UNREFERENCED_PARAMETER(h);UNREFERENCED_PARAMETER(alertable);UNREFERENCED_PARAMETER(timeout);return STATUS_SUCCESS;}
static LARGE_INTEGER NTAPI QueryQpc(PLARGE_INTEGER freq)
{
    LARGE_INTEGER out;
    if(freq)freq->QuadPart=1000000LL; /* 1 tick = 1 us in controlled model */
    out.QuadPart=(LONGLONG)model.qpc_ticks;return out;
}
static NTSTATUS NTAPI WaitObject(PVOID obj,KWAIT_REASON reason,KPROCESSOR_MODE mode,
    BOOLEAN alertable,PLARGE_INTEGER timeout)
{
    CHECK(obj==&queue.DispatchGate && reason==Executive && mode==KernelMode && !alertable && !timeout);
    CHECK(model.level==PASSIVE_LEVEL);++model.gate_depth;return STATUS_SUCCESS;
}
static LONG NTAPI ReleaseMutex(PRKMUTEX m,BOOLEAN wait)
{CHECK(m==&queue.DispatchGate && !wait && model.gate_depth && model.level==PASSIVE_LEVEL);--model.gate_depth;return 0;}
static LONG NTAPI SetEvent(PRKEVENT e,KPRIORITY prio,BOOLEAN wait)
{CHECK(e==&queue.Wake && !prio && !wait);return 0;}
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
NTSTATUS (NTAPI * const __imp_ZwDeviceIoControlFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,
    ULONG,PVOID,ULONG,PVOID,ULONG)=Ioctl;
NTSTATUS (NTAPI * const __imp_ZwWaitForSingleObject)(HANDLE,BOOLEAN,PLARGE_INTEGER)=WaitFile;
LARGE_INTEGER (NTAPI * const __imp_KeQueryPerformanceCounter)(PLARGE_INTEGER)=QueryQpc;
NTSTATUS (NTAPI * const __imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,
    BOOLEAN,PLARGE_INTEGER)=WaitObject;
LONG (NTAPI * const __imp_KeReleaseMutex)(PRKMUTEX,BOOLEAN)=ReleaseMutex;
LONG (NTAPI * const __imp_KeSetEvent)(PRKEVENT,KPRIORITY,BOOLEAN)=SetEvent;
KIRQL (NTAPI * const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=LockSpin;
VOID (NTAPI * const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=UnlockSpin;
VOID (NTAPI * const __imp_KeStallExecutionProcessor)(ULONG)=Stall;
NTSTATUS (NTAPI * const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=DelayThread;

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
