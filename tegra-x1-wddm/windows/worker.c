/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include "worker.h"

static VOID run(PVOID context)
{
    TG_GPU_WORKER *w=context;
    PEX_TIMER poll_timer=ExAllocateTimer(NULL,NULL,EX_TIMER_HIGH_RESOLUTION);
    PVOID objects[3]={&w->Stop,&w->Queue->Wake,poll_timer};
    LARGE_INTEGER timeout,dvfs_timeout;NTSTATUS status=STATUS_SUCCESS;BOOLEAN pending=FALSE;
    timeout.QuadPart=-10000; /* one millisecond only while GPU work is pending */
    for(;;) {
        BOOLEAN dvfs=InterlockedCompareExchange(&w->Hardware->Dvfs.Configured,0,0)!=0;
        PLARGE_INTEGER wait_timeout=pending?&timeout:
            (dvfs?TgGpuDvfsWaitTimeout(&w->Hardware->Dvfs,&dvfs_timeout):NULL);
        /* A relative dispatcher timeout can defer a completed fence to the
         * next system tick. Arm a one-shot only while work is outstanding. */
        BOOLEAN precise=pending && poll_timer!=NULL;
        if(precise)(VOID)ExSetTimer(poll_timer,timeout.QuadPart,0,NULL);
        NTSTATUS wait=KeWaitForMultipleObjects(precise?3:2,objects,WaitAny,Executive,KernelMode,
            FALSE,precise?NULL:wait_timeout,NULL);
        if(precise) {
            if(wait==STATUS_WAIT_0+2)wait=STATUS_TIMEOUT;
            else (VOID)ExCancelTimer(poll_timer,NULL);
        }
        if(wait==STATUS_WAIT_0)break;
        if(wait!=STATUS_WAIT_0+1 && !(wait_timeout && wait==STATUS_TIMEOUT)) {
            status=NT_SUCCESS(wait)?STATUS_UNSUCCESSFUL:wait;break;
        }
        if(!pending && dvfs) {
            status=TgGpuDvfsStep(w->Hardware,w->Queue,wait==STATUS_WAIT_0+1);
            if(!NT_SUCCESS(status))break;
        }
        if(pending || wait==STATUS_WAIT_0+1) {
            if(!pending && dvfs)TgGpuDvfsNoteServiceBegin(&w->Hardware->Dvfs);
            status=TgGpuServiceWorkQueue(w->Queue,&w->Hardware->Io,&w->Hardware->Power);
            if(!NT_SUCCESS(status))break;
            pending=status==STATUS_PENDING;
            if(!pending && dvfs)TgGpuDvfsNoteServiceComplete(&w->Hardware->Dvfs);
        }
    }
    if(poll_timer)(VOID)ExDeleteTimer(poll_timer,TRUE,TRUE,NULL);
    if(!NT_SUCCESS(status)) {
        KIRQL irql;
        KeAcquireSpinLock(&w->Queue->Lock,&irql);
        w->Queue->Paused=TRUE;w->Queue->Failure=status;
        KeReleaseSpinLock(&w->Queue->Lock,irql);w->Failure=status;
        (VOID)TgGpuRequestEngineRecoveryAt(w->Queue->Engine,status,TgGpuRecoveryWorker,NULL);
    }
    (VOID)PsTerminateSystemThread(NT_SUCCESS(status)?STATUS_SUCCESS:status);
}
NTSTATUS TgGpuStartWorker(TG_GPU_WORKER *w,TG_GPU_WORK_QUEUE *q,TG_GPU_HARDWARE *h)
{
    OBJECT_ATTRIBUTES attributes;NTSTATUS status;KIRQL irql;
    PAGED_CODE();
    if(!w || !q || !h || w->Thread || w->Queue || w->Hardware ||
       !q->Engine || h->Engine!=q->Engine || !h->Ready || !h->Power.owned || !h->Power.ready ||
       !h->Engine->UserdBar1Ready)return STATUS_INVALID_DEVICE_STATE;
    KeAcquireSpinLock(&q->Lock,&irql);
    status=q->Paused || !NT_SUCCESS(q->Failure) || q->Servicing?STATUS_DEVICE_BUSY:STATUS_SUCCESS;
    KeReleaseSpinLock(&q->Lock,irql);
    if(!NT_SUCCESS(status))return status;
    w->Queue=q;w->Hardware=h;w->Failure=STATUS_SUCCESS;
    KeInitializeEvent(&w->Stop,NotificationEvent,FALSE);
    InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    status=PsCreateSystemThread(&w->Thread,SYNCHRONIZE,&attributes,NULL,NULL,run,w);
    if(!NT_SUCCESS(status)){w->Thread=NULL;w->Queue=NULL;w->Hardware=NULL;}
    return status;
}
NTSTATUS TgGpuStopWorker(TG_GPU_WORKER *w)
{
    NTSTATUS status;
    PAGED_CODE();
    if(!w)return STATUS_INVALID_PARAMETER;
    if(!w->Thread)return STATUS_SUCCESS;
    (VOID)KeSetEvent(&w->Stop,IO_NO_INCREMENT,FALSE);
    status=ZwWaitForSingleObject(w->Thread,FALSE,NULL);
    if(status!=STATUS_SUCCESS)return NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status;
    status=ZwClose(w->Thread);
    if(NT_SUCCESS(status)){w->Thread=NULL;w->Queue=NULL;w->Hardware=NULL;}
    return status;
}
