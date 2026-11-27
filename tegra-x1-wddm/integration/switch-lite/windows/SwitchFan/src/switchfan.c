/* Shared Nintendo Switch fan/backlight driver. T214/HDH-001 is detected
 * from its dedicated PWM1 resource. New Lite firmware can additionally
 * expose PWM0 for backlight control. Clock/sensor setup is model-specific.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include "t214/performance.h"
#include "backlight.h"

#define PWM_CSR_0        0x00
#define IOCTL_SWBL_GET   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS)    /* 0x00226040 */
#define IOCTL_SWBL_SET   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_WRITE_ACCESS)   /* 0x0022A044 */

/* GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed. */
static const GUID SwFanDisplayStateGuid =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

typedef struct _SWBL_STATUS {
    ULONG Version;          /* 2 */
    ULONG Brightness;       /* user level, 8..255 */
    ULONG Duty;             /* what CSR_0 holds now */
    ULONG DisplayState;     /* 0 off, 1 on, 2 dimmed */
} SWBL_STATUS;

#define PWM_CSR_1        0x00  /* ACPI maps only 0x7000A010..13 */
#define PWM_CSR_EN       (1u << 31)
#define PWM_DUTY_MAX     236u
#define PWM_ABSOLUTE_OFF (0x100u << 16)
#define TMP451_LOCAL     0x00
#define TMP451_REMOTE    0x01
#define POLL_MS          2000
#define FAILSAFE_DUTY    153u

typedef struct _SWFAN_DEVICE {
    volatile UCHAR *Pwm;
    ULONG PwmLength;
    ULONG FanOffset;
    BOOLEAN IsLite;
    volatile UCHAR *BacklightPwm;
    ULONG BacklightLength;  /* zero when this aliases the Erista PWM map */
    BOOLEAN BacklightReady;
    WDFWAITLOCK BacklightLock;
    PVOID DisplayCallback;
    ULONG Brightness, DisplayState;
    volatile UCHAR *Car;
    LITE_PERFORMANCE Config;
    LARGE_INTEGER PmicIds[2];
    ULONG ConnectionCount;
    WDFIOTARGET Pmic;
    ULONG BootPllx, BootCpuKhz;
    UCHAR BootVout;
    BOOLEAN CpuConfigured, CpuApplied, CpuThrottled, CpuOverclockEnabled;
    LARGE_INTEGER SensorId;
    WDFIOTARGET Sensor;
    WDFTIMER Timer;
    volatile BOOLEAN Stopping;
    ULONG Duty;
    ULONG Failures;
    ULONG LastTemp;
    BOOLEAN TempValid;
    LONG Runs;
} SWFAN_DEVICE, *PSWFAN_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWFAN_DEVICE, SwFanGetDevice)

typedef struct _SWFAN_TIMER {
    WDFDEVICE Device;
} SWFAN_TIMER, *PSWFAN_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWFAN_TIMER, SwFanGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwFanEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwFanEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwFanEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY SwFanEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT SwFanEvtD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwFanEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwFanEvtSelfManagedIoCleanup;
EVT_WDF_TIMER SwFanEvtTimer;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND SwFanEvtSuspend;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART SwFanEvtRestart;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwFanEvtIoDeviceControl;
POWER_SETTING_CALLBACK SwFanDisplayStateCallback;

static WCHAR SwFanParamsPath[256];

static VOID SwFanTrace(PCWSTR Name, ULONG Value)
{
    if (SwFanParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwFanParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwFanOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
{
    WCHAR PathBuffer[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, Target);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(*Target, &Open);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return Status;
}

static NTSTATUS SwFanReadReg(WDFIOTARGET Target, UCHAR Register, PUCHAR Value)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Register, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Value, 1);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 2) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/* Same TMP451 configuration as Hekate: normal range, Mariko -12.5 C
 * remote offset, 4-second conversion period. All transfers run at PASSIVE_LEVEL. */
static NTSTATUS SwFanConfigureSensor(WDFIOTARGET Target)
{
    static const UCHAR Setup[][2] = {{0x09, 0x80}, {0x11, 0xF3}, {0x12, 0x80}, {0x0A, 0x02}};
    ULONG Index;
    for (Index = 0; Index < RTL_NUMBER_OF(Setup); ++Index) {
        UCHAR Buffer[2] = {Setup[Index][0], Setup[Index][1]};
        WDF_MEMORY_DESCRIPTOR Memory;
        WDF_REQUEST_SEND_OPTIONS Options;
        ULONG_PTR Bytes = 0;
        NTSTATUS Status;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
        WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
        Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
        if (!NT_SUCCESS(Status)) return Status;
        if (Bytes != sizeof(Buffer)) return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return STATUS_SUCCESS;
}

#include "t214/clock.h"
#ifdef SWITCH_LITE_GPU
#include "t214/gpu_pmic.h"
#endif

/* ------------------------------------------------------------------- fan */

static VOID SwFanSetDuty(PSWFAN_DEVICE Dev, ULONG Duty)
{
    ULONG Csr;
    if (Duty > PWM_DUTY_MAX) {
        Duty = PWM_DUTY_MAX;
    }
    Csr = (Duty == 0) ? (PWM_CSR_EN | PWM_ABSOLUTE_OFF) : (PWM_CSR_EN | ((PWM_DUTY_MAX - Duty) << 16));
    WRITE_REGISTER_ULONG((PULONG)(Dev->Pwm + Dev->FanOffset), Csr);
    Dev->Duty = Duty;
}

static ULONG SwFanDutyForTemp(ULONG Temp, ULONG Previous)
{
    if (Temp >= 45) return 236;
    if (Temp >= 43) return 212;
    if (Temp >= 40) return 177;
    if (Temp >= 38) return 118;
    if (Temp >= 35) return 76;
    if (Temp <= 32) return 0;
    return (Previous > 76) ? 76 : Previous;    /* 33-34 C: hold (at most the 35 C step) */
}

VOID SwFanEvtTimer(WDFTIMER Timer)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(SwFanGetTimer(Timer)->Device);
    UCHAR Local = 0, Remote = 0;
    NTSTATUS Status, ClockStatus = STATUS_SUCCESS;
    BOOLEAN ScreenOff, ClockRestored;
    ULONG DisplayState, BacklightDuty;

    if (Dev->Stopping) return;
    WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    ScreenOff = Dev->IsLite && Dev->BacklightReady && Dev->DisplayState == 0;
    DisplayState = Dev->DisplayState;
    BacklightDuty = SwBacklightDuty(Dev->Brightness, DisplayState);
    WdfWaitLockRelease(Dev->BacklightLock);
    SwFanTrace(L"DisplayState", DisplayState);
    SwFanTrace(L"BacklightDuty", BacklightDuty);
    /* Screen-off can remain in S0. Keep temperature polling, but release the
       optional CPU overclock before allowing the fan to stop. */
    if (ScreenOff) {
        ClockStatus = SwCpuApply(Dev, FALSE);
        SwFanTrace(L"CpuLastError", (ULONG)ClockStatus);
    }
    ClockRestored = Dev->CpuConfigured && !Dev->CpuApplied &&
                    NT_SUCCESS(ClockStatus) && SwCpuKhz(Dev) == Dev->BootCpuKhz;
    SwFanTrace(L"ScreenOffCooling", ScreenOff);
    Status = SwFanReadReg(Dev->Sensor, TMP451_REMOTE, &Remote);
    if (NT_SUCCESS(Status)) {
        Status = SwFanReadReg(Dev->Sensor, TMP451_LOCAL, &Local);
    }
    if (NT_SUCCESS(Status)) {
        ULONG Temp = (Remote > Local) ? Remote : Local;
        Dev->LastTemp = Temp;
        Dev->TempValid = TRUE;
        Dev->Failures = 0;
        ULONG Duty = SwFanDutyForTemp(Temp, Dev->Duty);
        // Retain the overclock floor until stock CPU clock is confirmed. On
        // wake, restore cooling before SwCpuTemperature can reapply overclock.
        if (Dev->IsLite && Dev->Config.CpuKhz == LITE_CPU_KHZ &&
            ((!ScreenOff && Dev->CpuOverclockEnabled) || !ClockRestored) && Duty < 51) Duty = 51;
        SwFanSetDuty(Dev, Duty);
        if (!ScreenOff) SwCpuTemperature(Dev, Temp, TRUE);
        SwFanTrace(L"TempSoC", Remote);
        SwFanTrace(L"TempBoard", Local);
    } else {
        Dev->TempValid = FALSE;
        SwFanTrace(L"LastSensorError", (ULONG)Status);
        /* A stopped fan needs cooling on the first lost temperature sample. */
        if (++Dev->Failures >= 3 || Dev->Duty == 0) {
            SwFanSetDuty(Dev, FAILSAFE_DUTY);
            SwCpuTemperature(Dev, 0, FALSE);
        }
    }
    SwFanTrace(L"Duty", Dev->Duty);
    SwFanTrace(L"SensorFailures", Dev->Failures);
    if (Dev->Car != NULL) SwFanTrace(L"CpuActualKhz", SwCpuKhz(Dev));
    SwFanTrace(L"Runs", (ULONG)InterlockedIncrement(&Dev->Runs));
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot passive timer, re-armed */
    }
}

/* ------------------------------------------------------------- backlight */

static ULONG SwFanReadParam(PCWSTR Name, ULONG Default)
{
    ULONG Value = Default;
    RTL_QUERY_REGISTRY_TABLE Table[2];

    if (SwFanParamsPath[0] == L'\0') {
        return Default;
    }
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwFanParamsPath, Table, NULL, NULL))) {
        Value = Default;
    }
    return Value;
}

/* CSR_0 from the brightness and the display state. Caller holds BacklightLock. */
static ULONG SwFanApplyBacklight(PSWFAN_DEVICE Dev)
{
    ULONG Duty = SwBacklightDuty(Dev->Brightness, Dev->DisplayState);
    if (Dev->BacklightPwm != NULL) {
        PULONG Csr = (PULONG)(Dev->BacklightPwm + PWM_CSR_0);
        WRITE_REGISTER_ULONG(Csr, SwBacklightCsr(READ_REGISTER_ULONG(Csr), Duty));
    }
    return Duty;
}

NTSTATUS SwFanDisplayStateCallback(LPCGUID Setting, PVOID Value, ULONG Length, PVOID Context)
{
    PSWFAN_DEVICE Dev = (PSWFAN_DEVICE)Context;
    if (Length >= sizeof(ULONG) && IsEqualGUID(Setting, &SwFanDisplayStateGuid)) {
        WdfWaitLockAcquire(Dev->BacklightLock, NULL);
        Dev->DisplayState = *(PULONG)Value;
        /* Power notification must not wait for registry/storage I/O. The
           existing temperature timer records this snapshot after the lock. */
        (VOID)SwFanApplyBacklight(Dev);
        WdfWaitLockRelease(Dev->BacklightLock);
    }
    return STATUS_SUCCESS;
}

VOID SwFanEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Info = 0;
    ULONG SavedBrightness = 0;
    UNREFERENCED_PARAMETER(OutLength);
    UNREFERENCED_PARAMETER(InLength);

#ifdef SWITCH_LITE_GPU
    if (Code == TG_GPU_IOCTL_PMIC) {
        Status = SwFanGpuPmic(Dev, Request, &Info);
        WdfRequestCompleteWithInformation(Request, Status, Info);
        return;
    }
#endif
    WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    if (Dev->BacklightPwm == NULL) {
        Status = STATUS_NOT_SUPPORTED;
    } else if (Code == IOCTL_SWBL_GET) {
        SWBL_STATUS *Out;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, NULL);
        if (NT_SUCCESS(Status)) {
            Out->Version = 2;
            Out->Brightness = Dev->Brightness;
            Out->Duty = (READ_REGISTER_ULONG((PULONG)(Dev->BacklightPwm + PWM_CSR_0)) >> 16) & 0x7FFF;
            Out->DisplayState = Dev->DisplayState;
            Info = sizeof(*Out);
        }
    } else if (Code == IOCTL_SWBL_SET) {
        PULONG In;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), (PVOID *)&In, NULL);
        if (NT_SUCCESS(Status)) {
            ULONG Level = *In;
            Dev->Brightness = SwBacklightClamp(Level);
            SavedBrightness = Dev->Brightness;
            (VOID)SwFanApplyBacklight(Dev);
        }
    }
    WdfWaitLockRelease(Dev->BacklightLock);
    /* The sequential queue preserves setter order without holding the
       display-notification lock while a registry hive is written. */
    if (SavedBrightness != 0) SwFanTrace(L"Brightness", SavedBrightness);
    WdfRequestCompleteWithInformation(Request, Status, Info);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwFanEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    Dev->TempValid = FALSE;
    /* Cooling resumes before the passive timer can restore CPU overclock. */
    if (Dev->IsLite && Dev->Pwm != NULL) SwFanSetDuty(Dev, 76);
    SwFanTrace(L"SleepFanOff", 0);
    return STATUS_SUCCESS;
}

NTSTATUS SwFanEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    POWER_ACTION Action = WdfDeviceGetSystemPowerAction(Device);
    BOOLEAN Sleeping = Action == PowerActionSleep || Action == PowerActionHibernate;
    BOOLEAN Stock = Dev->Config.CpuKhz != LITE_CPU_KHZ ||
                    (Dev->CpuConfigured && !Dev->CpuApplied && SwCpuKhz(Dev) == Dev->BootCpuKhz);
    /* SelfManagedIoSuspend has drained the timer and released CPU overclock.
       Device removal, shutdown and S0 device idle keep the existing cooling.
       Only a cool system sleep transition can stop polling with the fan off. */
    if (Dev->IsLite && Dev->Pwm != NULL && TargetState != WdfPowerDeviceD3Final && Sleeping && Stock &&
        Dev->TempValid && Dev->LastTemp <= 39) {
        SwFanSetDuty(Dev, 0);
        SwFanTrace(L"SleepFanOff", 1);
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwFanEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    BOOLEAN HaveSensor = FALSE;
    Dev->ConnectionCount = 0;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc == NULL) {
            continue;
        }
        if (Desc->Type == CmResourceTypeMemory) {
            if (Desc->u.Memory.Start.QuadPart == 0x7000A010 && Desc->u.Memory.Length == sizeof(ULONG)) {
                Dev->Pwm = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
                if (Dev->Pwm == NULL) return STATUS_INSUFFICIENT_RESOURCES;
                Dev->PwmLength = sizeof(ULONG);
                Dev->FanOffset = 0; Dev->IsLite = TRUE;
            } else if (Desc->u.Memory.Start.QuadPart == 0x7000A000 && Desc->u.Memory.Length == sizeof(ULONG)) {
                Dev->BacklightPwm = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
                if (Dev->BacklightPwm == NULL) return STATUS_INSUFFICIENT_RESOURCES;
                Dev->BacklightLength = sizeof(ULONG);
            } else if (Desc->u.Memory.Start.QuadPart == 0x7000A000 && Desc->u.Memory.Length >= 0x20) {
                Dev->Pwm = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
                if (Dev->Pwm == NULL) return STATUS_INSUFFICIENT_RESOURCES;
                Dev->PwmLength = Desc->u.Memory.Length;
                Dev->FanOffset = 0x10; Dev->IsLite = FALSE;
            } else if (Desc->u.Memory.Start.QuadPart == 0x60006000 && Desc->u.Memory.Length == 0x100) {
                Dev->Car = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, 0x100, PAGE_READWRITE | PAGE_NOCACHE);
                if (Dev->Car == NULL) return STATUS_INSUFFICIENT_RESOURCES;
            } else if (Desc->u.Memory.Start.QuadPart == LITE_PERF_ADDRESS && Desc->u.Memory.Length == 0x1000) {
                volatile ULONG *Map = (volatile ULONG *)MmMapIoSpaceEx(Desc->u.Memory.Start, 0x1000, PAGE_READONLY | PAGE_NOCACHE);
                ULONG I;
                if (Map == NULL) return STATUS_INSUFFICIENT_RESOURCES;
                for (I = 0; I < sizeof(Dev->Config)/sizeof(ULONG); ++I)
                    ((PULONG)&Dev->Config)[I] = READ_REGISTER_ULONG((PULONG)&Map[I]);
                MmUnmapIoSpace((PVOID)Map, 0x1000);
                if (Dev->Config.Magic != LITE_PERF_MAGIC || Dev->Config.Version != LITE_PERF_VERSION)
                    RtlZeroMemory(&Dev->Config, sizeof(Dev->Config));
            }
        } else if (Desc->Type == CmResourceTypeConnection &&
                   Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                   Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            LARGE_INTEGER Id;
            Id.LowPart = Desc->u.Connection.IdLowPart;
            Id.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            if (Dev->ConnectionCount == 0) { Dev->SensorId = Id; HaveSensor = TRUE; }
            else if (Dev->ConnectionCount <= 2) Dev->PmicIds[Dev->ConnectionCount - 1] = Id;
            ++Dev->ConnectionCount;
        }
    }
    if (Dev->Pwm == NULL || !HaveSensor) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!Dev->IsLite) Dev->BacklightPwm = Dev->Pwm;
    /* Read once per hardware start so a diagnostic does not introduce another
       live clock transition. Zero keeps the inherited CPU clock across lock. */
    Dev->CpuOverclockEnabled = SwFanReadParam(L"CpuOverclockEnabled", 1) != 0;
    SwFanTrace(L"CpuOverclockEnabled", Dev->CpuOverclockEnabled);
    SwFanTrace(L"BacklightAvailable", Dev->BacklightPwm != NULL);
    SwFanTrace(L"CpuRequestedKhz", Dev->Config.CpuKhz);
    SwFanTrace(L"RamRequestedKhz", Dev->Config.RamKhz);
    SwFanTrace(L"RamActualKhz", Dev->Config.RamActualKhz);
    SwFanTrace(L"HandoffStatus", Dev->Config.Status);
    SwFanTrace(L"PmicAddress", Dev->Config.PmicAddress);
    SwFanTrace(L"CoreMv", Dev->Config.CoreMv);
    SwFanTrace(L"BpmpError", Dev->Config.BpmpError);
    SwFanTrace(L"BpmpChannel", Dev->Config.BpmpChannel);
    SwFanTrace(L"CsrAtStart", READ_REGISTER_ULONG((PULONG)(Dev->Pwm + Dev->FanOffset)));
    return STATUS_SUCCESS;
}

NTSTATUS SwFanEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    /* Power notifications run outside the WDF queue: drain them before unmap. */
    if (Dev->DisplayCallback != NULL) {
        (VOID)PoUnregisterPowerSettingCallback(Dev->DisplayCallback);
        Dev->DisplayCallback = NULL;
    }
    WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    if (Dev->BacklightReady) {
        Dev->DisplayState = 1;
        (VOID)SwFanApplyBacklight(Dev);
    }
    if (Dev->BacklightLength != 0 && Dev->BacklightPwm != NULL)
        MmUnmapIoSpace((PVOID)Dev->BacklightPwm, Dev->BacklightLength);
    Dev->BacklightPwm = NULL;
    Dev->BacklightLength = 0;
    Dev->BacklightReady = FALSE;
    WdfWaitLockRelease(Dev->BacklightLock);
    if (Dev->Pwm != NULL) {
        (VOID)SwFanEvtSuspend(Device); /* stop polling before removing the PWM mapping */
        MmUnmapIoSpace((PVOID)Dev->Pwm, Dev->PwmLength);
        Dev->Pwm = NULL;
    }
    if (Dev->Car != NULL) { MmUnmapIoSpace((PVOID)Dev->Car, 0x100); Dev->Car = NULL; }
    return STATUS_SUCCESS;
}

static VOID SwFanStartBacklight(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    NTSTATUS Status;
    ULONG BootCsr, DefaultLevel;
    if (Dev->BacklightPwm == NULL || Dev->BacklightReady) return;
    WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    BootCsr = READ_REGISTER_ULONG((PULONG)(Dev->BacklightPwm + PWM_CSR_0));
    DefaultLevel = Dev->IsLite ? ((BootCsr >> 16) & 0x7FFF) : BACKLIGHT_MAX;
    if (DefaultLevel < BACKLIGHT_MIN || DefaultLevel > BACKLIGHT_MAX) DefaultLevel = 150;
    Dev->Brightness = SwFanReadParam(L"Brightness", DefaultLevel);
    if (Dev->Brightness < BACKLIGHT_MIN || Dev->Brightness > BACKLIGHT_MAX) {
        Dev->Brightness = DefaultLevel;
    }
    Dev->DisplayState = 1;
    Dev->BacklightReady = TRUE;
    SwFanTrace(L"BacklightCsrAtStart", BootCsr);
    SwFanTrace(L"Brightness", Dev->Brightness);
    (VOID)SwFanApplyBacklight(Dev);
    WdfWaitLockRelease(Dev->BacklightLock);
    if (Dev->DisplayCallback == NULL) {
        /* Called once right away with the current state, so not under BacklightLock. */
        Status = PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(Device), &SwFanDisplayStateGuid,
                                                SwFanDisplayStateCallback, Dev, &Dev->DisplayCallback);
        SwFanTrace(L"DisplayCallbackStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->DisplayCallback = NULL;
        }
    }

}

NTSTATUS SwFanEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    NTSTATUS Status;
    /* Backlight first: it must not depend on the temperature sensor. */
    SwFanStartBacklight(Device);
    Status = SwFanOpenTarget(Device, Dev->SensorId, &Dev->Sensor);
    SwFanTrace(L"OpenSensorStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        SwFanSetDuty(Dev, FAILSAFE_DUTY);
        return Status;
    }
    Dev->Duty = 76;                 /* what UEFI R366+ set */
    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwFanEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWFAN_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwFanTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwFanGetTimer(Dev->Timer)->Device = Device;
    }
    return SwFanEvtRestart(Device);
}

NTSTATUS SwFanEvtSuspend(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) WdfTimerStop(Dev->Timer, TRUE);
    (VOID)SwCpuApply(Dev, FALSE);
    if (Dev->Pwm != NULL) SwFanSetDuty(Dev, 76);
    return STATUS_SUCCESS;
}

NTSTATUS SwFanEvtRestart(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    NTSTATUS Status = Dev->IsLite ? SwFanConfigureSensor(Dev->Sensor) : STATUS_SUCCESS;
    SwFanStartBacklight(Device);
    SwFanTrace(L"ConfigureSensorStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        SwFanSetDuty(Dev, FAILSAFE_DUTY);
        return Status;
    }
    if (Dev->IsLite) SwCpuPrepare(Device);
    Dev->Stopping = FALSE;
    Dev->Failures = 0;
    WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(500));
    return STATUS_SUCCESS;
}

VOID SwFanEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    (VOID)SwFanEvtSuspend(Device);
    if (Dev->Pmic != NULL) { WdfObjectDelete(Dev->Pmic); Dev->Pmic = NULL; }
    Dev->CpuConfigured = FALSE;
    if (Dev->DisplayCallback != NULL) {
        (VOID)PoUnregisterPowerSettingCallback(Dev->DisplayCallback);
        Dev->DisplayCallback = NULL;
    }
    if (Dev->BacklightPwm != NULL) {
        WdfWaitLockAcquire(Dev->BacklightLock, NULL);
        Dev->DisplayState = 1;
        (VOID)SwFanApplyBacklight(Dev);
        Dev->BacklightReady = FALSE;
        WdfWaitLockRelease(Dev->BacklightLock);
    }
    if (Dev->Sensor != NULL) {
        WdfObjectDelete(Dev->Sensor);
        Dev->Sensor = NULL;
    }
}

NTSTATUS SwFanEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    DECLARE_CONST_UNICODE_STRING(DeviceName, L"\\Device\\SwitchBacklight");
    DECLARE_CONST_UNICODE_STRING(LinkName, L"\\DosDevices\\SwitchBacklight");
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwFanEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwFanEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry = SwFanEvtD0Entry;
    Pnp.EvtDeviceD0Exit = SwFanEvtD0Exit;
    Pnp.EvtDeviceSelfManagedIoInit = SwFanEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwFanEvtSelfManagedIoCleanup;
    Pnp.EvtDeviceSelfManagedIoSuspend = SwFanEvtSuspend;
    Pnp.EvtDeviceSelfManagedIoRestart = SwFanEvtRestart;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWFAN_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &SwFanGetDevice(Device)->BacklightLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfDeviceCreateSymbolicLink(Device, &LinkName);
    SwFanTrace(L"SymbolicLinkStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = SwFanEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfIoQueueCreate(Device, &QueueConfig, &Attributes, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwFanParamsPath)) {
        RtlCopyMemory(SwFanParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwFanParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwFanParamsPath);
    }
    SwFanTrace(L"DriverVersion", 12);
    WDF_DRIVER_CONFIG_INIT(&Config, SwFanEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
