/* SPDX-License-Identifier: MIT */
#include "hardware.h"

NTSTATUS TgGpuStartHardware(TG_GPU_HARDWARE *h,TG_GPU_CONTEXT_ENGINE *e,
    const struct tg_gpu_power_io *io,UCHAR address,const struct tg_gpu_gr_payload *firmware)
{
    KIRQL irql;NTSTATUS status;struct tg_gpu_gr_work work;
    PAGED_CODE();
    if(!h || !e || !io || !io->read32 || !io->write32 || !io->delay_us ||
       !io->read_pmic || !io->write_pmic || !firmware || !e->Commands.Handle ||
       h->Engine || h->Boot.Handle || h->Work.Handle || h->Power.owned || h->Clock.owned)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->HardwareOwner || e->Active || e->Notification || e->Creating ||
       e->Channel.referenced || e->Graphics.Memory.Handle) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(h,sizeof(*h));h->Engine=e;h->Io=*io;
    e->HardwareOwner=h;e->UserdBar1Ready=FALSE;
    KeReleaseSpinLock(&e->Lock,irql);
    h->Status.version=TG_GPU_CONTROL_VERSION;h->Status.target_khz=TG_GPU_RENDER_KHZ;
    h->Status.clock_result=h->Status.execution_result=UINT32_MAX;
    /* Bootstrap never executes these reports; distinct sentinels are required
     * by the shared arena builder and cannot count as completed work.
     */
    h->Status.expected[0]=0x6f5a3c21;h->Status.expected[1]=~0x6f5a3c21u;
    status=TgGpuAllocatePrivateMemory(&e->Dxgk,TG_GPU_ARENA_BYTES,0x80000000,0xffffffff,&h->Boot);
    if(!NT_SUCCESS(status))return status;
    status=TgGpuAllocatePrivateMemory(&e->Dxgk,TG_GPU_GR_CHANNEL_WORK_BYTES,0x80000000,0xffffffff,&h->Work);
    if(!NT_SUCCESS(status))return status;
    work=(struct tg_gpu_gr_work){h->Work.CpuAddress,(UINT64)h->Work.SystemPhysicalAddress.QuadPart,h->Work.Bytes};
    h->Status.power_result=tg_gpu_power_up(io,address,&h->Power);h->GpuOwned=h->Power.owned;
    h->Status.stage=100+h->Power.stage;
    if(h->Status.power_result!=TG_GPU_POWER_OK)return STATUS_DEVICE_NOT_READY;
    if(tg_gpu_frontend_start(io,&h->Power,&h->Status.frontend)!=TG_GPU_FE_OK)return STATUS_DEVICE_DATA_ERROR;
    h->Status.clock_result=tg_gpu_clock_start(io,&h->Power,TG_GPU_RENDER_KHZ,&h->Clock);
    h->Status.stage=200+h->Clock.stage;
    h->Status.measured_khz=h->Clock.measured_khz;h->Status.rail_uv=h->Clock.rail_uv;
    if(h->Status.clock_result!=TG_GPU_CLOCK_OK)return STATUS_DEVICE_DATA_ERROR;
    h->Status.execution_result=tg_gpu_boot_graphics(io,&h->Power,h->Boot.CpuAddress,
        (UINT64)h->Boot.SystemPhysicalAddress.QuadPart,firmware,&work,&h->Execution,&h->Status);
    h->Status.stage=300+h->Execution.stage;
    if(h->Status.execution_result!=TG_GPU_EXEC_OK)return STATUS_DEVICE_DATA_ERROR;
    /* The retained boot BAR1 root now points at the engine's shared USERD /
     * completion page. HardwareOwner pins Commands until verified isolation.
     */
    h->Status.execution_result=tg_gpu_boot_map_userd(io,&h->Power,h->Boot.CpuAddress,
        (UINT64)h->Boot.SystemPhysicalAddress.QuadPart,
        (UINT64)e->Commands.SystemPhysicalAddress.QuadPart+TG_GPU_COMMAND_USERD,&h->Execution,&h->Status);
    if(h->Status.execution_result!=TG_GPU_EXEC_OK)return STATUS_DEVICE_DATA_ERROR;
    status=TgGpuInitializeGraphicsTemplate(e,&work,&h->Status.graphics);
    if(!NT_SUCCESS(status))return status;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping)status=STATUS_DEVICE_NOT_READY;
    else {
        e->UserdBar1Offset=TG_GPU_BOOT_USERD_BAR1_OFFSET;
        e->UserdBar1Ready=TRUE;h->Ready=TRUE;status=STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
NTSTATUS TgGpuIsolateHardware(TG_GPU_HARDWARE *h)
{
    TG_GPU_CONTEXT_ENGINE *e;KIRQL irql;
    enum tg_gpu_clock_result cr=TG_GPU_CLOCK_OK;
    enum tg_gpu_power_result pr;
    UINT32 clamp,reset;
    PAGED_CODE();
    if(!h)return STATUS_INVALID_PARAMETER;
    e=h->Engine;if(!e)return STATUS_SUCCESS;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->HardwareOwner!=h) {KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;}
    if(e->QueueOwner || e->Active || e->Notification || e->Channel.referenced || e->Creating) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    h->Ready=FALSE;e->UserdBar1Ready=FALSE;
    KeReleaseSpinLock(&e->Lock,irql);
    if(!h->Isolated) {
        if(h->Power.ready) {
            tg_gpu_execute_stop(&h->Io,&h->Execution);
            pr=tg_gpu_power_drain(&h->Io,&h->Power);
            if(pr!=TG_GPU_POWER_OK){h->Status.restore_result=pr;return STATUS_IO_TIMEOUT;}
        }
        if(h->Clock.owned) {
            /* Queue recovery may already have isolated/powered off the GPU. */
            cr=h->Power.owned?tg_gpu_clock_stop(&h->Io,&h->Clock):TG_GPU_CLOCK_READBACK_ERROR;
        }
        pr=tg_gpu_power_down(&h->Io,&h->Power);
        h->Status.restore_result=((UINT32)cr<<16)|pr;
        if(pr!=TG_GPU_POWER_OK || h->Power.owned)return STATUS_DEVICE_DATA_ERROR;
        if(h->GpuOwned) {
            clamp=h->Io.read32(h->Io.context,0x7000e6d4);
            reset=h->Io.read32(h->Io.context,0x6000628c);
            if(clamp==UINT32_MAX || reset==UINT32_MAX || !(clamp&1) || !(reset&0x01000000)) {
                h->Power.owned=true;h->Power.ready=false;return STATUS_DEVICE_DATA_ERROR;
            }
        }
        h->Isolated=TRUE;
        if(h->GpuOwned) {
            KeAcquireSpinLock(&e->Lock,&irql);
            if(!++e->GraphicsGeneration)++e->GraphicsGeneration;
            KeReleaseSpinLock(&e->Lock,irql);
        }
        /* Full reset retires GPU-register/DMA ownership even when restoring
         * a saved clock register failed. Report that error; never retry GPU
         * MMIO after power-off merely to clear a software ownership flag.
         */
        h->Clock.owned=h->Clock.running=false;h->Execution.owned=false;
    }
    return h->Status.restore_result?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
}
NTSTATUS TgGpuStopHardware(TG_GPU_HARDWARE *h)
{
    TG_GPU_CONTEXT_ENGINE *e;KIRQL irql;NTSTATUS status;
    PAGED_CODE();
    if(!h)return STATUS_INVALID_PARAMETER;
    e=h->Engine;if(!e)return STATUS_SUCCESS;
    status=TgGpuIsolateHardware(h);
    if(!h->Isolated || (status!=STATUS_SUCCESS && status!=STATUS_DEVICE_DATA_ERROR))return status;
    status=TgGpuFreePrivateMemory(&e->Dxgk,&h->Work);
    if(!NT_SUCCESS(status))return status;
    status=TgGpuFreePrivateMemory(&e->Dxgk,&h->Boot);
    if(!NT_SUCCESS(status))return status;
    KeAcquireSpinLock(&e->Lock,&irql);
    e->HardwareOwner=NULL;h->Engine=NULL;
    KeReleaseSpinLock(&e->Lock,irql);
    return h->Status.restore_result?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
}
