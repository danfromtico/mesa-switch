/* SPDX-License-Identifier: MIT */
/* Reuse the WDK bridge fixture's tracked pool/IRPs and unload-safe completion
 * simulation. These are CPU controls, not a real WDF power or DMA test. */
#include <initguid.h>
#define TG_PLATFORM_FIXTURE_ONLY
#define __imp_ExAllocatePool2 unused_platform_alloc
#define __imp_ExFreePoolWithTag unused_platform_free
#define __imp_KeWaitForSingleObject unused_platform_wait
#define __imp_IoCancelIrp unused_platform_cancel
#define __imp_IofCallDriver unused_platform_call
#include "test_platform_windows.c"
#undef TG_PLATFORM_FIXTURE_ONLY
#undef __imp_ExAllocatePool2
#undef __imp_ExFreePoolWithTag
#undef __imp_KeWaitForSingleObject
#undef __imp_IoCancelIrp
#undef __imp_IofCallDriver
#include "../windows/host1x_clock.h"

enum { LeaseReady, LeaseNoReady, LeaseOldProvider, LeaseStopBeforeAdmission,
       LeasePendingReady, LeaseBadGate, LeaseNoCoreFloor, LeaseCoreUndervolt,
       LeaseCoreUnaligned, LeaseBadVersion };
static struct {
    UINT mode, quiesces;
    NTSTATUS quiesce_status;
    BOOLEAN defer_cancel, short_completion;
    PIRP pending;
} clock_model;

NTSTATUS TgGpuReferencePlatformBridge(TG_GPU_PLATFORM *p, PFILE_OBJECT *file,
                                     PDEVICE_OBJECT *owner)
{
    CHECK(p == &platform);
    *file = NULL; *owner = NULL;
    if (!NT_SUCCESS(model.open)) return model.open;
    *file = &fake_file; *owner = &fake_owner_device;
    (VOID)RefObj(*file); (VOID)RefObj(*owner);
    return STATUS_SUCCESS;
}

static PVOID NTAPI ClockAlloc(POOL_FLAGS flags, SIZE_T bytes, ULONG tag)
{
    CHECK(tag == 0x6b436754u);
    return AllocPool2(flags, bytes, 0x50674754u);
}
static VOID NTAPI ClockFree(PVOID p, ULONG tag)
{
    CHECK(tag == 0x6b436754u);
    FreePoolTag(p, 0x50674754u);
}
static NTSTATUS NTAPI ClockWait(PVOID object, KWAIT_REASON reason,
    KPROCESSOR_MODE mode, BOOLEAN alert, PLARGE_INTEGER timeout)
{
    CHECK(model.irql == PASSIVE_LEVEL && reason == Executive && mode == KernelMode &&
          !alert && timeout && timeout->QuadPart == -10000000LL);
    ++model.waits;
    return ((PKEVENT)object)->Header.SignalState ? STATUS_SUCCESS : STATUS_TIMEOUT;
}
static NTSTATUS NTAPI ConsumerQuiesce(PVOID context)
{
    CHECK(context == &clock_model && model.irql == PASSIVE_LEVEL);
    CHECK(model.live_pool == 1 && model.live_irps == 1 && model.file_refs == 1);
    ++clock_model.quiesces;
    return clock_model.quiesce_status;
}
static VOID FinishLease(void)
{
    PIRP irp = clock_model.pending;
    TG_HOST1X_CLOCK_REQUEST *op;
    CHECK(irp != NULL);
    op = irp->AssociatedIrp.SystemBuffer;
    op->QuiesceStatus = op->Quiesce(op->Context);
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = clock_model.short_completion ? 0 : sizeof(*op);
    clock_model.pending = NULL;
    complete_irp(irp);
}
static BOOLEAN NTAPI ClockCancel(PIRP irp)
{
    ++model.cancels;
    if (!clock_model.pending) return FALSE;
    CHECK(irp == clock_model.pending);
    irp->Cancel = TRUE;
    if (!clock_model.defer_cancel) FinishLease();
    return TRUE;
}
static NTSTATUS FASTCALL ClockCall(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetNextIrpStackLocation(irp);
    TG_HOST1X_CLOCK_REQUEST *op = irp->AssociatedIrp.SystemBuffer;
    CHECK(device == &fake_target_device && irp->RequestorMode == KernelMode);
    CHECK(stack->FileObject == &fake_file && stack->MajorFunction == IRP_MJ_DEVICE_CONTROL);
    CHECK(stack->Parameters.DeviceIoControl.IoControlCode == TG_HOST1X_IOCTL_CLOCK_HOLD);
    CHECK(stack->Parameters.DeviceIoControl.InputBufferLength == sizeof(*op) &&
          stack->Parameters.DeviceIoControl.OutputBufferLength == sizeof(*op));
    CHECK(op->Version == TG_HOST1X_CLOCK_ABI && op->Size == sizeof(*op) &&
          op->Ready && op->Quiesce && op->Context && !op->Reserved);
    if (clock_model.mode == LeaseOldProvider) {
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        complete_irp(irp);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    clock_model.pending = irp;
    if (clock_model.mode != LeaseNoReady) {
        op->PllpBase = 0x48115508u;
        op->GateL = 1u << 28; op->ResetL = 0;
        op->ReadyStatus = STATUS_SUCCESS;
        op->CoreBootFloorUv = op->CoreAppliedUv = 775000u;
        if (clock_model.mode == LeasePendingReady) op->ReadyStatus = STATUS_PENDING;
        if (clock_model.mode == LeaseBadGate) op->GateL = 0;
        if (clock_model.mode == LeaseNoCoreFloor) op->CoreBootFloorUv = 0;
        if (clock_model.mode == LeaseCoreUndervolt) op->CoreAppliedUv = 650000u;
        if (clock_model.mode == LeaseCoreUnaligned) op->CoreBootFloorUv = 774999u;
        if (clock_model.mode == LeaseBadVersion) op->Version = 0x00010001u;
        SetEvent(op->Ready, IO_NO_INCREMENT, FALSE);
    }
    if (clock_model.mode == LeaseStopBeforeAdmission) FinishLease();
    return STATUS_PENDING;
}

PVOID (NTAPI * const __imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=ClockAlloc;
VOID (NTAPI * const __imp_ExFreePoolWithTag)(PVOID,ULONG)=ClockFree;
NTSTATUS (NTAPI * const __imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=ClockWait;
BOOLEAN (NTAPI * const __imp_IoCancelIrp)(PIRP)=ClockCancel;
NTSTATUS (FASTCALL * const __imp_IofCallDriver)(PDEVICE_OBJECT,PIRP)=ClockCall;

static VOID ClockSetup(void)
{
    Setup();
    memset(&clock_model, 0, sizeof(clock_model));
    fake_driver.DeviceObject = &fake_owner_device;
}
static VOID NoClockObjects(void)
{
    CHECK(!model.live_pool && !model.live_irps && !model.file_refs && !model.owner_refs);
}
static BOOLEAN StageIs(const char *actual, const char *expected)
{
    if (!actual) return FALSE;
    while (*actual && *actual == *expected) { ++actual; ++expected; }
    return *actual == *expected;
}
void start(void)
{
    TG_HOST1X_CLOCK_LEASE *lease = NULL;
    const char *stage = NULL;
    ULONG pllp = 0, core_floor = 0, written;
    ClockSetup();
    CHECK(TgHost1xClockAcquire(&platform, NULL, &clock_model, &lease, &stage) == STATUS_INVALID_PARAMETER);
    model.open = STATUS_DEVICE_NOT_READY;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_DEVICE_NOT_READY);
    CHECK(StageIs(stage,"Host1x bridge reference"));
    CHECK(!lease); NoClockObjects();

    ClockSetup(); model.completion_registration = STATUS_INSUFFICIENT_RESOURCES;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!lease); NoClockObjects();

    CHECK(StageIs(stage,"Host1x completion registration"));

    ClockSetup(); clock_model.mode = LeaseOldProvider;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_INVALID_DEVICE_REQUEST);
    CHECK(StageIs(stage,"Host1x IRP completed before admission"));
    CHECK(lease && !TgHost1xClockHeld(lease, NULL, NULL) && !clock_model.quiesces);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease); NoClockObjects();

    for (UINT bad = LeasePendingReady; bad <= LeaseBadVersion; ++bad) {
        ClockSetup(); clock_model.mode = bad;
        CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_DEVICE_PROTOCOL_ERROR);
        CHECK(!TgHost1xClockHeld(lease, NULL, NULL));
        CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease && !clock_model.quiesces);
        NoClockObjects();
    }

    ClockSetup(); clock_model.mode = LeaseStopBeforeAdmission;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_CANCELLED);
    CHECK(!clock_model.quiesces && !TgHost1xClockHeld(lease, NULL, NULL));
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease); NoClockObjects();

    ClockSetup();
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_SUCCESS);
    CHECK(TgHost1xClockHeld(lease, &pllp, &core_floor) && pllp == 0x48115508u && core_floor == 775000u);
    CHECK(model.live_pool == 1 && model.live_irps == 1 && model.owner_refs == 1);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease && clock_model.quiesces == 1);
    NoClockObjects();

    ClockSetup(); clock_model.mode = LeaseNoReady; clock_model.defer_cancel = TRUE;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_IO_TIMEOUT);
    CHECK(lease && model.live_pool == 1 && model.live_irps == 1);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_IO_TIMEOUT && lease && !TgHost1xClockHeld(lease, NULL, NULL));
    FinishLease();
    CHECK(!clock_model.quiesces); /* hardware was never admitted */
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease); NoClockObjects();

    ClockSetup(); clock_model.defer_cancel = TRUE;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_SUCCESS);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_IO_TIMEOUT && lease && !TgHost1xClockHeld(lease, NULL, NULL));
    FinishLease();
    CHECK(clock_model.quiesces == 1);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease); NoClockObjects();

    ClockSetup(); clock_model.quiesce_status = STATUS_DEVICE_BUSY;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_SUCCESS);
    FinishLease(); /* provider power-down before the explicit release */
    CHECK(!TgHost1xClockHeld(lease, NULL, NULL) && clock_model.quiesces == 1);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_DEVICE_BUSY && lease);
    CHECK(model.live_pool == 1 && model.live_irps == 1 && model.file_refs == 1);
    clock_model.quiesce_status = STATUS_SUCCESS;
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease && clock_model.quiesces == 2);
    NoClockObjects();

    ClockSetup(); clock_model.quiesce_status = STATUS_PENDING;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_SUCCESS);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_DEVICE_PROTOCOL_ERROR && lease);
    clock_model.quiesce_status = STATUS_SUCCESS;
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease && clock_model.quiesces == 2);
    NoClockObjects();

    ClockSetup(); clock_model.short_completion = TRUE;
    CHECK(TgHost1xClockAcquire(&platform, ConsumerQuiesce, &clock_model, &lease, &stage) == STATUS_SUCCESS);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_DEVICE_PROTOCOL_ERROR && lease);
    CHECK(TgHost1xClockRelease(&lease) == STATUS_SUCCESS && !lease && clock_model.quiesces == 2);
    NoClockObjects();
    static const char pass[] = "PASS: pending clock IRP admission/cancel/timeout/failure retention (CPU only)\r\n";
    WriteFile(GetStdHandle((ULONG)-11), pass, sizeof(pass)-1, &written, NULL);
    ExitProcess(0);
}
