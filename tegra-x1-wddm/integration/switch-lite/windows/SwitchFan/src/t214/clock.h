/* SPDX-License-Identifier: GPL-2.0-only
 * Mariko CPU clock helper, owned by the Lite fan driver's passive timer.
 * PLL voltage 1120 mV is from both CPUB01_PLL_CVB_TABLE variants in L4T;
 * 1996.8 MHz stays below their common 2014.5 MHz / 1120 mV limit.
 * MAX77812 M4 is CPU. M1 (GPU), M3 (DRAM) and their registers are untouched.
 */
#define CPU_CCLK_POLICY 0x020
#define CPU_PLLX_BASE   0x0E0
#define CPU_PLLX_MISC   0x0E4
#define CPU_PLL_ENABLE (1u << 30)
#define CPU_PLL_BYPASS (1u << 31)
#define CPU_PLL_LOCK   (1u << 27)
#define CPU_PLL_DIVS   (0xFFu | (0xFFu << 8) | (0x1Fu << 20))
#define CPU_ON_PLLX    0x20008888u
#define CPU_ON_CLKM    0x20000000u
#define CPU_M4_VOUT    0x26
#define CPU_OC_VOUT    174u /* 250 + 174*5 = 1120 mV */

static ULONG SwCpuRead(PSWFAN_DEVICE Dev, ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Dev->Car + Offset));
}

static VOID SwCpuWrite(PSWFAN_DEVICE Dev, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Dev->Car + Offset), Value);
}

static ULONG SwCpuKhz(PSWFAN_DEVICE Dev)
{
    static const ULONG Pdiv[] = {1,2,3,4,5,6,8,10,12,16,12,16,20,24,32,32};
    ULONG Base, M, N, P, Policy;
    if (Dev->Car == NULL) return 0;
    Policy = SwCpuRead(Dev, CPU_CCLK_POLICY);
    if (Policy == CPU_ON_CLKM) return 19200;
    if (Policy != CPU_ON_PLLX) return 0;
    Base = SwCpuRead(Dev, CPU_PLLX_BASE);
    M = Base & 0xFF; N = (Base >> 8) & 0xFF; P = (Base >> 20) & 0x1F;
    if (M == 0 || P >= RTL_NUMBER_OF(Pdiv)) return 0;
    return (38400 * N) / (M * Pdiv[P]);
}

static NTSTATUS SwCpuVoltage(PSWFAN_DEVICE Dev, UCHAR Code)
{
    UCHAR Buffer[2] = {CPU_M4_VOUT, Code}, Readback;
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    LARGE_INTEGER Delay;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendWriteSynchronously(Dev->Pmic, NULL, &Memory, NULL, &Options, &Bytes);
    if (!NT_SUCCESS(Status)) return Status;
    if (Bytes != sizeof(Buffer)) return STATUS_DEVICE_PROTOCOL_ERROR;
    Delay.QuadPart = -20000; /* 2 ms; Hekate allows 1 ms for this regulator. */
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    Status = SwFanReadReg(Dev->Pmic, CPU_M4_VOUT, &Readback);
    if (NT_SUCCESS(Status) && Readback != Code) Status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(Status)) SwFanTrace(L"CpuMv", 250 + Code * 5);
    return Status;
}

static NTSTATUS SwCpuRelock(PSWFAN_DEVICE Dev, ULONG Dividers)
{
    ULONG Old = SwCpuRead(Dev, CPU_PLLX_BASE), New, Spin;
    KIRQL Irql;
    BOOLEAN Locked = FALSE;
    if ((Old & (CPU_PLL_ENABLE | CPU_PLL_BYPASS)) != CPU_PLL_ENABLE ||
        SwCpuRead(Dev, CPU_CCLK_POLICY) != CPU_ON_PLLX) return STATUS_INVALID_DEVICE_STATE;
    New = (Old & ~(CPU_PLL_DIVS | CPU_PLL_LOCK)) | (Dividers & CPU_PLL_DIVS);
    if ((Old & CPU_PLL_DIVS) == (New & CPU_PLL_DIVS)) return STATUS_SUCCESS;
    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    SwCpuWrite(Dev, CPU_CCLK_POLICY, CPU_ON_CLKM);
    KeStallExecutionProcessor(10);
    SwCpuWrite(Dev, CPU_PLLX_MISC, SwCpuRead(Dev, CPU_PLLX_MISC) | (1u << 18));
    SwCpuWrite(Dev, CPU_PLLX_BASE, New & ~CPU_PLL_ENABLE);
    KeStallExecutionProcessor(10);
    SwCpuWrite(Dev, CPU_PLLX_BASE, New | CPU_PLL_ENABLE);
    for (Spin = 0; Spin < 1000; ++Spin) {
        if ((SwCpuRead(Dev, CPU_PLLX_BASE) & CPU_PLL_LOCK) != 0) { Locked = TRUE; break; }
        KeStallExecutionProcessor(10);
    }
    if (!Locked) {
        SwCpuWrite(Dev, CPU_PLLX_BASE, Old & ~(CPU_PLL_ENABLE | CPU_PLL_LOCK));
        KeStallExecutionProcessor(10);
        SwCpuWrite(Dev, CPU_PLLX_BASE, (Old & ~CPU_PLL_LOCK) | CPU_PLL_ENABLE);
        for (Spin = 0; Spin < 1000; ++Spin) {
            if ((SwCpuRead(Dev, CPU_PLLX_BASE) & CPU_PLL_LOCK) != 0) break;
            KeStallExecutionProcessor(10);
        }
    }
    if ((SwCpuRead(Dev, CPU_PLLX_BASE) & CPU_PLL_LOCK) != 0)
        SwCpuWrite(Dev, CPU_CCLK_POLICY, CPU_ON_PLLX);
    KeLowerIrql(Irql);
    return Locked ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

static NTSTATUS SwCpuApply(PSWFAN_DEVICE Dev, BOOLEAN Overclock)
{
    NTSTATUS Status;
    if (!Dev->CpuConfigured || Dev->CpuApplied == Overclock) return STATUS_SUCCESS;
    if (Dev->Car == NULL || Dev->Pmic == NULL) return STATUS_DEVICE_NOT_READY;
    if (Overclock) {
        Status = SwCpuVoltage(Dev, CPU_OC_VOUT);
        if (!NT_SUCCESS(Status)) return Status;
        Status = SwCpuRelock(Dev, 2u | (104u << 8)); /* 38.4/2*104 = 1996.8 MHz */
        if (!NT_SUCCESS(Status)) {
            if (SwCpuKhz(Dev) <= Dev->BootCpuKhz) (VOID)SwCpuVoltage(Dev, Dev->BootVout);
            return Status;
        }
        Dev->CpuApplied = TRUE;
    } else {
        Status = SwCpuRelock(Dev, Dev->BootPllx);
        if (!NT_SUCCESS(Status)) return Status;
        Dev->CpuApplied = FALSE;
        Status = SwCpuVoltage(Dev, Dev->BootVout); /* frequency down before voltage down */
    }
    SwFanTrace(L"CpuActualKhz", SwCpuKhz(Dev));
    SwFanTrace(L"CpuOverclockActive", Dev->CpuApplied);
    return Status;
}

static VOID SwCpuPrepare(WDFDEVICE Device)
{
    PSWFAN_DEVICE Dev = SwFanGetDevice(Device);
    LARGE_INTEGER Id;
    NTSTATUS Status;
    // CPU M4/PLLX control is independent of the firmware's EMC result.
    // Keep the voltage/readback and thermal checks even when RAM stays stock.
    if (Dev->CpuConfigured || Dev->Car == NULL || Dev->Config.CpuKhz != LITE_CPU_KHZ ||
        Dev->Config.Status == LitePerfDisabled || Dev->Config.Status == LitePerfBadConfig) return;
    if (Dev->Config.PmicAddress == 0x31 && Dev->ConnectionCount >= 2) Id = Dev->PmicIds[0];
    else if (Dev->Config.PmicAddress == 0x33 && Dev->ConnectionCount >= 3) Id = Dev->PmicIds[1];
    else { SwFanTrace(L"CpuLastError", STATUS_DEVICE_CONFIGURATION_ERROR); return; }
    Status = SwFanOpenTarget(Device, Id, &Dev->Pmic);
    if (NT_SUCCESS(Status)) Status = SwFanReadReg(Dev->Pmic, CPU_M4_VOUT, &Dev->BootVout);
    if (NT_SUCCESS(Status) && (Dev->BootVout < 70 || Dev->BootVout > CPU_OC_VOUT))
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (NT_SUCCESS(Status)) {
        Dev->BootPllx = SwCpuRead(Dev, CPU_PLLX_BASE);
        Dev->BootCpuKhz = SwCpuKhz(Dev);
        if (Dev->BootCpuKhz == 0 || Dev->BootCpuKhz >= LITE_CPU_KHZ ||
            SwCpuRead(Dev, CPU_CCLK_POLICY) != CPU_ON_PLLX) Status = STATUS_INVALID_DEVICE_STATE;
    }
    Dev->CpuConfigured = NT_SUCCESS(Status);
    if (!Dev->CpuConfigured && Dev->Pmic != NULL) { WdfObjectDelete(Dev->Pmic); Dev->Pmic = NULL; }
    SwFanTrace(L"CpuLastError", (ULONG)Status);
    SwFanTrace(L"CpuActualKhz", SwCpuKhz(Dev));
    SwFanTrace(L"CpuBootKhz", Dev->BootCpuKhz);
}

static VOID SwCpuTemperature(PSWFAN_DEVICE Dev, ULONG Temp, BOOLEAN Valid)
{
    NTSTATUS Status;
    if (!Dev->CpuConfigured) return;
    if (!Valid || Temp >= 80) Dev->CpuThrottled = TRUE;
    else if (Temp <= 70) Dev->CpuThrottled = FALSE;
    Status = SwCpuApply(Dev, Dev->CpuOverclockEnabled && !Dev->CpuThrottled);
    SwFanTrace(L"CpuLastError", (ULONG)Status);
    SwFanTrace(L"CpuThrottled", Dev->CpuThrottled);
    SwFanTrace(L"CpuActualKhz", SwCpuKhz(Dev));
}
