/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWFAN_HOST1X_CLOCK_H
#define SWFAN_HOST1X_CLOCK_H
#include "windows/host1x_clock_abi.h"

/* The existing FAN0 CAR resource covers these status registers. Only the
 * already-running Host1x clock and locked PLLP may be borrowed. No global
 * clock, mux or reset is changed, including after a failed quiesce callback.
 * SwitchFan's CPU/EMC suspend paths must keep these inherited clocks intact
 * until the power-managed queue has drained before D0Exit/ReleaseHardware.
 */
typedef struct _SW_HOST1X_PROVIDER {
    WDFQUEUE Queue;
    KSPIN_LOCK Lock;
    WDFREQUEST Held;
    BOOLEAN Faulted;
} SW_HOST1X_PROVIDER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SW_HOST1X_PROVIDER, SwHost1xProvider)

typedef struct _SW_HOST1X_REQUEST {
    WDFDEVICE Device;
    WDFREQUEST Request;
    TG_HOST1X_CLOCK_REQUEST *Op;
    PIO_WORKITEM Work;
    KEVENT Published;
    volatile LONG WorkQueued;
    NTSTATUS CompletionStatus;
    BOOLEAN PowerReference;
} SW_HOST1X_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SW_HOST1X_REQUEST, SwHost1xRequest)

/* Only failed kernel requests are recorded, using the provider's existing
 * passive diagnostic sink. Preserve the actual status and ownership path. */
static VOID SwHost1xFailure(ULONG stage, NTSTATUS status)
{
    SwFanTrace(L"Host1xFailureStage", stage);
    SwFanTrace(L"Host1xFailureStatus", (ULONG)status);
}

static NTSTATUS SwHost1xReadCoreFloor(WDFDEVICE device,
                                     TG_HOST1X_CLOCK_REQUEST *op)
{
    WDFIOTARGET target;
    WDFMEMORY memory;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_MEMORY_DESCRIPTOR buffer;
    WDF_REQUEST_SEND_OPTIONS options;
    SWCORE_VOTE *query;
    ULONG_PTR bytes = 0;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, SWCORE_DEVICE_NAME);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfIoTargetCreate(device, &attributes, &target);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name,
                                               GENERIC_READ | GENERIC_WRITE);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(target, &open);
    if (!NT_SUCCESS(status)) { WdfObjectDelete(target); return status; }
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = target;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, 0x63485653u,
                            sizeof(*query), &memory, (PVOID *)&query);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(query, sizeof(*query));
        query->Version = SWCORE_VOTE_VERSION;
        /* Query only: never clear EMC's vote or open the PMIC/SPB directly.
         * The existing owner clamps all future EMC writes to BootFloorUv.
         * WDF memory retains the buffer if a timed request is still retiring. */
        WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&buffer, memory, NULL);
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
        status = WdfIoTargetSendIoctlSynchronously(target, NULL,
            IOCTL_SWCORE_EMC_VOTE, &buffer, &buffer, &options, &bytes);
        if (status == STATUS_SUCCESS) {
            if (bytes != sizeof(*query) || query->Version != SWCORE_VOTE_VERSION ||
                query->SetVote || query->MinimumUv || query->VoteUv > query->AppliedUv ||
                !TgHost1xCoreFloorValid(query->BootFloorUv,query->AppliedUv))
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            else {
                op->CoreBootFloorUv = query->BootFloorUv;
                op->CoreAppliedUv = query->AppliedUv;
            }
        } else if (NT_SUCCESS(status)) status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    WdfIoTargetClose(target);
    WdfObjectDelete(target);
    return status;
}

static VOID SwHost1xDropClaim(WDFDEVICE device, WDFREQUEST request, NTSTATUS quiesce)
{
    SW_HOST1X_PROVIDER *p = SwHost1xProvider(device);
    KIRQL irql;
    KeAcquireSpinLock(&p->Lock, &irql);
    if (p->Held == request) {
        if (quiesce != STATUS_SUCCESS) p->Faulted = TRUE;
        p->Held = NULL;
    }
    KeReleaseSpinLock(&p->Lock, irql);
}

static VOID NTAPI SwHost1xStopWorker(PDEVICE_OBJECT device, PVOID context)
{
    SW_HOST1X_REQUEST *c = context;
    WDFREQUEST request = c->Request;
    WDFDEVICE owner = c->Device;
    PIO_WORKITEM work = c->Work;
    UNREFERENCED_PARAMETER(device);
    /* Cancellation can arrive while the acquire callback publishes Ready. */
    (VOID)KeWaitForSingleObject(&c->Published, Executive, KernelMode, FALSE, NULL);
    c->Op->QuiesceStatus = c->Op->Quiesce(c->Op->Context);
    SwHost1xDropClaim(owner, request, c->Op->QuiesceStatus);
    if (c->PowerReference) WdfDeviceResumeIdle(owner);
    /* A failed callback is reported verbatim. It does not claim retirement
     * and must not gate the inherited clock or release consumer DMA memory. */
    WdfRequestCompleteWithInformation(request, c->CompletionStatus, sizeof(*c->Op));
    WdfObjectDereference(request);
    IoFreeWorkItem(work);
    WdfObjectDereference(owner);
}

static VOID SwHost1xQueueStop(WDFREQUEST request, NTSTATUS status)
{
    SW_HOST1X_REQUEST *c = SwHost1xRequest(request);
    if (InterlockedCompareExchange(&c->WorkQueued, 1, 0) != 0) return;
    c->CompletionStatus = status;
    /* EvtRequestCancel may run at DISPATCH_LEVEL. The preallocated work item
     * invokes the consumer only at PASSIVE_LEVEL, without provider locks. */
    IoQueueWorkItem(c->Work, SwHost1xStopWorker, DelayedWorkQueue, c);
}

static VOID SwHost1xCancel(WDFREQUEST request)
{
    SwHost1xQueueStop(request, STATUS_CANCELLED);
}

static VOID SwHost1xStop(WDFQUEUE queue, WDFREQUEST request, ULONG flags)
{
    SW_HOST1X_REQUEST *c = SwHost1xRequest(request);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(queue);
    UNREFERENCED_PARAMETER(flags);
    WdfObjectReference(request);
    if (InterlockedCompareExchange(&c->WorkQueued, 0, 0) == 0) {
        status = WdfRequestUnmarkCancelable(request);
        /* STATUS_CANCELLED transfers completion to EvtRequestCancel. */
        if (status == STATUS_SUCCESS)
            SwHost1xQueueStop(request, STATUS_DEVICE_NOT_READY);
    }
    WdfObjectDereference(request);
    /* The outstanding request is completed by the passive stop worker;
     * do not acknowledge a power transition before that callback finishes. */
}

static VOID SwHost1xAcquire(WDFQUEUE queue, WDFREQUEST request,
                           size_t out_bytes, size_t in_bytes, ULONG code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    PSWFAN_DEVICE dev = SwFanGetDevice(device);
    SW_HOST1X_PROVIDER *p = SwHost1xProvider(device);
    SW_HOST1X_REQUEST *c = NULL;
    TG_HOST1X_CLOCK_REQUEST *in, *op = NULL, copy;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    ULONG failure_stage = 2;
    KIRQL irql;
    BOOLEAN claimed = FALSE;
    if (code != TG_HOST1X_IOCTL_CLOCK_HOLD ||
        WdfRequestGetRequestorMode(request) != KernelMode) {
        WdfRequestComplete(request, STATUS_ACCESS_DENIED); return;
    }
    if (in_bytes != sizeof(copy) || out_bytes != sizeof(copy)) {
        SwHost1xFailure(2, STATUS_INFO_LENGTH_MISMATCH);
        WdfRequestComplete(request, STATUS_INFO_LENGTH_MISMATCH); return;
    }
    status = WdfRequestRetrieveInputBuffer(request, sizeof(copy), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) { SwHost1xFailure(3, status); WdfRequestComplete(request, status); return; }
    copy = *in;
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(copy), (PVOID *)&op, NULL);
    if (!NT_SUCCESS(status)) { SwHost1xFailure(4, status); WdfRequestComplete(request, status); return; }
    if (copy.Version != TG_HOST1X_CLOCK_ABI || copy.Size != sizeof(copy) ||
        copy.Reserved || copy.CoreBootFloorUv || copy.CoreAppliedUv ||
        !copy.Ready || !copy.Quiesce) {
        SwHost1xFailure(5, STATUS_INVALID_PARAMETER);
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
    }
    *op = copy;
    op->ReadyStatus = STATUS_DEVICE_NOT_READY;
    op->QuiesceStatus = STATUS_SUCCESS; /* no work admitted yet */
    failure_stage = 6;
    if (!dev->IsLite || dev->Stopping || !dev->CpuConfigured || !dev->Car) {
        status = STATUS_DEVICE_NOT_READY; goto fail;
    }
    KeAcquireSpinLock(&p->Lock, &irql);
    if (!p->Held && !p->Faulted) { p->Held = request; claimed = TRUE; }
    status = p->Faulted ? STATUS_DEVICE_NOT_READY : STATUS_DEVICE_BUSY;
    KeReleaseSpinLock(&p->Lock, irql);
    failure_stage = 7;
    if (!claimed) goto fail;
    failure_stage = 8;
    op->GateL = SwCpuRead(dev, 0x10);
    op->ResetL = SwCpuRead(dev, 0x04);
    op->PllpBase = SwCpuRead(dev, 0xa0);
    if (op->GateL == MAXULONG || op->ResetL == MAXULONG || op->PllpBase == MAXULONG ||
        !(op->GateL & (1u << 28)) || (op->ResetL & (1u << 28)) ||
        (op->PllpBase & ((1u << 31) | (1u << 30) | (1u << 27))) !=
            ((1u << 30) | (1u << 27))) {
        status = STATUS_DEVICE_NOT_READY; goto fail;
    }
    failure_stage = 13;
    status = SwHost1xReadCoreFloor(device, op);
    if (status != STATUS_SUCCESS) goto fail;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SW_HOST1X_REQUEST);
    failure_stage = 9;
    status = WdfObjectAllocateContext(request, &attributes, (PVOID *)&c);
    if (!NT_SUCCESS(status)) goto fail;
    c->Device = device; c->Request = request; c->Op = op;
    KeInitializeEvent(&c->Published, NotificationEvent, FALSE);
    failure_stage = 10;
    c->Work = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(device));
    if (!c->Work) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    /* Already dispatched from a power-managed queue: never wait for D0 here.
     * The explicit reference covers S0 idle; EvtIoStop handles Sx/DFx. */
    failure_stage = 11;
    status = WdfDeviceStopIdle(device, FALSE);
    c->PowerReference = NT_SUCCESS(status);
    /* KMDF before1.33 may return PENDING while a system-managed idle device
     * is still in D0. This callback itself is dispatched by a power-managed
     * queue, so D0 is already established; the pending result owns a power
     * reference too. It is not a substitute for waiting for hardware DMA. */
    if (status != STATUS_SUCCESS && status != STATUS_PENDING) {
        if (NT_SUCCESS(status)) status = STATUS_DEVICE_NOT_READY;
        goto fail;
    }
    WdfObjectReference(device);
    WdfObjectReference(request);
    failure_stage = 12;
    status = WdfRequestMarkCancelableEx(request, SwHost1xCancel);
    if (!NT_SUCCESS(status)) {
        WdfObjectDereference(request); WdfObjectDereference(device); goto fail;
    }
    WdfObjectReference(request); /* publication callback vs. stop-worker completion */
    op->ReadyStatus = STATUS_SUCCESS;
    KeSetEvent(op->Ready, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&c->Published, IO_NO_INCREMENT, FALSE);
    WdfObjectDereference(request);
    return; /* hold the real request and its power reference */
fail:
    SwHost1xFailure(failure_stage, status);
    if (c && c->PowerReference) WdfDeviceResumeIdle(device);
    if (c && c->Work) IoFreeWorkItem(c->Work);
    if (claimed) SwHost1xDropClaim(device, request, STATUS_SUCCESS); /* no work admitted */
    op->ReadyStatus = status;
    KeSetEvent(op->Ready, IO_NO_INCREMENT, FALSE);
    WdfRequestCompleteWithInformation(request, status, sizeof(*op));
}

static NTSTATUS SwFanHost1xCreateQueue(WDFDEVICE device)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG config;
    SW_HOST1X_PROVIDER *p;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SW_HOST1X_PROVIDER);
    status = WdfObjectAllocateContext(device, &attributes, (PVOID *)&p);
    if (!NT_SUCCESS(status)) return status;
    KeInitializeSpinLock(&p->Lock);
    SwFanTrace(L"Host1xFailureStage", 0);
    SwFanTrace(L"Host1xFailureStatus", 0);
    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchParallel);
    config.PowerManaged = WdfTrue;
    config.EvtIoDeviceControl = SwHost1xAcquire;
    config.EvtIoStop = SwHost1xStop;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    return WdfIoQueueCreate(device, &config, &attributes, &p->Queue);
}

static VOID SwFanHost1xDispatch(WDFDEVICE device, WDFREQUEST request)
{
    SW_HOST1X_PROVIDER *p = SwHost1xProvider(device);
    NTSTATUS status = WdfRequestForwardToIoQueue(request, p->Queue);
    if (!NT_SUCCESS(status)) {
        if (WdfRequestGetRequestorMode(request) == KernelMode)
            SwHost1xFailure(1, status);
        WdfRequestComplete(request, status);
    }
}
#endif
