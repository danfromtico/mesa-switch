/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "gr_firmware_io.h"
NTSTATUS TgGpuStartPlatformAdapter(TG_GPU_ADAPTER *a)
{
    TG_GPU_FIRMWARE firmware={0};struct tg_gpu_power_io io;UCHAR address=0;
    NTSTATUS status,cleanup;
    PAGED_CODE();
    if(!a || a->State!=TgGpuAdapterReady || !a->EngineReady || !a->QueueReady ||
       a->Platform.Bridge || a->Platform.Resources || a->Hardware.Engine ||
       a->Engine.HardwareOwner || a->Worker.Thread)
        return STATUS_INVALID_DEVICE_STATE;
    status=TgGpuOpenPlatform(&a->Platform,&a->Resources);
    if(NT_SUCCESS(status))status=TgGpuPlatformPreflight(&a->Platform,&address);
    if(NT_SUCCESS(status))status=TgGpuFirmwareLoad(&firmware);
    if(NT_SUCCESS(status)) {
        io=TgGpuPlatformIo(&a->Platform);
        status=TgGpuStartHardware(&a->Hardware,&a->Engine,&io,address,&firmware.payload);
        if(NT_SUCCESS(status) && !NT_SUCCESS(a->Platform.IoStatus))status=a->Platform.IoStatus;
    }
    TgGpuFirmwareFree(&firmware);
    if(NT_SUCCESS(status))TgGpuDvfsAttach(&a->Hardware,&a->Platform,NULL);
    if(NT_SUCCESS(status))status=TgGpuStartWorker(&a->Worker,&a->Queue,&a->Hardware);
    if(NT_SUCCESS(status))return STATUS_SUCCESS;
    if(!a->Hardware.Power.owned)TgGpuPlatformAbandonRail(&a->Platform,address);
    cleanup=TgGpuDdiStopDevice(a);
    return NT_SUCCESS(cleanup)?status:cleanup;
}
