/* SPDX-License-Identifier: GPL-2.0-only */
#include "windows/pmic.h"

/* Reviewed DVFS bridge from WindowsDrivers 9adf15b89a5271960d14892e22f954596eb8309c.
 * Keep GPU PMIC transport in mesa; SwitchFan owns the CPU/power policy and SPB
 * connection. Every register transaction is serialized by its PmicLock.
 */
#define GPU_M1_ENABLE_REG 0x06u
#define GPU_M1_VOUT_REG   0x23u
#define GPU_M1_MIN_CODE   68u  /* 250 + 68*5  = 590 mV (T210B01 HighOPT Vmin) */
#define GPU_M1_MAX_CODE   160u /* 250 + 160*5 = 1050 mV (T210B01 rail ceiling) */

static BOOLEAN SwFanGpuLiveCounter(PSWFAN_DEVICE Dev)
{
    return Dev->GpuAbiTime && KeQueryInterruptTime() - Dev->GpuAbiTime <= 20000000ULL;
}

static NTSTATUS SwFanGpuPmic(PSWFAN_DEVICE Dev, WDFREQUEST Request, ULONG_PTR *Info)
{
    struct tg_gpu_pmic_request *In, *Out, Op;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;
    UCHAR Buffer[2], Readback = 0, EnCtrl = 0, M1Code = 0;
    ULONG_PTR Bytes;
    NTSTATUS Status;

    if (WdfRequestGetRequestorMode(Request) != KernelMode)
        return STATUS_ACCESS_DENIED;
    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(Op), (PVOID *)&In, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Op = *In;
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(Op), (PVOID *)&Out, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (!Dev->IsLite || Dev->Stopping || !Dev->Pmic || !Dev->CpuConfigured)
        return STATUS_DEVICE_NOT_READY;

    if (!Op.write && Op.reg >= TG_GPU_PMIC_QUERY_ADDRESS) {
        if (Op.reg == TG_GPU_PMIC_QUERY_ADDRESS) {
            Op.value = Dev->Config.PmicAddress;
        } else if (Op.reg == TG_GPU_PMIC_QUERY_PLLP) {
            if (Dev->Car == NULL) return STATUS_DEVICE_NOT_READY;
            Op.value = SwCpuRead(Dev, 0xa0);
        } else if (Op.reg == TG_GPU_PMIC_QUERY_TEMP) {
            if (!Dev->TempValid) return STATUS_DEVICE_NOT_READY;
            Op.value = Dev->LastTemp;
        } else if (Op.reg == TG_GPU_PMIC_QUERY_EMC_KHZ) {
            if (!Dev->Config.RamActualKhz) return STATUS_DEVICE_NOT_READY;
            Op.value = Dev->Config.RamActualKhz;
        } else if (Op.reg == TG_GPU_PMIC_QUERY_DVFS_POLICY) {
            Dev->GpuBridgeSeen = TRUE;
            Op.value = (ULONG)InterlockedCompareExchange(&Dev->GpuDvfsPolicy, 0, 0);
        } else {
            return STATUS_INVALID_PARAMETER;
        }
        *Out = Op;
        *Info = sizeof(Op);
        return STATUS_SUCCESS;
    }

    if (Op.write == 1 && Op.reg >= TG_GPU_PMIC_REPORT_GPU_KHZ) {
        if (Op.reg == TG_GPU_PMIC_REPORT_GPU_KHZ) {
            Dev->GpuBridgeSeen = TRUE;
            Dev->GpuReportedKhz = Op.value;
            Dev->GpuSampleTime = Op.value && SwFanGpuLiveCounter(Dev) ? KeQueryInterruptTime() : 0;
        } else if (Op.reg == TG_GPU_PMIC_REPORT_GPU_UV) {
            Dev->GpuBridgeSeen = TRUE;
            Dev->GpuReportedUv = Op.value;
        } else if (Op.reg == TG_GPU_PMIC_REPORT_GPU_STATUS) {
            Dev->GpuBridgeSeen = TRUE;
            Dev->GpuStatusWord = Op.value;
        } else if (Op.reg == TG_GPU_PMIC_REPORT_ABI) {
            Dev->GpuAbiTime = Op.value == TG_GPU_LIVE_COUNTER_ABI ? KeQueryInterruptTime() : 0;
        } else {
            return STATUS_INVALID_PARAMETER;
        }
        *Out = Op;
        *Info = sizeof(Op);
        return STATUS_SUCCESS;
    }

    if (Op.address != Dev->Config.PmicAddress || Op.write > 1 || Op.value > 255 ||
        (Op.reg != GPU_M1_ENABLE_REG && Op.reg != GPU_M1_VOUT_REG &&
         (Op.reg != CPU_M4_VOUT || Op.write)))
        return STATUS_INVALID_PARAMETER;

    SwFanPmicLock(Dev);
    if (Op.write) {
        Buffer[0] = (UCHAR)Op.reg;
        Buffer[1] = (UCHAR)Op.value;
        if (Op.reg == GPU_M1_VOUT_REG) {
            if (Op.value > GPU_M1_MAX_CODE) {
                SwFanPmicUnlock(Dev);
                return STATUS_INVALID_PARAMETER;
            }
            if (Op.value < GPU_M1_MIN_CODE) {
                Status = SwFanReadReg(Dev->Pmic, GPU_M1_ENABLE_REG, &EnCtrl);
                if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
                /* Never allow undervolting M1 below 590 mV while enabled;
                 * restoring original_voltage after M1 disable remains valid. */
                if ((EnCtrl & 1u) != 0) {
                    SwFanPmicUnlock(Dev);
                    return STATUS_INVALID_PARAMETER;
                }
            }
        } else if (Op.reg == GPU_M1_ENABLE_REG) {
            if ((Op.value & 1u) != 0) {
                if (!Dev->TempValid || Dev->LastTemp >= 80) {
                    SwFanPmicUnlock(Dev);
                    return STATUS_DEVICE_NOT_READY;
                }
                Status = SwFanReadReg(Dev->Pmic, GPU_M1_VOUT_REG, &M1Code);
                if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
                if (M1Code < GPU_M1_MIN_CODE || M1Code > GPU_M1_MAX_CODE) {
                    SwFanPmicUnlock(Dev);
                    return STATUS_INVALID_DEVICE_STATE;
                }
            }
            Status = SwFanReadReg(Dev->Pmic, GPU_M1_ENABLE_REG, &Readback);
            if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
            Buffer[1] = (UCHAR)((Readback & ~1u) | (Op.value & 1u));
        }
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
        WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
        Bytes = 0;
        Status = WdfIoTargetSendWriteSynchronously(Dev->Pmic, NULL, &Memory, NULL, &Options, &Bytes);
        if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
        if (Bytes != sizeof(Buffer)) { SwFanPmicUnlock(Dev); return STATUS_DEVICE_PROTOCOL_ERROR; }
    }
    Status = SwFanReadReg(Dev->Pmic, (UCHAR)Op.reg, &Readback);
    SwFanPmicUnlock(Dev);
    if (!NT_SUCCESS(Status)) return Status;
    if (Op.write && Readback != Buffer[1]) return STATUS_DEVICE_DATA_ERROR;
    if (Op.write && Op.reg == GPU_M1_VOUT_REG)
        Dev->GpuReportedUv = (250u + (ULONG)Readback * 5u) * 1000u;
    Op.value = Readback;
    *Out = Op;
    *Info = sizeof(Op);
    return STATUS_SUCCESS;
}
