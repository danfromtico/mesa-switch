/* SPDX-License-Identifier: MIT */
#include "context.h"
#define TAG 0x43674754u

NTSTATUS TgGpuReferenceVideoDevice(TG_GPU_CONTEXT_ENGINE *e, HANDLE handle, TG_GPU_DEVICE **out)
{
    KIRQL irql;
    if (!e || !out) return STATUS_INVALID_PARAMETER;
    *out=NULL;
    KeAcquireSpinLock(&e->Lock,&irql);
    for (LIST_ENTRY *l=e->Devices.Flink;l!=&e->Devices;l=l->Flink) {
        TG_GPU_DEVICE *d=CONTAINING_RECORD(l,TG_GPU_DEVICE,Link);
        if (d!=handle) continue;
        if (!d->Closing && d->Process && !d->Process->Closing &&
            d->VideoReferences!=MAXUINT) {
            ++d->VideoReferences; *out=d;
        }
        break;
    }
    KeReleaseSpinLock(&e->Lock,irql);
    return *out ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}
void TgGpuDereferenceVideoDevice(TG_GPU_DEVICE *d)
{
    KIRQL irql;
    KeAcquireSpinLock(&d->Engine->Lock,&irql);
    NT_ASSERT(d->VideoReferences);
    --d->VideoReferences;
    KeReleaseSpinLock(&d->Engine->Lock,irql);
}
NTSTATUS APIENTRY TgGpuDdiDestroyDevice(HANDLE device)
{
    TG_GPU_DEVICE *d=device; TG_GPU_CONTEXT_ENGINE *e; KIRQL irql;
    if (!d) return STATUS_INVALID_PARAMETER;
    e=d->Engine; KeAcquireSpinLock(&e->Lock,&irql);
    if (d->Contexts || d->OpenAllocations) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    d->Closing=TRUE;
    if (d->VideoReferences) {
        KeReleaseSpinLock(&e->Lock,irql);
        NTSTATUS status=e->VideoCloseDevice ? e->VideoCloseDevice(e->VideoOwner,d) : STATUS_DEVICE_BUSY;
        if (status!=STATUS_SUCCESS) return status;
        KeAcquireSpinLock(&e->Lock,&irql);
        if (d->VideoReferences) { KeReleaseSpinLock(&e->Lock,irql); return STATUS_DEVICE_BUSY; }
    }
    RemoveEntryList(&d->Link); if (d->Process) --d->Process->Devices;
    KeReleaseSpinLock(&e->Lock,irql); ExFreePoolWithTag(d,TAG); return STATUS_SUCCESS;
}
