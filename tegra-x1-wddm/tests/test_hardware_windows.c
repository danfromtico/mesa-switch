/* SPDX-License-Identifier: MIT */
#include "../windows/hardware.h"
#include "../windows/reset_backend.h"
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

/* These are explicit allocation/GPU substitutes, not a native GPU test.
 * Link hardware.obj/private_memory.obj only: no real power/firmware routines
 * or OS Dxgkrnl allocation functions are used by this executable.
 */
static _Alignas(PAGE_SIZE) UCHAR memory[3][TG_GPU_GR_CHANNEL_WORK_BYTES],commands[TG_GPU_COMMAND_BYTES];
static BOOLEAN used[3];
static TG_GPU_CONTEXT_ENGINE engine;
static TG_GPU_HARDWARE hardware;
enum {FailNone,FailPower,FailFrontend,FailClock,FailBoot,FailMap,FailTemplate};
static struct {
    UINT fail_stage,allocs,fail_after,reads,drains,clock_stops,downs,frees;
    HANDLE fail_free;
    BOOLEAN fail_drain,fail_down,fail_isolation,fail_clock_stop,clock_started,execute_stopped,mapped;
} model;
static KIRQL level;
static TG_GPU_WORK_QUEUE queue;
static TG_GPU_WORKER worker;
static BOOLEAN fail_join,fail_queue,fail_thread;
static UINT gate_depth;
static UINT reset_trace_records;
static BOOLEAN reset_trace_open;
static NTSTATUS reset_trace_status;
static UINT packet_trace_calls;
VOID TgGpuTraceResetWork(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *i,
    const struct tg_gpu_power *p)
{
    CHECK(q==&queue && i==&hardware.Io && p==&hardware.Power && level==PASSIVE_LEVEL &&
        gate_depth && !worker.Thread && q->Paused && engine.Resetting &&
        !q->Lock && !engine.Lock && reset_trace_open);
    ++packet_trace_calls;
}
VOID TgGpuTraceExecutionState(TG_GPU_CONTEXT_ENGINE *e,enum tg_gpu_execution_source source,
    NTSTATUS status,UINT fence,const DXGKARG_SUBMITCOMMANDVIRTUAL *submit)
{
    CHECK(e==&engine && level==PASSIVE_LEVEL && !engine.Lock && !queue.Lock && !fence && !submit);
    if(source==TgGpuExecutionResetBegin){CHECK(!reset_trace_open && status==STATUS_PENDING);reset_trace_open=TRUE;}
    else {CHECK(source==TgGpuExecutionResetEnd && reset_trace_open);reset_trace_open=FALSE;reset_trace_status=status;}
    ++reset_trace_records;
}
static NTSTATUS NTAPI Wait(PVOID object,KWAIT_REASON reason,KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout)
{CHECK(object==&queue.DispatchGate && reason==Executive && mode==KernelMode && !alertable && !timeout && level==PASSIVE_LEVEL);++gate_depth;return STATUS_SUCCESS;}
static LONG NTAPI Release(PRKMUTEX mutex,BOOLEAN wait)
{CHECK(mutex==&queue.DispatchGate && !wait && gate_depth && level==PASSIVE_LEVEL);--gate_depth;return 0;}
NTSTATUS (NTAPI * const __imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=Wait;
LONG (NTAPI * const __imp_KeReleaseMutex)(PRKMUTEX,BOOLEAN)=Release;
NTSTATUS TgGpuInitializeWorkQueue(TG_GPU_WORK_QUEUE *q,TG_GPU_CONTEXT_ENGINE *e)
{CHECK(q==&queue && e==&engine && !e->QueueOwner && !q->Engine && !gate_depth);memset(q,0,sizeof(*q));q->Engine=e;e->QueueOwner=q;return STATUS_SUCCESS;}
NTSTATUS TgGpuResetWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *i,struct tg_gpu_power *p,struct tg_gpu_clock *c)
{(void)i;(void)p;(void)c;CHECK(q==&queue && q->Paused && gate_depth && !worker.Thread);return fail_queue?STATUS_IO_TIMEOUT:STATUS_SUCCESS;}
NTSTATUS TgGpuStopWorkQueue(TG_GPU_WORK_QUEUE *q)
{CHECK(q==&queue && gate_depth);engine.QueueOwner=NULL;q->Engine=NULL;return STATUS_SUCCESS;}
NTSTATUS TgGpuStartWorker(TG_GPU_WORKER *w,TG_GPU_WORK_QUEUE *q,TG_GPU_HARDWARE *h)
{CHECK(w==&worker && q==&queue && h==&hardware && h->Ready && !w->Thread);if(engine.Resetting)CHECK(gate_depth && engine.RecoveryPending);if(fail_thread)return STATUS_NO_MEMORY;w->Thread=(HANDLE)1;w->Queue=q;w->Hardware=h;return STATUS_SUCCESS;}
NTSTATUS TgGpuStopWorker(TG_GPU_WORKER *w)
{CHECK(w==&worker && !gate_depth);if(fail_join)return STATUS_UNSUCCESSFUL;w->Thread=NULL;w->Queue=NULL;w->Hardware=NULL;return STATUS_SUCCESS;}
static KIRQL NTAPI Lock(PKSPIN_LOCK l){KIRQL old=level;CHECK(!*l);*l=1;level=DISPATCH_LEVEL;return old;}
static VOID NTAPI Unlock(PKSPIN_LOCK l,KIRQL old){CHECK(*l && level==DISPATCH_LEVEL);*l=0;level=old;}
KIRQL (NTAPI * const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=Lock;
VOID (NTAPI * const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=Unlock;
static PHYSICAL_ADDRESS NTAPI Physical(PVOID p)
{
    PHYSICAL_ADDRESS result={0};ULONG_PTR offset=(ULONG_PTR)p-(ULONG_PTR)memory;
    CHECK(offset<sizeof(memory));result.QuadPart=0xc2000000+(offset/sizeof(memory[0]))*0x1000000+offset%sizeof(memory[0]);return result;
}
PHYSICAL_ADDRESS (NTAPI * const __imp_MmGetPhysicalAddress)(PVOID)=Physical;
static NTSTATUS NTAPI Allocate(HANDLE device,DXGKARGCB_ALLOCATECONTIGUOUSMEMORY *a)
{
    UINT i;
    CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && a->CacheType==DXGK_MEMORY_CACHING_TYPE_NON_CACHED);
    CHECK(a->LowestAcceptableAddress.QuadPart==0x80000000 && a->HighestAcceptableAddress.QuadPart==0xffffffff);
    if(model.allocs++==model.fail_after)return STATUS_INSUFFICIENT_RESOURCES;
    i=a->NumberOfBytes==TG_GPU_ARENA_BYTES?0:a->NumberOfBytes==TG_GPU_GR_CHANNEL_WORK_BYTES?1:2;
    CHECK(i!=2 || a->NumberOfBytes==TG_GPU_GR_IMAGE_MAX_BYTES);CHECK(!used[i]);
    used[i]=TRUE;a->hMemoryHandle=(HANDLE)(ULONG_PTR)(i+1);a->pMemory=memory[i];return STATUS_SUCCESS;
}
static NTSTATUS NTAPI Free(HANDLE device,const DXGKARGCB_FREECONTIGUOUSMEMORY *a)
{
    UINT i=(UINT)(ULONG_PTR)a->hMemoryHandle-1;
    CHECK(device==(HANDLE)1 && level==PASSIVE_LEVEL && i<3 && used[i]);
    if(a->hMemoryHandle==model.fail_free)return STATUS_UNSUCCESSFUL;
    CHECK(!hardware.GpuOwned || hardware.Isolated);used[i]=FALSE;++model.frees;return STATUS_SUCCESS;
}
static UINT32 Read(void *p,UINT32 address)
{
    CHECK(p==&model && level==PASSIVE_LEVEL);++model.reads;
    if(address==0x7000e6d4)return model.fail_isolation?0:1;
    CHECK(address==0x6000628c);return model.fail_isolation?0:0x01000000;
}
static void Write(void *p,UINT32 a,UINT32 v){(void)p;(void)a;(void)v;CHECK(FALSE);}
static void Delay(void *p,UINT32 us){(void)p;(void)us;CHECK(FALSE);}
static bool ReadPmic(void *p,UINT8 a,UINT8 r,UINT8 *v){(void)p;(void)a;(void)r;(void)v;CHECK(FALSE);return false;}
static bool WritePmic(void *p,UINT8 a,UINT8 r,UINT8 v){(void)p;(void)a;(void)r;(void)v;CHECK(FALSE);return false;}
static struct tg_gpu_power_io io={&model,Read,Write,ReadPmic,WritePmic,Delay, NULL};
enum tg_gpu_power_result tg_gpu_power_up(const struct tg_gpu_power_io *i,uint8_t address,struct tg_gpu_power *p)
{
    CHECK(i && i->context==&model && address==0x33 && level==PASSIVE_LEVEL);
    p->owned=true;p->ready=model.fail_stage!=FailPower;p->stage=TG_GPU_STAGE_READY;
    return p->ready?TG_GPU_POWER_OK:TG_GPU_POWER_PMIC_ERROR;
}
enum tg_gpu_power_result tg_gpu_power_drain(const struct tg_gpu_power_io *i,const struct tg_gpu_power *p)
{
    CHECK(i->context==&model && p->owned && p->ready && level==PASSIVE_LEVEL);
    CHECK(!hardware.Execution.owned || model.execute_stopped);++model.drains;
    return model.fail_drain?TG_GPU_POWER_MC_TIMEOUT:TG_GPU_POWER_OK;
}
enum tg_gpu_power_result tg_gpu_power_down(const struct tg_gpu_power_io *i,struct tg_gpu_power *p)
{
    CHECK(i->context==&model && level==PASSIVE_LEVEL);if(!p->owned)return TG_GPU_POWER_OK;
    CHECK(!model.clock_started || model.clock_stops);++model.downs;
    if(model.fail_down)return TG_GPU_POWER_RESET_ERROR;
    p->owned=p->ready=false;return TG_GPU_POWER_OK;
}
enum tg_gpu_frontend_result tg_gpu_frontend_start(const struct tg_gpu_power_io *i,
    const struct tg_gpu_power *p,struct tg_gpu_frontend_status *s)
{
    CHECK(i->context==&model && p->ready && level==PASSIVE_LEVEL);s->stage=1;
    return model.fail_stage==FailFrontend?TG_GPU_FE_PRI_FAULT:TG_GPU_FE_OK;
}
enum tg_gpu_clock_result tg_gpu_clock_start(const struct tg_gpu_power_io *i,
    const struct tg_gpu_power *p,uint32_t khz,struct tg_gpu_clock *c)
{
    CHECK(i->context==&model && p->ready && khz==TG_GPU_RENDER_KHZ && level==PASSIVE_LEVEL);
    c->owned=true;c->running=model.fail_stage!=FailClock;c->measured_khz=khz;c->rail_uv=800000;
    model.clock_started=TRUE;return c->running?TG_GPU_CLOCK_OK:TG_GPU_CLOCK_COUNTER_ERROR;
}
enum tg_gpu_clock_result tg_gpu_clock_stop(const struct tg_gpu_power_io *i,struct tg_gpu_clock *c)
{
    CHECK(i->context==&model && c->owned && model.drains && level==PASSIVE_LEVEL);++model.clock_stops;
    if(model.fail_clock_stop)return TG_GPU_CLOCK_READBACK_ERROR;
    c->owned=c->running=false;return TG_GPU_CLOCK_OK;
}
enum tg_gpu_execute_result tg_gpu_boot_graphics(const struct tg_gpu_power_io *i,
    const struct tg_gpu_power *p,void *cpu,uint64_t physical,const struct tg_gpu_gr_payload *firmware,
    const struct tg_gpu_gr_work *w,struct tg_gpu_execution *s,struct tg_gpu_status *status)
{
    CHECK(i->context==&model && p->ready && cpu==memory[0] && physical==0xc2000000 && firmware);
    CHECK(w->cpu==memory[1] && w->physical==0xc3000000 && w->bytes==TG_GPU_GR_CHANNEL_WORK_BYTES);
    CHECK(status->expected[0]!=status->expected[1] && !status->tested && level==PASSIVE_LEVEL);
    s->owned=s->fifo_ready=true;s->boot_ready=model.fail_stage!=FailBoot;
    status->graphics=(struct tg_gpu_gr_context_status){.stage=9,.context_bytes=512,.gpcs=1,.tpcs=2,.zculls=4,
        .image_magic=0x600dc0de,.image_changed_words=64};
    return s->boot_ready?TG_GPU_EXEC_OK:TG_GPU_EXEC_GR_BOOT;
}
enum tg_gpu_execute_result tg_gpu_boot_map_userd(const struct tg_gpu_power_io *i,
    const struct tg_gpu_power *p,void *cpu,uint64_t physical,uint64_t userd,
    const struct tg_gpu_execution *s,struct tg_gpu_status *status)
{
    CHECK(i->context==&model && p->ready && s->boot_ready && !s->channel_bound && level==PASSIVE_LEVEL);
    CHECK(cpu==memory[0] && physical==0xc2000000 && userd==0xc1002000);
    CHECK(engine.HardwareOwner==&hardware && !engine.UserdBar1Ready);model.mapped=TRUE;status->bar1_phase=7;
    return model.fail_stage==FailMap?TG_GPU_EXEC_MMU_TIMEOUT:TG_GPU_EXEC_OK;
}
void tg_gpu_execute_stop(const struct tg_gpu_power_io *i,struct tg_gpu_execution *s)
{
    CHECK(i->context==&model && hardware.Power.ready && level==PASSIVE_LEVEL);
    if(s->owned){model.execute_stopped=TRUE;s->boot_ready=s->fifo_ready=false;}
}
NTSTATUS TgGpuInitializeGraphicsTemplate(TG_GPU_CONTEXT_ENGINE *e,
    const struct tg_gpu_gr_work *w,const struct tg_gpu_gr_context_status *s)
{
    CHECK(e==&engine && w->cpu==memory[1] && s->stage==9 && model.mapped && level==PASSIVE_LEVEL);
    if(model.fail_stage==FailTemplate)return STATUS_DEVICE_DATA_ERROR;
    NTSTATUS status=TgGpuAllocatePrivateMemory(&e->Dxgk,TG_GPU_GR_IMAGE_MAX_BYTES,0x80000000,0xffffffff,&e->Graphics.Memory);
    if(NT_SUCCESS(status)){e->Graphics.ImageBytes=s->context_bytes;if(!e->GraphicsGeneration)e->GraphicsGeneration=1;}return status;
}
static void Setup(void)
{
    for(UINT i=0;i<3;++i)CHECK(!used[i]);
    memset(&engine,0,sizeof(engine));memset(&hardware,0,sizeof(hardware));memset(&model,0,sizeof(model));
    memset(&queue,0,sizeof(queue));memset(&worker,0,sizeof(worker));
    InitializeListHead(&engine.Contexts);fail_join=fail_queue=fail_thread=FALSE;CHECK(!gate_depth);
    model.fail_after=UINT32_MAX;
    engine.Dxgk.Version=DXGKDDI_INTERFACE_VERSION_WDDM2_4;engine.Dxgk.DeviceHandle=(HANDLE)1;
    engine.Dxgk.DxgkCbAllocateContiguousMemory=Allocate;engine.Dxgk.DxgkCbFreeContiguousMemory=Free;
    engine.Commands=(TG_GPU_PRIVATE_MEMORY){(HANDLE)9,commands,{.QuadPart=0xc1000000},sizeof(commands)};
}
static void Finish(void)
{
    CHECK(!hardware.Engine && !engine.HardwareOwner && !hardware.Boot.Handle && !hardware.Work.Handle);
    CHECK(!engine.UserdBar1Ready && !hardware.Ready);
    CHECK(NT_SUCCESS(TgGpuFreePrivateMemory(&engine.Dxgk,&engine.Graphics.Memory)));
    for(UINT i=0;i<3;++i)CHECK(!used[i]);
}
static void TestReset(void)
{
    struct tg_gpu_gr_payload firmware={0};TG_GPU_CONTEXT lost={0};UINT before;
    for(UINT failure=0;failure<7;++failure) {
        Setup();CHECK(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)==STATUS_SUCCESS);
        CHECK(TgGpuInitializeWorkQueue(&queue,&engine)==STATUS_SUCCESS);
        CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);
        lost.GraphicsGeneration=1;lost.Root.Valid=TRUE;InsertTailList(&engine.Contexts,&lost.Link);
        engine.LastCompletedFence=7;engine.Marker=2;memset(commands,0xa5,sizeof(commands));
        if(failure==1)fail_join=TRUE;
        if(failure==2)fail_queue=TRUE;
        if(failure==3)model.fail_drain=TRUE;
        UINT trace_before=reset_trace_records,packets_before=packet_trace_calls;
        NTSTATUS status=TgGpuResetBackend(&engine,&queue,&hardware,&worker);
        CHECK(reset_trace_records==trace_before+2 && !reset_trace_open && reset_trace_status==status);
        CHECK(packet_trace_calls==packets_before+(failure==1?0:1));
        if(failure>=1 && failure<=3) {
            CHECK(!NT_SUCCESS(status) && !model.frees && engine.Resetting && engine.RecoveryPending);
            fail_join=fail_queue=FALSE;model.fail_drain=FALSE;
            CHECK(TgGpuResetBackend(&engine,&queue,&hardware,&worker)==STATUS_SUCCESS);
        } else CHECK(status==STATUS_SUCCESS);
        CHECK(hardware.Isolated && hardware.Engine==&engine && !hardware.Power.owned && !worker.Thread);
        CHECK(!queue.Engine && !engine.QueueOwner && !lost.Root.Valid && lost.GraphicsGeneration==1);
        CHECK(engine.GraphicsGeneration==2 && engine.LastCompletedFence==7 && engine.Marker==2 && !model.frees);
        CHECK(used[0] && used[1] && used[2] && commands[0]==0xa5);
        before=model.reads+model.drains+model.downs+model.clock_stops;
        CHECK(TgGpuResetBackend(&engine,&queue,&hardware,&worker)==STATUS_SUCCESS);
        CHECK(before==model.reads+model.drains+model.downs+model.clock_stops);
        if(failure==4)model.fail_free=(HANDLE)2;
        if(failure==5)model.fail_free=(HANDLE)3;
        if(failure==6)fail_thread=TRUE;
        status=TgGpuRestartBackend(&engine,&queue,&hardware,&worker,&hardware.Io,0x33,&firmware);
        if(failure>=4) {
            CHECK(!NT_SUCCESS(status) && engine.Resetting && engine.RecoveryPending && !worker.Thread);
            model.fail_free=NULL;fail_thread=FALSE;
            CHECK(TgGpuRestartBackend(&engine,&queue,&hardware,&worker,&io,0x33,&firmware)==STATUS_SUCCESS);
        } else CHECK(status==STATUS_SUCCESS);
        CHECK(!engine.Resetting && !engine.RecoveryPending && hardware.Ready && worker.Thread && queue.Engine==&engine);
        CHECK(engine.GraphicsGeneration>=2 && lost.GraphicsGeneration!=engine.GraphicsGeneration && !lost.Root.Valid);
        CHECK(engine.LastCompletedFence==7 && engine.Marker==2 && commands[0]==0 && !gate_depth);
        CHECK(TgGpuResetBackend(&engine,&queue,&hardware,&worker)==STATUS_SUCCESS);
        CHECK(TgGpuStopHardware(&hardware)==STATUS_SUCCESS);RemoveEntryList(&lost.Link);Finish();
    }
}
void start(void)
{
    struct tg_gpu_gr_payload firmware={0};ULONG written;UINT before;
    static const char pass[]="PASS: cold hardware/isolation and two-phase global reset/restart, retained backing, lost epochs and failed join/free/thread retry (controlled substitutes)\r\n";
    for(UINT n=0;n<3;++n) {
        Setup();model.fail_after=n;
        CHECK(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)==STATUS_INSUFFICIENT_RESOURCES);
        CHECK(hardware.Engine==&engine && engine.HardwareOwner==&hardware && !hardware.Ready);
        CHECK(NT_SUCCESS(TgGpuStopHardware(&hardware)));Finish();
    }
    for(UINT stage=FailPower;stage<=FailTemplate;++stage) {
        Setup();model.fail_stage=stage;
        CHECK(!NT_SUCCESS(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)));
        CHECK(!hardware.Ready && engine.HardwareOwner==&hardware);
        CHECK(NT_SUCCESS(TgGpuStopHardware(&hardware)));Finish();
    }
    Setup();CHECK(NT_SUCCESS(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)));
    CHECK(hardware.Ready && engine.UserdBar1Ready && engine.UserdBar1Offset==0x13000 && model.mapped);
    CHECK(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)==STATUS_INVALID_PARAMETER);
    engine.QueueOwner=(PVOID)1;CHECK(TgGpuStopHardware(&hardware)==STATUS_DEVICE_BUSY && hardware.Ready);
    engine.QueueOwner=NULL;model.fail_drain=TRUE;
    CHECK(TgGpuStopHardware(&hardware)==STATUS_IO_TIMEOUT && hardware.Boot.Handle && !model.frees && !model.downs);
    model.fail_drain=FALSE;CHECK(NT_SUCCESS(TgGpuStopHardware(&hardware)));Finish();
    Setup();CHECK(NT_SUCCESS(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)));
    model.fail_down=TRUE;CHECK(TgGpuStopHardware(&hardware)==STATUS_DEVICE_DATA_ERROR && !model.frees);
    model.fail_down=FALSE;CHECK(NT_SUCCESS(TgGpuStopHardware(&hardware)));Finish();
    Setup();CHECK(NT_SUCCESS(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)));
    model.fail_isolation=TRUE;CHECK(TgGpuStopHardware(&hardware)==STATUS_DEVICE_DATA_ERROR && !model.frees && hardware.Power.owned);
    model.fail_isolation=FALSE;CHECK(NT_SUCCESS(TgGpuStopHardware(&hardware)));Finish();
    Setup();CHECK(NT_SUCCESS(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)));
    model.fail_free=(HANDLE)2;
    CHECK(TgGpuStopHardware(&hardware)==STATUS_UNSUCCESSFUL && hardware.Isolated && engine.HardwareOwner==&hardware);
    before=model.reads+model.downs+model.drains+model.clock_stops;model.fail_free=NULL;
    CHECK(NT_SUCCESS(TgGpuStopHardware(&hardware)) && before==model.reads+model.downs+model.drains+model.clock_stops);Finish();
    Setup();CHECK(NT_SUCCESS(TgGpuStartHardware(&hardware,&engine,&io,0x33,&firmware)));
    model.fail_clock_stop=TRUE;
    CHECK(TgGpuStopHardware(&hardware)==STATUS_DEVICE_DATA_ERROR && hardware.Isolated && !hardware.Clock.owned);
    CHECK(hardware.Status.restore_result && !hardware.Engine);Finish();
    TestReset();
    WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL);ExitProcess(0);
}
