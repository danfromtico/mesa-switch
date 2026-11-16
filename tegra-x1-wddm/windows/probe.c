/* SPDX-License-Identifier: GPL-2.0-only */
/* Opt-in Windows hardware bring-up transport. This is a System-class KMDF
 * device, not a display miniport or Vulkan ICD. It leaves the POST display
 * alone and touches GPU registers only for an explicit administrator test.
 */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "control.h"
#include "platform_io.h"
#include "../platform/t214_resources.h"
#include "../platform/t214_clock.h"
#include "../core/execute.h"
#include "gr_firmware_io.h"
#include "diagnostic_backend.h"
#include "nvdec_backend.h"
#include "nvdec_control.h"

struct gpu_map { ULONG base, size; volatile UCHAR *va; };
static const struct gpu_map map_profile[] = {
    {T214_GPU_REG_BASE, T214_GPU_REG_SIZE, NULL},
    {T214_GPU_BAR1_BASE, T214_GPU_BAR1_SIZE, NULL},
    {T214_GPU_AUX_BASE, T214_GPU_AUX_SIZE, NULL},
#define MAP_RANGE(base, size) {base, size, NULL},
    T214_GPU_EXTRA_RESOURCES(MAP_RANGE)
#undef MAP_RANGE
};

typedef struct _GPU_DEVICE {
    struct gpu_map maps[T214_GPU_RESOURCE_COUNT];
    TG_GPU_RESOURCES resources; /* borrowed view of the mappings below */
    TG_GPU_PLATFORM platform;
    TG_GPU_DIAGNOSTIC_BACKEND *backend;
    struct tg_gpu_power power;
    struct tg_gpu_clock clock;
    struct tg_gpu_execution execution;
    struct tg_gpu_status last;
    PVOID arena;
    PHYSICAL_ADDRESS arena_address;
    struct tg_gpu_gr_work graphics;
    PVOID lic;
    WDFINTERRUPT interrupt;
    WDFDPC irq_dpc;
    struct tg_gpu_irq_state irq;
    volatile LONG irq_live, irq_dpcs;
    KEVENT irq_event;
    /* Experimental NVDEC bring-up transport; only explicit, validated LOAD
     * requests map its registers or create the engine. */
    volatile UINT32 *nvdec_regs;
    volatile UINT32 *host1x_regs;
    volatile UINT32 *pmc_regs;
    volatile UINT32 *car_regs;
    volatile UINT32 *mc_regs;
    struct tg_nvdec_win *nvdec;
} GPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GPU_DEVICE, GpuContext)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD GpuDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE GpuPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE GpuRelease;
EVT_WDF_DEVICE_D0_EXIT GpuD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GpuControl;
EVT_WDF_INTERRUPT_ISR GpuInterrupt;
EVT_WDF_DPC GpuInterruptDpc;



static NTSTATUS GpuNvdecUnmap(GPU_DEVICE *d)
{
    if (d->nvdec) {
        NTSTATUS status = TgNvdecDestroy(d->nvdec);
        if (!NT_SUCCESS(status)) return status;
        d->nvdec = NULL;
    }
    if (d->nvdec_regs) { MmUnmapIoSpace((PVOID)d->nvdec_regs, 0x40000); d->nvdec_regs = NULL; }
    if (d->host1x_regs) { MmUnmapIoSpace((PVOID)d->host1x_regs, 0x40000); d->host1x_regs = NULL; }
    if (d->pmc_regs) { MmUnmapIoSpace((PVOID)d->pmc_regs, 0x1000); d->pmc_regs = NULL; }
    if (d->car_regs) { MmUnmapIoSpace((PVOID)d->car_regs, 0x1000); d->car_regs = NULL; }
    if (d->mc_regs) { MmUnmapIoSpace((PVOID)d->mc_regs, 0x1000); d->mc_regs = NULL; }
    return STATUS_SUCCESS;
}

static void NvdecCopyError(char *dst, const char *src)
{
    if (src && src[0])
        (VOID)RtlStringCchCopyA(dst, TG_NVDEC_ERROR_SIZE, src);
    else
        (VOID)RtlStringCchCopyA(dst, TG_NVDEC_ERROR_SIZE, "NVDEC request failed");
}

static void GpuNvdecControl(WDFDEVICE device, WDFREQUEST request, ULONG code)
{
    GPU_DEVICE *d = GpuContext(device);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t in_len = 0, out_len = 0;
    PVOID in = NULL, out = NULL;
    size_t reply_bytes = 0;
    union {
        struct tg_nvdec_load_result load;
        struct tg_nvdec_config_result config;
        struct tg_nvdec_decode_result decode;
        struct tg_nvdec_status_result status;
    } reply;

    /* METHOD_BUFFERED input and output are the same system buffer. Build
     * headers separately until all request fields/payload have been read. */

    if (code == TG_GPU_IOCTL_NVDEC_STATUS && !d->nvdec) {
        struct tg_nvdec_status_result *res;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*res), &out, &out_len);
        if (NT_SUCCESS(status)) {
            res = out;
            RtlZeroMemory(res, sizeof(*res));
            res->version = TG_NVDEC_CONTROL_VERSION;
            res->created = 0;
        }
        WdfRequestCompleteWithInformation(request, status,
            NT_SUCCESS(status) ? sizeof(struct tg_nvdec_status_result) : 0);
        return;
    }
    if (!d->nvdec && code != TG_GPU_IOCTL_NVDEC_LOAD) {
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
        return;
    }

    if (code == TG_GPU_IOCTL_NVDEC_LOAD) {
        struct tg_nvdec_load_request *req;
        struct tg_nvdec_load_result *res;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*req), &in, &in_len);
        if (!NT_SUCCESS(status))
            goto done;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*res), &out, &out_len);
        if (!NT_SUCCESS(status))
            goto done;
        req = in;
        res = &reply.load;
        reply_bytes = sizeof(*res);
        RtlZeroMemory(res, sizeof(*res));
        res->version = TG_NVDEC_CONTROL_VERSION;
        if (in_len < sizeof(*req) || req->version != TG_NVDEC_CONTROL_VERSION ||
            req->size != in_len - sizeof(*req)) {
            res->result = STATUS_INVALID_PARAMETER;
            NvdecCopyError(res->error, "firmware request header/size mismatch");
            status = STATUS_SUCCESS;
            goto done;
        }
        if (!TgNvdecFirmwareValid((const UINT8 *)in + sizeof(*req),
                                  req->size, req->sha256)) {
            res->result = STATUS_INVALID_PARAMETER;
            NvdecCopyError(res->error, "firmware does not match the supported image");
            status = STATUS_SUCCESS;
            goto done;
        }
        if (!d->nvdec) {
            res->result = (UINT32)STATUS_NOT_SUPPORTED;
            NvdecCopyError(res->error, "replacement probe has no coexisting resource/clock owner");
            status = STATUS_SUCCESS;
            goto done;
        }
        res->result = (UINT32)TgNvdecLoadFirmware(d->nvdec,
            (const UINT8 *)in + sizeof(*req), req->size, req->sha256);
        res->firmware_bytes = req->size;
        res->booted = NT_SUCCESS((NTSTATUS)res->result) && TgNvdecFirmwareReady(d->nvdec);
        RtlCopyMemory(res->sha256, req->sha256, TG_NVDEC_SHA256_SIZE);
        if (!NT_SUCCESS((NTSTATUS)res->result))
            NvdecCopyError(res->error, TgNvdecLastError(d->nvdec));
        status = STATUS_SUCCESS;
    } else if (code == TG_GPU_IOCTL_NVDEC_CONFIGURE) {
        struct tg_nvdec_config_request *req;
        struct tg_nvdec_config_result *res;
        UINT32 pitch = 0, ah = 0, fs = 0;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*req), &in, &in_len);
        if (!NT_SUCCESS(status))
            goto done;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*res), &out, &out_len);
        if (!NT_SUCCESS(status))
            goto done;
        req = in;
        res = &reply.config;
        reply_bytes = sizeof(*res);
        RtlZeroMemory(res, sizeof(*res));
        res->version = TG_NVDEC_CONTROL_VERSION;
        if (req->version != TG_NVDEC_CONTROL_VERSION) {
            res->result = STATUS_REVISION_MISMATCH;
            status = STATUS_SUCCESS;
            goto done;
        }
        res->result = (UINT32)TgNvdecConfigure(d->nvdec, req->width, req->height,
                                               &pitch, &ah, &fs);
        if (NT_SUCCESS((NTSTATUS)res->result)) {
            res->pitch = pitch;
            res->aligned_height = ah;
            res->frame_size = fs;
        } else {
            NvdecCopyError(res->error, TgNvdecLastError(d->nvdec));
        }
        status = STATUS_SUCCESS;
    } else if (code == TG_GPU_IOCTL_NVDEC_DECODE) {
        struct tg_nvdec_decode_request *req;
        struct tg_nvdec_decode_result *res;
        const UINT8 *bits;
        const UINT32 *offsets;
        struct tg_nvdec_status st = {0};
        UINT32 aligned_offsets[TG_NVDEC_MAX_SLICES + 1u];
        UINT32 expected = 0, observed = 0;
        size_t need;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*req), &in, &in_len);
        if (!NT_SUCCESS(status))
            goto done;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*res), &out, &out_len);
        if (!NT_SUCCESS(status))
            goto done;
        req = in;
        res = &reply.decode;
        reply_bytes = sizeof(*res);
        RtlZeroMemory(res, sizeof(*res));
        res->version = TG_NVDEC_CONTROL_VERSION;
        if (req->version != TG_NVDEC_CONTROL_VERSION || !req->bitstream_size ||
            !req->slice_count || req->slice_count > TG_NVDEC_MAX_SLICES) {
            res->result = STATUS_INVALID_PARAMETER;
            status = STATUS_SUCCESS;
            goto done;
        }
        need = sizeof(*req) + req->bitstream_size +
               ((size_t)req->slice_count + 1u) * sizeof(UINT32);
        if (in_len < need) {
            res->result = STATUS_BUFFER_TOO_SMALL;
            status = STATUS_SUCCESS;
            goto done;
        }
        bits = (const UINT8 *)in + sizeof(*req);
        RtlCopyMemory(aligned_offsets, bits + req->bitstream_size,
                      ((size_t)req->slice_count + 1u) * sizeof(UINT32));
        offsets = aligned_offsets;
        res->result = (UINT32)TgNvdecDecode(d->nvdec, &req->pic, bits,
            req->bitstream_size, offsets, req->slice_count,
            (UINT8 *)out + sizeof(*res), out_len - sizeof(*res),
            &st, &expected, &observed);
        res->mbs_correctly_decoded = st.mbs_correctly_decoded;
        res->mbs_in_error = st.mbs_in_error;
        res->cycles = st.cycles;
        res->error_status = st.error_status;
        res->slice_header_error_code = st.slice_header_error_code;
        res->expected_syncpoint = expected;
        res->observed_syncpoint = observed;
        TgNvdecGetConfig(d->nvdec, &res->width, &res->height, &res->pitch,
                         &res->aligned_height, &res->frame_size);
        if (!NT_SUCCESS((NTSTATUS)res->result)) {
            NvdecCopyError(res->error, TgNvdecLastError(d->nvdec));
        } else {
            res->bytes_written = res->frame_size;
            RtlCopyMemory(out, res, sizeof(*res));
            WdfRequestCompleteWithInformation(request, STATUS_SUCCESS,
                                              sizeof(*res) + res->frame_size);
            return;
        }
        status = STATUS_SUCCESS;
    } else if (code == TG_GPU_IOCTL_NVDEC_STATUS) {
        struct tg_nvdec_status_result *res;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*res), &out, &out_len);
        if (!NT_SUCCESS(status))
            goto done;
        res = &reply.status;
        reply_bytes = sizeof(*res);
        RtlZeroMemory(res, sizeof(*res));
        res->version = TG_NVDEC_CONTROL_VERSION;
        res->created = 1;
        res->firmware_ready = TgNvdecFirmwareReady(d->nvdec);
        res->surfaces_ready = TgNvdecSurfacesReady(d->nvdec);
        TgNvdecGetConfig(d->nvdec, &res->width, &res->height, &res->pitch,
                         &res->aligned_height, &res->frame_size);
        status = STATUS_SUCCESS;
    } else if (code == TG_GPU_IOCTL_NVDEC_RESET) {
        struct tg_nvdec_status_result *res;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*res), &out, &out_len);
        if (!NT_SUCCESS(status))
            goto done;
        status = TgNvdecReset(d->nvdec);
        if (!NT_SUCCESS(status)) goto done;
        res = &reply.status;
        reply_bytes = sizeof(*res);
        RtlZeroMemory(res, sizeof(*res));
        res->version = TG_NVDEC_CONTROL_VERSION;
        res->created = 1;
        status = STATUS_SUCCESS;
    }

done:
    if (NT_SUCCESS(status) && reply_bytes)
        RtlCopyMemory(out, &reply, reply_bytes);
    WdfRequestCompleteWithInformation(request, status,
        NT_SUCCESS(status) ? reply_bytes : 0);
}

static NTSTATUS GpuOpenPlatform(WDFDEVICE device)
{
    GPU_DEVICE *d = GpuContext(device);
    return d->platform.Bridge?STATUS_SUCCESS:TgGpuOpenPlatform(&d->platform,&d->resources);
}
static struct tg_gpu_power_io GpuIo(GPU_DEVICE *d)
{
    return TgGpuPlatformIo(&d->platform);
}

BOOLEAN GpuInterrupt(WDFINTERRUPT interrupt,ULONG message)
{
    GPU_DEVICE *d=GpuContext(WdfInterruptGetDevice(interrupt));
    UNREFERENCED_PARAMETER(message);
    if(!InterlockedCompareExchange(&d->irq_live,0,0)) return FALSE;
    struct tg_gpu_power_io io=GpuIo(d);
    if(!tg_gpu_irq_handle(&io,&d->irq,&d->last.interrupt)) return FALSE;
    (VOID)WdfDpcEnqueue(d->irq_dpc);
    return TRUE;
}
VOID GpuInterruptDpc(WDFDPC dpc)
{
    GPU_DEVICE *d=GpuContext((WDFDEVICE)WdfDpcGetParentObject(dpc));
    InterlockedIncrement(&d->irq_dpcs);
    KeSetEvent(&d->irq_event,IO_NO_INCREMENT,FALSE);
}
static bool GpuIrqArm(void *context)
{
    GPU_DEVICE *d=context;
    struct tg_gpu_power_io io=GpuIo(d);
    KeClearEvent(&d->irq_event);
    WdfInterruptAcquireLock(d->interrupt);
    enum tg_gpu_irq_result r=tg_gpu_irq_arm(&io,&d->power,&d->irq,&d->last.interrupt);
    d->last.irq_gsi=T214_GPU_NONSTALL_GSI;
    InterlockedExchange(&d->irq_live,r==TG_GPU_IRQ_OK);
    WdfInterruptReleaseLock(d->interrupt);
    return r==TG_GPU_IRQ_OK;
}
static bool GpuIrqWait(void *context,uint32_t expected)
{
    GPU_DEVICE *d=context;
    ULONGLONG deadline=KeQueryInterruptTime()+20000000; /* two seconds per event */
    for(;;) {
        LARGE_INTEGER timeout;
        uint32_t count,error;
        KeClearEvent(&d->irq_event); /* clear before checking to avoid a lost wake */
        WdfInterruptAcquireLock(d->interrupt);
        count=d->last.interrupt.interrupts;error=d->last.interrupt.result;
        WdfInterruptReleaseLock(d->interrupt);
        if(error) return false;
        if(count>expected) {
            WdfInterruptAcquireLock(d->interrupt);
            d->last.interrupt.result=TG_GPU_IRQ_UNEXPECTED;
            WdfInterruptReleaseLock(d->interrupt);
            return false;
        }
        if(count>=expected && (uint32_t)InterlockedCompareExchange(&d->irq_dpcs,0,0)>=expected)
            return count==expected;
        ULONGLONG now=KeQueryInterruptTime();
        if(now>=deadline) {
            WdfInterruptAcquireLock(d->interrupt);
            if(!d->last.interrupt.result) d->last.interrupt.result=TG_GPU_IRQ_TIMEOUT;
            WdfInterruptReleaseLock(d->interrupt);
            return false;
        }
        timeout.QuadPart=-(LONGLONG)(deadline-now);
        (VOID)KeWaitForSingleObject(&d->irq_event,Executive,KernelMode,FALSE,&timeout);
    }
}
static VOID GpuIrqStop(GPU_DEVICE *d)
{
    if(!d->interrupt) return;
    struct tg_gpu_power_io io=GpuIo(d);
    WdfInterruptAcquireLock(d->interrupt);
    InterlockedExchange(&d->irq_live,0);
    if(d->irq.saved && d->power.ready)
        (VOID)tg_gpu_irq_disarm(&io,&d->irq,&d->last.interrupt);
    WdfInterruptReleaseLock(d->interrupt);
    (VOID)WdfDpcCancel(d->irq_dpc,TRUE);
    d->last.irq_dpcs=(uint32_t)InterlockedCompareExchange(&d->irq_dpcs,0,0);
}
static VOID GpuSaveResult(WDFDEVICE device)
{
    GPU_DEVICE *d = GpuContext(device);
    WDFKEY key;
    LARGE_INTEGER now;
    DECLARE_CONST_UNICODE_STRING(result_name, L"LastResult");
    DECLARE_CONST_UNICODE_STRING(time_name, L"LastResultTime");
    DECLARE_CONST_UNICODE_STRING(target_name, L"SelectedKhz");
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfDeviceGetDriver(device),
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) return;
    KeQuerySystemTime(&now);
    /* Fixed-size values overwritten after a user test; no growing log. */
    (VOID)WdfRegistryAssignValue(key, &result_name, REG_BINARY, sizeof(d->last), &d->last);
    (VOID)WdfRegistryAssignValue(key, &time_name, REG_QWORD, sizeof(now), &now);
    if (!d->last.power_result && !d->last.clock_result && !d->last.execution_result &&
        !d->last.frontend.result && !d->last.restore_result && !d->last.resources_owned)
        (VOID)WdfRegistryAssignULong(key, &target_name, d->last.target_khz);
    WdfRegistryClose(key);
}
static VOID GpuCleanup(GPU_DEVICE *d)
{
    struct tg_gpu_power_io io = GpuIo(d);
    enum tg_gpu_clock_result cr;
    enum tg_gpu_power_result pr;
    GpuIrqStop(d); /* no ISR/DPC can touch MMIO after this point */
    tg_gpu_execute_stop(&io, &d->execution);
    if (d->execution.owned && d->power.ready) {
        pr = tg_gpu_power_drain(&io, &d->power);
        if (pr != TG_GPU_POWER_OK) {
            d->last.restore_result = pr;
            d->last.resources_owned = 1;
            return; /* Never free memory while device writes may be live. */
        }
    }
    cr = tg_gpu_clock_stop(&io, &d->clock);
    pr = tg_gpu_power_down(&io, &d->power);
    d->last.restore_result = ((uint32_t)cr << 16) | pr;
    if (!d->power.owned) {
        /* Reset retires GPU mask ownership. LIC remains powered and must
         * still be restored after a partial interrupt-unwind failure.
         */
        if(d->irq.saved && tg_gpu_irq_retire(&io,&d->irq)!=TG_GPU_IRQ_OK)
            d->last.interrupt.restore_result=TG_GPU_IRQ_REGISTER;
        /* Full reset/power-down retired the GPU's register ownership too. */
        d->clock.owned = d->clock.running = false;
        d->execution.owned = d->execution.fifo_ready = false;
        d->execution.channel=(struct tg_gpu_channel){0};
        if (d->arena) {
            MmFreeContiguousMemorySpecifyCache(d->arena, TG_GPU_ARENA_BYTES, MmNonCached);
            d->arena = NULL;
        }
        if(d->graphics.cpu) {
            MmFreeContiguousMemorySpecifyCache(d->graphics.cpu,d->graphics.bytes,MmNonCached);
            d->graphics=(struct tg_gpu_gr_work){0};
        }
    }
    d->last.resources_owned = d->power.owned || d->execution.owned || d->irq.saved;
}
static NTSTATUS GpuRun(WDFDEVICE device, ULONG target, BOOLEAN graphics,BOOLEAN notify,BOOLEAN three_d,BOOLEAN clear,BOOLEAN draw)
{
    GPU_DEVICE *d = GpuContext(device);
    struct tg_gpu_power_io io = GpuIo(d);
    PHYSICAL_ADDRESS low = {0}, high, boundary = {0};
    TG_GPU_FIRMWARE firmware = {0};
    NTSTATUS status;
    uint8_t address;
    ULONG seed;
    if (target != TG_GPU_CONTROL_DEFAULT_KHZ && target != TG_GPU_CONTROL_OC_KHZ)
        return STATUS_INVALID_PARAMETER;
    if(notify && (!d->interrupt || !d->lic)) return STATUS_NOT_SUPPORTED;
    if (d->backend || d->power.owned || d->clock.owned || d->execution.owned || d->irq.saved)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&d->power, sizeof(d->power));
    RtlZeroMemory(&d->clock, sizeof(d->clock));
    RtlZeroMemory(&d->execution, sizeof(d->execution));
    status = GpuOpenPlatform(device);
    if (!NT_SUCCESS(status)) return status;
    status = TgGpuPlatformPreflight(&d->platform,&address);
    if (!NT_SUCCESS(status)) goto early;
    RtlZeroMemory(&d->last, sizeof(d->last));
    InterlockedExchange(&d->irq_dpcs,0);
    d->last.version = TG_GPU_CONTROL_VERSION;
    d->last.tested = 1;
    d->last.target_khz = target;
    d->last.clock_result = d->last.execution_result = UINT32_MAX;
    d->platform.IoStatus = STATUS_SUCCESS;
    seed = 0x13579bdfu ^ (ULONG)KeQueryInterruptTime();
    if (seed == 0xdeadbeef || ~seed == 0xdeadbeef) seed ^= 0x1000;
    d->last.expected[0] = seed; d->last.expected[1] = ~seed;
    d->last.firmware_status = TgGpuFirmwareLoad(&firmware);
    if (!NT_SUCCESS((NTSTATUS)d->last.firmware_status)) {
        d->last.execution_result = TG_GPU_EXEC_GR_BOOT;
        d->last.stage = 90;
        goto done;
    }
    high.QuadPart = 0xffffffff;
    d->arena = MmAllocateContiguousMemorySpecifyCache(TG_GPU_ARENA_BYTES,
                                                      low, high, boundary, MmNonCached);
    if (!d->arena) { TgGpuFirmwareFree(&firmware); status = STATUS_INSUFFICIENT_RESOURCES; goto early; }
    d->arena_address = MmGetPhysicalAddress(d->arena);
    if(graphics) {
        SIZE_T bytes=three_d?TG_GPU_GR_CHANNEL_WORK_BYTES:TG_GPU_GR_WORK_BYTES;
        d->graphics.cpu=MmAllocateContiguousMemorySpecifyCache(bytes,
            low,high,boundary,MmNonCached);
        if(!d->graphics.cpu) {
            d->last.execution_result=TG_GPU_EXEC_GR_CONTEXT;
            d->last.graphics.result=TG_GPU_GR_CTX_ARGUMENT;
            d->last.stage=91;goto done;
        }
        d->graphics.physical=(uint64_t)MmGetPhysicalAddress(d->graphics.cpu).QuadPart;
        d->graphics.bytes=bytes;
    }
    d->last.power_result = tg_gpu_power_up(&io, address, &d->power);
    d->last.stage = 100 + d->power.stage;
    if (d->last.power_result != TG_GPU_POWER_OK) goto done;
    d->last.boot0 = io.read32(io.context,T214_GPU_REG_BASE);
    if (tg_gpu_frontend_start(&io, &d->power, &d->last.frontend) != TG_GPU_FE_OK) {
        d->last.execution_result = TG_GPU_EXEC_FRONTEND;
        d->last.stage = 150;
        goto done;
    }
    d->last.clock_result = tg_gpu_clock_start(&io, &d->power, target, &d->clock);
    d->last.stage = 200 + d->clock.stage;
    d->last.measured_khz = d->clock.measured_khz;
    d->last.rail_uv = d->clock.rail_uv;
    if (d->last.clock_result != TG_GPU_CLOCK_OK) goto done;
    KeMemoryBarrier();
    const struct tg_gpu_execution_notify notification={d,GpuIrqArm,GpuIrqWait};
    d->last.execution_result = draw?
        tg_gpu_execute_draw(&io,&d->power,d->arena,(uint64_t)d->arena_address.QuadPart,
            &firmware.payload,&d->graphics,&d->execution,&d->last):clear?
        tg_gpu_execute_clear(&io,&d->power,d->arena,(uint64_t)d->arena_address.QuadPart,
            &firmware.payload,&d->graphics,&d->execution,&d->last):three_d?
        tg_gpu_execute_3d(&io,&d->power,d->arena,(uint64_t)d->arena_address.QuadPart,
            &firmware.payload,&d->graphics,&d->execution,&d->last):notify?
        tg_gpu_execute_irq(&io,&d->power,d->arena,(uint64_t)d->arena_address.QuadPart,
            &firmware.payload,&notification,&d->execution,&d->last):graphics?
        tg_gpu_execute_gr(&io,&d->power,d->arena,(uint64_t)d->arena_address.QuadPart,
            &firmware.payload,&d->graphics,&d->execution,&d->last):
        tg_gpu_execute(&io,&d->power,d->arena,(uint64_t)d->arena_address.QuadPart,
            &firmware.payload,&d->execution,&d->last);
    d->last.stage = 300 + d->execution.stage;
done:
    TgGpuFirmwareFree(&firmware); /* GPU DMA uses only its copied private arena. */
    if (d->power.ready) tg_gpu_frontend_snapshot(&io, &d->last.frontend);
    if (d->execution.fifo_ready && d->last.execution_result != TG_GPU_EXEC_OK)
        tg_gpu_execute_snapshot(&io, &d->execution, &d->last);
    GpuCleanup(d);
    if (!d->power.owned && !d->irq.saved) (VOID)TgGpuClosePlatform(&d->platform);
    GpuSaveResult(device);
    return d->platform.IoStatus;
early:
    {
    NTSTATUS close=TgGpuClosePlatform(&d->platform);
    return NT_SUCCESS(close)?status:close;
    }
}

static NTSTATUS GpuBackendRun(WDFDEVICE device,ULONG target)
{
    GPU_DEVICE *d=GpuContext(device);TG_GPU_FIRMWARE firmware={0};
    struct tg_gpu_power_io io;NTSTATUS status,close;UCHAR address;ULONG seed;
    if(target!=TG_GPU_CONTROL_DEFAULT_KHZ)return STATUS_INVALID_PARAMETER;
    if(d->backend || d->power.owned || d->clock.owned || d->execution.owned || d->irq.saved)
        return STATUS_INVALID_DEVICE_STATE;
    status=GpuOpenPlatform(device);if(!NT_SUCCESS(status))return status;
    status=TgGpuPlatformPreflight(&d->platform,&address);
    if(!NT_SUCCESS(status)) {
        close=TgGpuClosePlatform(&d->platform);return NT_SUCCESS(close)?status:close;
    }
    RtlZeroMemory(&d->last,sizeof(d->last));d->last.version=TG_GPU_CONTROL_VERSION;
    d->last.tested=1;d->last.target_khz=target;d->last.backend.requested=1;
    d->platform.IoStatus=STATUS_SUCCESS;io=GpuIo(d);
    seed=0x13579bdfu^(ULONG)KeQueryInterruptTime();
    if(seed==TG_GPU_CLEAR_SENTINEL || ~seed==TG_GPU_CLEAR_SENTINEL)seed^=0x1000;
    status=TgGpuFirmwareLoad(&firmware);d->last.firmware_status=(UINT)status;
    if(NT_SUCCESS(status)) {
        status=TgGpuRunDiagnosticBackend(&d->backend,&io,address,&firmware.payload,seed,&d->last);
        d->last.backend.result=(UINT)status;
    } else {
        d->last.stage=90;d->last.execution_result=TG_GPU_EXEC_GR_BOOT;
        d->last.backend.result=(UINT)status;
    }
    TgGpuFirmwareFree(&firmware);
    if(!d->backend) {
        close=TgGpuClosePlatform(&d->platform);
        if(!NT_SUCCESS(close) && !d->last.backend.result)d->last.backend.result=(UINT)close;
    }
    GpuSaveResult(device);
    /* Report a GPU/worker failure in the fixed-size status, preserving its
     * evidence for the caller. Transport failure still fails the IOCTL. */
    return d->platform.IoStatus;
}

VOID GpuControl(WDFQUEUE queue, WDFREQUEST request, size_t out_size, size_t in_size, ULONG code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    GPU_DEVICE *d = GpuContext(device);
    struct tg_gpu_status *out;
    struct tg_gpu_request *in, op;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(out_size); UNREFERENCED_PARAMETER(in_size);
    if (code == TG_GPU_IOCTL_NVDEC_LOAD || code == TG_GPU_IOCTL_NVDEC_CONFIGURE ||
        code == TG_GPU_IOCTL_NVDEC_DECODE || code == TG_GPU_IOCTL_NVDEC_STATUS ||
        code == TG_GPU_IOCTL_NVDEC_RESET) {
        GpuNvdecControl(device, request, code);
        return;
    }
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    if (code == TG_GPU_IOCTL_RUN || code == TG_GPU_IOCTL_GR_RUN || code==TG_GPU_IOCTL_IRQ_RUN ||
        code==TG_GPU_IOCTL_3D_RUN || code==TG_GPU_IOCTL_CLEAR_RUN || code==TG_GPU_IOCTL_DRAW_RUN ||
        code==TG_GPU_IOCTL_BACKEND_RUN) {
        status = WdfRequestRetrieveInputBuffer(request, sizeof(op), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status)) {
            op = *in;
            status = op.version != TG_GPU_CONTROL_VERSION ? STATUS_REVISION_MISMATCH :
                code==TG_GPU_IOCTL_BACKEND_RUN?GpuBackendRun(device,op.target_khz):
                GpuRun(device,op.target_khz,
                       code==TG_GPU_IOCTL_GR_RUN||code==TG_GPU_IOCTL_3D_RUN||code==TG_GPU_IOCTL_CLEAR_RUN||code==TG_GPU_IOCTL_DRAW_RUN,
                       code==TG_GPU_IOCTL_IRQ_RUN,code==TG_GPU_IOCTL_3D_RUN||code==TG_GPU_IOCTL_CLEAR_RUN||code==TG_GPU_IOCTL_DRAW_RUN,
                       code==TG_GPU_IOCTL_CLEAR_RUN||code==TG_GPU_IOCTL_DRAW_RUN,code==TG_GPU_IOCTL_DRAW_RUN);
        }
    } else if (code == TG_GPU_IOCTL_STATUS) status = STATUS_SUCCESS;
    else status = STATUS_INVALID_DEVICE_REQUEST;
    if (NT_SUCCESS(status)) *out = d->last;
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? sizeof(*out) : 0);
}
NTSTATUS GpuPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    GPU_DEVICE *d = GpuContext(device);
    ULONG i, n, found = 0;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR irq=NULL,irq_raw=NULL;
    NTSTATUS status=STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlCopyMemory(d->maps, map_profile, sizeof(map_profile));
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR rr=WdfCmResourceListGetDescriptor(raw,i);
        if(r && r->Type==CmResourceTypeInterrupt && rr && rr->Type==CmResourceTypeInterrupt &&
           rr->u.Interrupt.Vector==T214_GPU_NONSTALL_GSI) {
            if(irq || (r->Flags&CM_RESOURCE_INTERRUPT_LATCHED) ||
               r->ShareDisposition!=CmResourceShareDeviceExclusive) goto fail;
            irq=r;irq_raw=rr;
        }
        if (!r || r->Type != CmResourceTypeMemory) continue;
        if(r->u.Memory.Start.QuadPart==T214_GPU_LIC_BASE && r->u.Memory.Length==T214_GPU_LIC_SIZE) {
            if(d->lic) goto fail;
            d->lic=MmMapIoSpace(r->u.Memory.Start,r->u.Memory.Length,MmNonCached);
            if(!d->lic) goto fail;
            d->resources.Lic=*r;d->resources.LicMapping=d->lic;
            continue;
        }
        for (n = 0; n < T214_GPU_RESOURCE_COUNT; ++n)
            if (r->u.Memory.Start.QuadPart == d->maps[n].base && r->u.Memory.Length == d->maps[n].size) break;
        if (n == T214_GPU_RESOURCE_COUNT || d->maps[n].va) goto fail;
        d->maps[n].va = MmMapIoSpace(r->u.Memory.Start, r->u.Memory.Length, MmNonCached);
        if (!d->maps[n].va) goto fail;
        d->resources.Memory[n]=*r;d->resources.Mapping[n]=(PVOID)d->maps[n].va;
        ++found;
    }
    if(found!=T214_GPU_RESOURCE_COUNT) goto fail;
    if(d->lic) {
        WDF_INTERRUPT_CONFIG config;
        if(!irq) goto fail;
        WDF_INTERRUPT_CONFIG_INIT(&config,GpuInterrupt,NULL);
        config.AutomaticSerialization=FALSE;
        config.InterruptRaw=irq_raw;config.InterruptTranslated=irq;
        status=WdfInterruptCreate(device,&config,WDF_NO_OBJECT_ATTRIBUTES,&d->interrupt);
        if(!NT_SUCCESS(status)) goto fail;
    }
    return STATUS_SUCCESS;
fail:
    if(d->lic) { MmUnmapIoSpace(d->lic,T214_GPU_LIC_SIZE);d->lic=NULL; }
    for (n = 0; n < T214_GPU_RESOURCE_COUNT; ++n)
        if (d->maps[n].va) { MmUnmapIoSpace((PVOID)d->maps[n].va, d->maps[n].size); d->maps[n].va = NULL; }
    RtlZeroMemory(&d->resources,sizeof(d->resources));
    return status;
}
NTSTATUS GpuD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    GPU_DEVICE *d = GpuContext(device);
    UNREFERENCED_PARAMETER(target);
    if(d->backend) {
        NTSTATUS status=TgGpuStopDiagnosticBackend(&d->backend,&d->last);
        if(d->backend || !NT_SUCCESS(status))return NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status;
    }
    if (d->power.owned || d->execution.owned || d->arena || d->irq.saved) GpuCleanup(d);
    if(d->power.owned || d->irq.saved)return STATUS_DEVICE_BUSY;
    if (d->nvdec) {
        NTSTATUS nvdec_status = TgNvdecDestroy(d->nvdec);
        if (!NT_SUCCESS(nvdec_status)) return nvdec_status;
        d->nvdec = NULL;
    }
    return TgGpuClosePlatform(&d->platform);
}
NTSTATUS GpuRelease(WDFDEVICE device, WDFCMRESLIST resources)
{
    GPU_DEVICE *d = GpuContext(device);
    ULONG i;
    UNREFERENCED_PARAMETER(resources);
    {
        NTSTATUS nvdec_status = GpuNvdecUnmap(d);
        if (!NT_SUCCESS(nvdec_status)) return nvdec_status;
    }

    if(d->backend) {
        NTSTATUS status=TgGpuStopDiagnosticBackend(&d->backend,&d->last);
        if(d->backend || !NT_SUCCESS(status))return NT_SUCCESS(status)?STATUS_DEVICE_BUSY:status;
    }
    if (d->power.owned || d->execution.owned || d->arena || d->irq.saved) GpuCleanup(d);
    if(d->power.owned || d->irq.saved) return STATUS_DEVICE_BUSY;
    NTSTATUS status=TgGpuClosePlatform(&d->platform);
    if(!NT_SUCCESS(status))return status;
    if(d->interrupt) { WdfObjectDelete(d->interrupt);d->interrupt=NULL; }
    if(d->lic) { MmUnmapIoSpace(d->lic,T214_GPU_LIC_SIZE);d->lic=NULL; }
    for (i = 0; i < T214_GPU_RESOURCE_COUNT; ++i)
        if (d->maps[i].va) { MmUnmapIoSpace((PVOID)d->maps[i].va, d->maps[i].size); d->maps[i].va = NULL; }
    RtlZeroMemory(&d->resources,sizeof(d->resources));
    return d->power.owned ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
NTSTATUS GpuDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\TegraGpuProbe");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\TegraGpuProbe");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)");
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = GpuPrepare;
    pnp.EvtDeviceReleaseHardware = GpuRelease;
    pnp.EvtDeviceD0Exit = GpuD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, GPU_DEVICE);
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(status)) return status;
    KeInitializeEvent(&GpuContext(device)->irq_event,NotificationEvent,FALSE);
    {
        WDF_DPC_CONFIG config;
        WDF_DPC_CONFIG_INIT(&config,GpuInterruptDpc);
        config.AutomaticSerialization=FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT(&attr);
        attr.ParentObject=device;
        status=WdfDpcCreate(&config,&attr,&GpuContext(device)->irq_dpc);
        if(!NT_SUCCESS(status)) return status;
    }
    GpuContext(device)->last.version = TG_GPU_CONTROL_VERSION;
    GpuContext(device)->last.target_khz = TG_GPU_CONTROL_DEFAULT_KHZ;
    {
        WDFKEY key;
        ULONG selected;
        DECLARE_CONST_UNICODE_STRING(target_name, L"SelectedKhz");
        if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(driver, KEY_QUERY_VALUE,
            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
            if (NT_SUCCESS(WdfRegistryQueryULong(key, &target_name, &selected)) &&
                (selected == TG_GPU_CONTROL_DEFAULT_KHZ || selected == TG_GPU_CONTROL_OC_KHZ))
                GpuContext(device)->last.target_khz = selected;
            WdfRegistryClose(key);
        }
    }
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = GpuControl;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, GpuDeviceAdd);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
