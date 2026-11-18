/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include "../windows/worker.h"
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
static TG_GPU_WORKER worker;
static TG_GPU_WORK_QUEUE queue;
static TG_GPU_HARDWARE hardware;
static TG_GPU_CONTEXT_ENGINE engine;
static struct {
    PKSTART_ROUTINE routine;
    PVOID context;
    NTSTATUS create,join,close,exit_status,service[3],dvfs_status;
    NTSTATUS waits[4];BOOLEAN timed[4],terminated;
    LONGLONG expected_timeout[4];
    BOOLEAN have_timer,armed;
    UINT timer_allocs,timer_arms,timer_cancels,timer_deletes;
    UINT services,wait_count,joins,closes,recovery_requests,dvfs_steps,dvfs_wakes,dvfs_begins,dvfs_completes;
} model;
/* No real thread/dispatcher/GPU is used by this native scheduling fixture. */
static NTSTATUS Create(PHANDLE out,ULONG access,POBJECT_ATTRIBUTES attr,HANDLE process,
    PCLIENT_ID client,PKSTART_ROUTINE routine,PVOID context)
{
    CHECK(access==SYNCHRONIZE && attr->Attributes==OBJ_KERNEL_HANDLE && !attr->ObjectName);
    CHECK(!process && !client && routine && context==&worker);
    if(!NT_SUCCESS(model.create))return model.create;
    *out=(HANDLE)123;model.routine=routine;model.context=context;return STATUS_SUCCESS;
}
static NTSTATUS Terminate(NTSTATUS status)
{CHECK(!model.terminated && model.timer_allocs==1 && !model.armed &&
       model.timer_deletes==(UINT)model.have_timer);
 model.terminated=TRUE;model.exit_status=status;return STATUS_SUCCESS;}
static PEX_TIMER AllocateTimer(PEXT_CALLBACK callback,PVOID context,ULONG flags)
{
    CHECK(!callback && !context && flags==EX_TIMER_HIGH_RESOLUTION && !model.timer_allocs);
    ++model.timer_allocs;return model.have_timer?(PEX_TIMER)&model.armed:NULL;
}
static BOOLEAN SetTimer(PEX_TIMER timer,LONGLONG due,LONGLONG period,PEXT_SET_PARAMETERS params)
{
    CHECK(timer==(PEX_TIMER)&model.armed && !model.armed && due==-10000 && !period && !params);
    ++model.timer_arms;model.armed=TRUE;return FALSE;
}
static BOOLEAN CancelTimer(PEX_TIMER timer,PEXT_CANCEL_PARAMETERS params)
{
    CHECK(timer==(PEX_TIMER)&model.armed && model.armed && !params);
    ++model.timer_cancels;model.armed=FALSE;return TRUE;
}
static BOOLEAN DeleteTimer(PEX_TIMER timer,BOOLEAN cancel,BOOLEAN wait,PEXT_DELETE_PARAMETERS params)
{
    CHECK(timer==(PEX_TIMER)&model.armed && !model.armed && cancel && wait && !params);
    ++model.timer_deletes;return FALSE;
}
static VOID NTAPI InitializeEvent(PRKEVENT e,EVENT_TYPE type,BOOLEAN state)
{CHECK(e==&worker.Stop && type==NotificationEvent && !state);e->Header.SignalState=state;}
static LONG NTAPI SetEvent(PRKEVENT e,KPRIORITY priority,BOOLEAN wait)
{CHECK(e==&worker.Stop && !priority && !wait);e->Header.SignalState=1;return 0;}
static KIRQL NTAPI Lock(PKSPIN_LOCK l){CHECK(l==&queue.Lock && !*l);*l=1;return PASSIVE_LEVEL;}
static VOID NTAPI Release(PKSPIN_LOCK l,KIRQL old){CHECK(l==&queue.Lock && *l && old==PASSIVE_LEVEL);*l=0;}
static NTSTATUS NTAPI Wait(ULONG count,PVOID objects[],WAIT_TYPE type,KWAIT_REASON reason,
    KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout,PKWAIT_BLOCK blocks)
{
    UINT i=model.wait_count++;
    CHECK(i<4 && objects[0]==&worker.Stop && objects[1]==&queue.Wake);
    CHECK(type==WaitAny && reason==Executive && mode==KernelMode && !alertable && !blocks);
    if(model.armed) {
        CHECK(count==3 && objects[2]==(PVOID)&model.armed && !timeout && model.timed[i]);
        if(model.waits[i]==STATUS_TIMEOUT){model.armed=FALSE;return STATUS_WAIT_0+2;}
        return model.waits[i];
    }
    CHECK(count==2);
    CHECK((timeout!=NULL)==model.timed[i]);
    if(timeout)CHECK(timeout->QuadPart==(model.expected_timeout[i]?model.expected_timeout[i]:-10000));
    return model.waits[i];
}
static NTSTATUS NTAPI Join(HANDLE h,BOOLEAN alertable,PLARGE_INTEGER timeout)
{CHECK(h==(HANDLE)123 && !alertable && !timeout && worker.Stop.Header.SignalState);++model.joins;return model.join;}
static NTSTATUS NTAPI Close(HANDLE h)
{CHECK(h==(HANDLE)123 && model.terminated && model.joins);++model.closes;return model.close;}
NTSTATUS (* const __imp_PsCreateSystemThread)(PHANDLE,ULONG,POBJECT_ATTRIBUTES,HANDLE,PCLIENT_ID,PKSTART_ROUTINE,PVOID)=Create;
NTSTATUS (* const __imp_PsTerminateSystemThread)(NTSTATUS)=Terminate;
__typeof__(&ExAllocateTimer) const __imp_ExAllocateTimer=AllocateTimer;
__typeof__(&ExSetTimer) const __imp_ExSetTimer=SetTimer;
__typeof__(&ExCancelTimer) const __imp_ExCancelTimer=CancelTimer;
__typeof__(&ExDeleteTimer) const __imp_ExDeleteTimer=DeleteTimer;
VOID (NTAPI * const __imp_KeInitializeEvent)(PRKEVENT,EVENT_TYPE,BOOLEAN)=InitializeEvent;
LONG (NTAPI * const __imp_KeSetEvent)(PRKEVENT,KPRIORITY,BOOLEAN)=SetEvent;
KIRQL (NTAPI * const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=Lock;
VOID (NTAPI * const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=Release;
NTSTATUS (NTAPI * const __imp_KeWaitForMultipleObjects)(ULONG,PVOID[],WAIT_TYPE,KWAIT_REASON,
    KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER,PKWAIT_BLOCK)=Wait;
NTSTATUS (NTAPI * const __imp_ZwWaitForSingleObject)(HANDLE,BOOLEAN,PLARGE_INTEGER)=Join;
NTSTATUS (NTAPI * const __imp_ZwClose)(HANDLE)=Close;
NTSTATUS TgGpuServiceWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power)
{
    CHECK(q==&queue && io==&hardware.Io && power==&hardware.Power && !queue.Lock && !model.terminated);
    CHECK(model.services<3);return model.service[model.services++];
}
NTSTATUS TgGpuRequestEngineRecoveryAt(TG_GPU_CONTEXT_ENGINE *e,NTSTATUS status,
    enum tg_gpu_recovery_source source,const DXGKARG_SUBMITCOMMANDVIRTUAL *submit)
{
    CHECK(source==TgGpuRecoveryWorker && !submit);
    CHECK(e==&engine && !NT_SUCCESS(status) && queue.Paused && queue.Failure==status);
    ++model.recovery_requests;return STATUS_SUCCESS;
}
PLARGE_INTEGER TgGpuDvfsWaitTimeout(const TG_GPU_DVFS *d,LARGE_INTEGER *storage)
{
    CHECK(d==&hardware.Dvfs && storage && d->Configured);
    storage->QuadPart=-2000000LL;return storage;
}
VOID TgGpuDvfsNoteServiceBegin(TG_GPU_DVFS *d){CHECK(d==&hardware.Dvfs && d->Configured);++model.dvfs_begins;}
VOID TgGpuDvfsNoteServiceComplete(TG_GPU_DVFS *d){CHECK(d==&hardware.Dvfs && d->Configured);++model.dvfs_completes;}
NTSTATUS TgGpuDvfsStep(TG_GPU_HARDWARE *h,TG_GPU_WORK_QUEUE *q,BOOLEAN woke)
{
    CHECK(h==&hardware && q==&queue && h->Dvfs.Configured && !queue.Lock && !model.terminated);
    ++model.dvfs_steps;if(woke)++model.dvfs_wakes;return model.dvfs_status;
}
static void Setup(void)
{
    CHECK(!worker.Thread && !worker.Queue && !worker.Hardware);
    memset(&worker,0,sizeof(worker));memset(&queue,0,sizeof(queue));
    memset(&hardware,0,sizeof(hardware));memset(&model,0,sizeof(model));
    queue.Engine=&engine;hardware.Engine=&engine;hardware.Ready=TRUE;
    hardware.Power.owned=hardware.Power.ready=true;engine.UserdBar1Ready=TRUE;
    model.waits[0]=STATUS_WAIT_0+1;model.waits[1]=STATUS_TIMEOUT;
    model.waits[2]=STATUS_WAIT_0+1;model.waits[3]=STATUS_WAIT_0;
    model.timed[1]=model.timed[2]=TRUE;model.service[0]=model.service[1]=STATUS_PENDING;
}
void start(void)
{
    static const char pass[]="PASS: pending one-shot, early wake and stop, allocation fallback, DVFS, recovery and worker lifetime (controlled imports)\r\n";
    ULONG written;
    Setup();hardware.Ready=FALSE;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_INVALID_DEVICE_STATE);
    hardware.Ready=TRUE;model.create=STATUS_INSUFFICIENT_RESOURCES;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!worker.Thread && !worker.Queue && !worker.Hardware);
    Setup();CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_INVALID_DEVICE_STATE);
    model.routine(model.context);
    CHECK(model.services==3 && model.wait_count==4 && model.terminated && model.exit_status==STATUS_SUCCESS);
    CHECK(!queue.Paused && worker.Queue==&queue && worker.Hardware==&hardware);
    CHECK(!model.recovery_requests);
    model.join=STATUS_TIMEOUT;
    CHECK(TgGpuStopWorker(&worker)==STATUS_DEVICE_BUSY && !model.closes && worker.Thread);
    model.join=STATUS_SUCCESS;model.close=STATUS_UNSUCCESSFUL;
    CHECK(TgGpuStopWorker(&worker)==STATUS_UNSUCCESSFUL && worker.Thread && worker.Queue==&queue);
    model.close=STATUS_SUCCESS;CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS && !worker.Thread && !worker.Queue && !worker.Hardware);
    CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS);
    Setup();model.have_timer=TRUE;hardware.Dvfs.Configured=1;
    model.timed[0]=model.timed[3]=TRUE;model.expected_timeout[0]=model.expected_timeout[3]=-2000000;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);model.routine(model.context);
    CHECK(model.services==3 && model.timer_arms==2 && model.timer_cancels==1 &&
          model.dvfs_begins==1 && model.dvfs_completes==1 && model.dvfs_steps==1);
    CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS);
    Setup();model.have_timer=TRUE;model.waits[1]=STATUS_WAIT_0;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);model.routine(model.context);
    CHECK(model.services==1 && model.timer_arms==1 && model.timer_cancels==1 && !model.recovery_requests);
    CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS);
    Setup();model.service[0]=STATUS_DEVICE_DATA_ERROR;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);model.routine(model.context);
    CHECK(model.wait_count==1 && model.services==1 && model.exit_status==STATUS_DEVICE_DATA_ERROR);
    CHECK(queue.Paused && queue.Failure==STATUS_DEVICE_DATA_ERROR && worker.Failure==STATUS_DEVICE_DATA_ERROR);
    CHECK(model.recovery_requests==1);
    CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS);
    Setup();model.waits[0]=STATUS_UNSUCCESSFUL;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);model.routine(model.context);
    CHECK(!model.services && queue.Paused && worker.Failure==STATUS_UNSUCCESSFUL);
    CHECK(model.recovery_requests==1 && !model.dvfs_steps);
    CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS);
    /* Configured DVFS hook: idle holdoff timer, quiescent wake/idle steps, and
     * zero DVFS calls while a packet is pending on the GPU. */
    Setup();hardware.Dvfs.Configured=1;
    model.timed[0]=model.timed[1]=model.timed[2]=model.timed[3]=TRUE;
    model.expected_timeout[0]=model.expected_timeout[3]=-2000000LL;
    model.expected_timeout[1]=-10000LL;model.expected_timeout[2]=-2000000LL;
    model.waits[0]=STATUS_WAIT_0+1;model.waits[1]=STATUS_TIMEOUT;
    model.waits[2]=STATUS_TIMEOUT;model.waits[3]=STATUS_WAIT_0;
    model.service[0]=STATUS_PENDING;model.service[1]=STATUS_SUCCESS;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);model.routine(model.context);
    CHECK(model.wait_count==4 && model.services==2 && model.dvfs_steps==2 && model.dvfs_wakes==1);
    CHECK(model.dvfs_begins==1 && model.dvfs_completes==1 && !queue.Paused && !model.recovery_requests);
    CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS);
    Setup();hardware.Dvfs.Configured=1;model.timed[0]=TRUE;model.expected_timeout[0]=-2000000LL;
    model.waits[0]=STATUS_TIMEOUT;model.dvfs_status=STATUS_DEVICE_DATA_ERROR;
    CHECK(TgGpuStartWorker(&worker,&queue,&hardware)==STATUS_SUCCESS);model.routine(model.context);
    CHECK(model.dvfs_steps==1 && !model.services && queue.Paused && model.recovery_requests==1);
    CHECK(TgGpuStopWorker(&worker)==STATUS_SUCCESS);
    CHECK(WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL));ExitProcess(0);
}
