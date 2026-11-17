/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include "host1x_clock.h"

#define TG_CLOCK_TAG 0x6b436754u
#define TG_CLOCK_TIMEOUT (-10000000LL)
enum { ClockPending, ClockAdmitted, ClockStopping, ClockCompleted };

struct _TG_HOST1X_CLOCK_LEASE {
    PIRP Irp;
    PFILE_OBJECT File;
    volatile LONG References, State, ReleaseRequested;
    KEVENT Ready, Done;
    NTSTATUS Status;
    ULONG_PTR Information;
    BOOLEAN ReleaseFailed;
    TG_HOST1X_QUIESCE Quiesce;
    PVOID Context;
    TG_HOST1X_CLOCK_REQUEST Op;
};

static VOID clock_dereference(TG_HOST1X_CLOCK_LEASE *c)
{
    if (InterlockedDecrement(&c->References) != 0) return;
    IoFreeIrp(c->Irp);
    ObfDereferenceObject(c->File);
    ExFreePoolWithTag(c, TG_CLOCK_TAG);
}

static NTSTATUS NTAPI clock_quiesce(PVOID context)
{
    TG_HOST1X_CLOCK_LEASE *c = context;
    LONG old = InterlockedExchange(&c->State, ClockStopping);
    /* Closing wins over admission. No hardware work is permitted before
     * Acquire successfully changes Pending to Admitted. */
    if (old == ClockPending) return STATUS_SUCCESS;
    if (old != ClockAdmitted) return STATUS_INVALID_DEVICE_STATE;
    return c->Quiesce(c->Context);
}

static NTSTATUS NTAPI clock_complete(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    TG_HOST1X_CLOCK_LEASE *c = context;
    UNREFERENCED_PARAMETER(device);
    c->Status = irp->IoStatus.Status;
    c->Information = irp->IoStatus.Information;
    InterlockedExchange(&c->State, ClockCompleted);
    KeSetEvent(&c->Ready, IO_NO_INCREMENT, FALSE); /* unsupported old provider */
    KeSetEvent(&c->Done, IO_NO_INCREMENT, FALSE);
    clock_dereference(c);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS TgHost1xClockAcquire(TG_GPU_PLATFORM *platform,
    TG_HOST1X_QUIESCE quiesce, PVOID context, TG_HOST1X_CLOCK_LEASE **lease,
    const char **stage)
{
    TG_HOST1X_CLOCK_LEASE *c;
    PFILE_OBJECT file;
    PDEVICE_OBJECT owner, target;
    PIO_STACK_LOCATION stack;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    PAGED_CODE();
    /* Preserve the failed public-API boundary even after the lease is closed.
     * These static strings carry no handles or runtime-private object data. */
#define CLOCK_STAGE(s) do { if (stage) *stage = (s); } while (0)
    CLOCK_STAGE("Host1x request validation");
    if (!lease || *lease || !quiesce) return STATUS_INVALID_PARAMETER;
    CLOCK_STAGE("Host1x bridge reference");
    status = TgGpuReferencePlatformBridge(platform, &file, &owner);
    if (!NT_SUCCESS(status)) return status;
    target = IoGetRelatedDeviceObject(file);
    if (!target) { status = STATUS_DEVICE_NOT_READY; goto fail_refs; }
    CLOCK_STAGE("Host1x request allocation");
    c = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), TG_CLOCK_TAG);
    if (!c) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail_refs; }
    RtlZeroMemory(c, sizeof(*c));
    c->Irp = IoAllocateIrp(target->StackSize, FALSE);
    if (!c->Irp) {
        ExFreePoolWithTag(c, TG_CLOCK_TAG);
        status = STATUS_INSUFFICIENT_RESOURCES; goto fail_refs;
    }
    c->References = 2; c->File = file;
    c->Quiesce = quiesce; c->Context = context;
    c->Status = STATUS_PENDING;
    KeInitializeEvent(&c->Ready, NotificationEvent, FALSE);
    KeInitializeEvent(&c->Done, NotificationEvent, FALSE);
    c->Op.Version = TG_HOST1X_CLOCK_ABI;
    c->Op.Size = sizeof(c->Op);
    c->Op.Ready = &c->Ready;
    c->Op.Quiesce = clock_quiesce;
    c->Op.Context = c;
    c->Op.ReadyStatus = STATUS_DEVICE_NOT_READY;
    c->Op.QuiesceStatus = STATUS_UNSUCCESSFUL;
    c->Irp->RequestorMode = KernelMode;
    c->Irp->AssociatedIrp.SystemBuffer = &c->Op;
    c->Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    c->Irp->IoStatus.Information = 0;
    stack = IoGetNextIrpStackLocation(c->Irp);
    stack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    stack->FileObject = file;
    stack->Parameters.DeviceIoControl.IoControlCode = TG_HOST1X_IOCTL_CLOCK_HOLD;
    stack->Parameters.DeviceIoControl.InputBufferLength = sizeof(c->Op);
    stack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(c->Op);
    CLOCK_STAGE("Host1x completion registration");
    status = IoSetCompletionRoutineEx(owner, c->Irp, clock_complete, c, TRUE, TRUE, TRUE);
    if (!NT_SUCCESS(status)) {
        c->References = 1; clock_dereference(c);
        ObfDereferenceObject(owner); return status;
    }
    ObfDereferenceObject(owner);
    *lease = c; /* retain on every submitted-request failure or timeout */
    CLOCK_STAGE("Host1x request dispatch");
    (VOID)IoCallDriver(target, c->Irp);
    timeout.QuadPart = TG_CLOCK_TIMEOUT;
    CLOCK_STAGE("Host1x ready event");
    status = KeWaitForSingleObject(&c->Ready, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS) {
        InterlockedExchange(&c->ReleaseRequested, 1);
        (VOID)IoCancelIrp(c->Irp);
        return status == STATUS_TIMEOUT || NT_SUCCESS(status) ? STATUS_IO_TIMEOUT : status;
    }
    if (InterlockedCompareExchange(&c->State, 0, 0) == ClockCompleted) {
        CLOCK_STAGE("Host1x IRP completed before admission");
        return NT_SUCCESS(c->Status) ? STATUS_DEVICE_NOT_READY : c->Status;
    }
    CLOCK_STAGE("Host1x provider readiness");
    if (c->Op.ReadyStatus != STATUS_SUCCESS)
        return NT_SUCCESS(c->Op.ReadyStatus) ? STATUS_DEVICE_PROTOCOL_ERROR : c->Op.ReadyStatus;
    CLOCK_STAGE("Host1x provider reply validation");
    if (c->Op.Version != TG_HOST1X_CLOCK_ABI || c->Op.Size != sizeof(c->Op) ||
        c->Op.Reserved || c->Op.Ready != &c->Ready ||
        c->Op.Quiesce != clock_quiesce || c->Op.Context != c ||
        !TgHost1xCoreFloorValid(c->Op.CoreBootFloorUv,c->Op.CoreAppliedUv) ||
        c->Op.GateL == MAXULONG || c->Op.ResetL == MAXULONG || c->Op.PllpBase == MAXULONG ||
        !(c->Op.GateL & (1u << 28)) || (c->Op.ResetL & (1u << 28)) ||
        (c->Op.PllpBase & ((1u << 31) | (1u << 30) | (1u << 27))) !=
            ((1u << 30) | (1u << 27)))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    CLOCK_STAGE("Host1x admission race");
    if (InterlockedCompareExchange(&c->State, ClockAdmitted, ClockPending) != ClockPending)
        return STATUS_DEVICE_NOT_READY;
    CLOCK_STAGE("Host1x lease admitted");
#undef CLOCK_STAGE
    return STATUS_SUCCESS;
fail_refs:
    ObfDereferenceObject(file); ObfDereferenceObject(owner);
    return status;
}

BOOLEAN TgHost1xClockHeld(TG_HOST1X_CLOCK_LEASE *c, ULONG *pllp, ULONG *core_floor)
{
    if (!c || InterlockedCompareExchange(&c->ReleaseRequested, 0, 0) ||
        InterlockedCompareExchange(&c->State, 0, 0) != ClockAdmitted) return FALSE;
    if (pllp) *pllp = c->Op.PllpBase;
    if (core_floor) *core_floor = c->Op.CoreBootFloorUv;
    return TRUE;
}

NTSTATUS TgHost1xClockRelease(TG_HOST1X_CLOCK_LEASE **lease)
{
    TG_HOST1X_CLOCK_LEASE *c;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    PAGED_CODE();
    if (!lease) return STATUS_INVALID_PARAMETER;
    c = *lease;
    if (!c) return STATUS_SUCCESS;
    InterlockedExchange(&c->ReleaseRequested, 1);
    if (InterlockedCompareExchange(&c->State, 0, 0) != ClockCompleted)
        (VOID)IoCancelIrp(c->Irp);
    timeout.QuadPart = TG_CLOCK_TIMEOUT;
    status = KeWaitForSingleObject(&c->Done, Executive, KernelMode, FALSE, &timeout);
    if (status != STATUS_SUCCESS)
        return status == STATUS_TIMEOUT || NT_SUCCESS(status) ? STATUS_IO_TIMEOUT : status;
    status = STATUS_SUCCESS;
    if (c->Op.ReadyStatus == STATUS_SUCCESS) {
        status = c->Information == sizeof(c->Op) ?
            c->Op.QuiesceStatus : STATUS_DEVICE_PROTOCOL_ERROR;
        if (status != STATUS_SUCCESS && NT_SUCCESS(status)) status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if (status != STATUS_SUCCESS) {
        if (!c->ReleaseFailed) { c->ReleaseFailed = TRUE; return status; }
        /* A completed IRP with a failed stop is not proof that DMA retired.
         * Keep its record until an explicit retry obtains actual quiescence. */
        status = c->Quiesce(c->Context);
        if (status != STATUS_SUCCESS)
            return NT_SUCCESS(status) ? STATUS_DEVICE_PROTOCOL_ERROR : status;
    }
    *lease = NULL;
    clock_dereference(c);
    return status;
}
