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

/* PmicLock held. These writes are used only after the GPU owner has proved
 * isolation; live NAPLL changes remain in the GPU worker. */
static NTSTATUS SwFanGpuWriteM1Locked(PSWFAN_DEVICE Dev, UCHAR Reg, UCHAR Value)
{
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR memory;
    UCHAR bytes[2] = {Reg, Value}, check = 0;
    ULONG_PTR count = 0;
    NTSTATUS status;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, bytes, sizeof(bytes));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    status = WdfIoTargetSendWriteSynchronously(Dev->Pmic, NULL, &memory, NULL, &options, &count);
    if (!NT_SUCCESS(status)) return status;
    if (count != sizeof(bytes)) return STATUS_DEVICE_PROTOCOL_ERROR;
    status = SwFanReadReg(Dev->Pmic, Reg, &check);
    return !NT_SUCCESS(status) ? status : check == Value ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

static NTSTATUS SwFanGpuApplyReleased(PSWFAN_DEVICE Dev)
{
    struct tg_emc_gpu_vote *vote = &Dev->EmcGpuVote;
    ULONG floor = vote->failed == vote->request ? vote->guard_uv : TG_GPU_EMC_VOTE_UV(vote->request);
    UCHAR enable, code = (UCHAR)vote->release_value;
    BOOLEAN on = (vote->release_value & 0x100u) != 0;
    LARGE_INTEGER delay;
    NTSTATUS status;
    if (!vote->release_valid) return STATUS_INVALID_DEVICE_STATE;
    if (floor < vote->guard_uv) floor = vote->guard_uv;
    if (floor) {
        ULONG floor_code = (floor - 250000u + 4999u) / 5000u;
        if (!on || code < floor_code) code = (UCHAR)floor_code;
        on = TRUE;
    }
    if (code > GPU_M1_MAX_CODE || (on && code < GPU_M1_MIN_CODE)) return STATUS_INVALID_DEVICE_STATE;
    status = SwFanReadReg(Dev->Pmic, GPU_M1_ENABLE_REG, &enable);
    if (!NT_SUCCESS(status)) return status;
    enable = (UCHAR)((enable & ~1u) | (on ? 1u : 0u));
    /* With no remaining consumer, disable M1 before restoring a low VOUT. */
    if (!on) {
        status = SwFanGpuWriteM1Locked(Dev, GPU_M1_ENABLE_REG, enable);
        if (!NT_SUCCESS(status)) return status;
    }
    status = SwFanGpuWriteM1Locked(Dev, GPU_M1_VOUT_REG, code);
    if (NT_SUCCESS(status) && on) status = SwFanGpuWriteM1Locked(Dev, GPU_M1_ENABLE_REG, enable);
    if (!NT_SUCCESS(status)) return status;
    delay.QuadPart = -50000; /* same 5 ms settling interval as GPU power-up */
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    if (vote->request && vote->failed != vote->request &&
        !tg_emc_gpu_ack(vote, vote->request, 250000u + 5000u * code, on))
        return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}

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
            SwFanCpuLock(Dev);
            Op.value = Dev->Emc.SampleTime &&
                KeQueryInterruptTime() - Dev->Emc.SampleTime <= 60000000ULL &&
                (Dev->Emc.Published.Flags & SWFAN_EMC_RATE_VALID) ? Dev->Emc.Published.ActualKhz : 0;
            SwFanCpuUnlock(Dev);
            if (!Op.value) return STATUS_DEVICE_NOT_READY;
        } else if (Op.reg == TG_GPU_PMIC_QUERY_DVFS_POLICY) {
            Dev->GpuBridgeSeen = TRUE;
            Op.value = (ULONG)InterlockedCompareExchange(&Dev->GpuDvfsPolicy, 0, 0);
        } else if (Op.reg == TG_GPU_PMIC_QUERY_EMC_VOTE || Op.reg == TG_GPU_PMIC_QUERY_EMC_GUARD) {
            SwFanPmicLock(Dev);
            Op.value = Op.reg == TG_GPU_PMIC_QUERY_EMC_VOTE ?
                (Dev->EmcGpuVote.failed == Dev->EmcGpuVote.request ?
                 Dev->EmcGpuVote.acknowledged : Dev->EmcGpuVote.request) : Dev->EmcGpuVote.guard_uv;
            SwFanPmicUnlock(Dev);
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
        } else if (Op.reg == TG_GPU_PMIC_REPORT_EMC_ABI) {
            SwFanPmicLock(Dev);
            Dev->EmcGpuAbiTime = Op.value == TG_GPU_EMC_VOTE_ABI ? KeQueryInterruptTime() : 0;
            SwFanPmicUnlock(Dev);
        } else if (Op.reg == TG_GPU_PMIC_REPORT_EMC_ACK) {
            SwFanPmicLock(Dev);
            Status = SwFanReadReg(Dev->Pmic, GPU_M1_ENABLE_REG, &EnCtrl);
            if (NT_SUCCESS(Status)) Status = SwFanReadReg(Dev->Pmic, GPU_M1_VOUT_REG, &M1Code);
            if (NT_SUCCESS(Status) && !tg_emc_gpu_ack(&Dev->EmcGpuVote, Op.value,
                250000u + 5000u * M1Code, (EnCtrl & 1u) != 0)) Status = STATUS_INVALID_DEVICE_STATE;
            SwFanPmicUnlock(Dev);
            if (!NT_SUCCESS(Status)) return Status;
        } else if (Op.reg == TG_GPU_PMIC_CLAIM_RAIL) {
            if (Op.value != TG_GPU_EMC_VOTE_ABI) return STATUS_INVALID_PARAMETER;
            SwFanPmicLock(Dev);
            Dev->EmcGpuVote.released = false;
            Dev->EmcGpuAbiTime = KeQueryInterruptTime();
            SwFanPmicUnlock(Dev);
        } else if (Op.reg == TG_GPU_PMIC_REPORT_EMC_FAILURE) {
            SwFanPmicLock(Dev);
            if (!Op.value || Op.value != Dev->EmcGpuVote.request || Op.value == Dev->EmcGpuVote.acknowledged)
                Status = STATUS_INVALID_DEVICE_STATE;
            else { Dev->EmcGpuVote.failed = Op.value; Status = STATUS_SUCCESS; }
            SwFanPmicUnlock(Dev);
            if (!NT_SUCCESS(Status)) return Status;
        } else if (Op.reg == TG_GPU_PMIC_RELEASE_RAIL) {
            if (Op.address != Dev->Config.PmicAddress || Op.value > 0xffffu ||
                (Op.value & 0xffu) > GPU_M1_MAX_CODE ||
                ((Op.value & 0x100u) && (Op.value & 0xffu) < GPU_M1_MIN_CODE))
                return STATUS_INVALID_PARAMETER;
            SwFanPmicLock(Dev);
            /* Retain the first pre-GPU state across adapter restarts. A later
             * snapshot may already contain a voltage retained for EMC. */
            if (!Dev->EmcGpuVote.release_valid) {
                Dev->EmcGpuVote.release_value = Op.value;
                Dev->EmcGpuVote.release_valid = true;
            }
            Status = SwFanGpuApplyReleased(Dev);
            if (NT_SUCCESS(Status)) Dev->EmcGpuVote.released = true;
            SwFanPmicUnlock(Dev);
            if (!NT_SUCCESS(Status)) return Status;
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
            if (250000u + 5000u * Op.value < Dev->EmcGpuVote.guard_uv) {
                SwFanPmicUnlock(Dev);
                return STATUS_INVALID_DEVICE_STATE;
            }
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
            if (!(Op.value & 1u) && Dev->EmcGpuVote.guard_uv) {
                SwFanPmicUnlock(Dev);
                return STATUS_INVALID_DEVICE_STATE;
            }
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
