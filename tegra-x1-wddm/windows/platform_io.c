/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include "platform_io.h"
#include "../platform/t214_resources.h"

static NTSTATUS failure(TG_GPU_PLATFORM *p,NTSTATUS status)
{
    if(p && !NT_SUCCESS(status) && NT_SUCCESS(p->IoStatus))p->IoStatus=status;
    return status;
}
NTSTATUS TgGpuOpenPlatform(TG_GPU_PLATFORM *p,const TG_GPU_RESOURCES *resources)
{
    OBJECT_ATTRIBUTES attributes;IO_STATUS_BLOCK iosb={0};NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\SwitchBacklight");
    PAGED_CODE();
    if(!p || !resources || p->Bridge || p->Resources)return STATUS_INVALID_PARAMETER;
    for(UINT i=0;i<TgGpuApertureCount;++i)
        if(!resources->Mapping[i] || resources->Memory[i].Type!=CmResourceTypeMemory ||
           resources->Memory[i].u.Memory.Length<4)return STATUS_DEVICE_CONFIGURATION_ERROR;
    InitializeObjectAttributes(&attributes,(PUNICODE_STRING)&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    p->IoStatus=STATUS_SUCCESS;
    status=ZwCreateFile(&p->Bridge,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&attributes,&iosb,NULL,
        FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);
    if(!NT_SUCCESS(status)){p->Bridge=NULL;return failure(p,status);}
    p->Resources=resources;return STATUS_SUCCESS;
}
NTSTATUS TgGpuClosePlatform(TG_GPU_PLATFORM *p)
{
    NTSTATUS status;
    PAGED_CODE();
    if(!p)return STATUS_INVALID_PARAMETER;
    if(!p->Bridge){p->Resources=NULL;return STATUS_SUCCESS;}
    status=ZwClose(p->Bridge);
    if(NT_SUCCESS(status)){p->Bridge=NULL;p->Resources=NULL;}
    return failure(p,status);
}
static NTSTATUS platform_request(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op,BOOLEAN optional)
{
    IO_STATUS_BLOCK iosb={0};NTSTATUS status;
    PAGED_CODE();
    if(!p || !p->Bridge || !op)return STATUS_INVALID_PARAMETER;
    status=ZwDeviceIoControlFile(p->Bridge,NULL,NULL,NULL,&iosb,TG_GPU_IOCTL_PMIC,
        op,sizeof(*op),op,sizeof(*op));
    if(status==STATUS_PENDING) {
        status=ZwWaitForSingleObject(p->Bridge,FALSE,NULL);
        if(status!=STATUS_SUCCESS)return failure(p,NT_SUCCESS(status)?STATUS_IO_TIMEOUT:status);
        status=iosb.Status;
    }
    if(status!=STATUS_SUCCESS && NT_SUCCESS(status))status=STATUS_DEVICE_PROTOCOL_ERROR;
    if(NT_SUCCESS(status) && iosb.Information!=sizeof(*op))status=STATUS_DEVICE_PROTOCOL_ERROR;
    if(optional && (status==STATUS_INVALID_PARAMETER || status==STATUS_NOT_SUPPORTED ||
                    status==STATUS_INVALID_DEVICE_REQUEST || status==STATUS_DEVICE_NOT_READY))
        return status;
    return failure(p,status);
}
NTSTATUS TgGpuPlatformRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op)
{ return platform_request(p,op,FALSE); }
NTSTATUS TgGpuPlatformTryRequest(TG_GPU_PLATFORM *p,struct tg_gpu_pmic_request *op)
{ return platform_request(p,op,TRUE); }
NTSTATUS TgGpuPlatformPreflight(TG_GPU_PLATFORM *p,UCHAR *address)
{
    struct tg_gpu_pmic_request request={0,TG_GPU_PMIC_QUERY_ADDRESS,0,0};NTSTATUS status;
    if(!address)return STATUS_INVALID_PARAMETER;
    status=TgGpuPlatformRequest(p,&request);if(!NT_SUCCESS(status))return status;
    if(!request.value || request.value>0x7f)return failure(p,STATUS_DEVICE_PROTOCOL_ERROR);
    UCHAR candidate=(UCHAR)request.value;
    request=(struct tg_gpu_pmic_request){0,TG_GPU_PMIC_QUERY_TEMP,0,0};
    status=TgGpuPlatformRequest(p,&request);if(!NT_SUCCESS(status))return status;
    if(request.value>=80)return failure(p,STATUS_DEVICE_NOT_READY);
    *address=candidate;return STATUS_SUCCESS;
}
static PULONG location(TG_GPU_PLATFORM *p,UINT32 address,BOOLEAN write)
{
    const TG_GPU_RESOURCES *r=p->Resources;
    if(!r || (address&3))return NULL;
    if(r->LicMapping && address>=T214_GPU_LIC_BASE && address-T214_GPU_LIC_BASE<=T214_GPU_LIC_SIZE-4)
        return (PULONG)((PUCHAR)r->LicMapping+address-T214_GPU_LIC_BASE);
    for(UINT i=0;i<TgGpuApertureCount;++i) {
        UINT64 base=(UINT64)r->Memory[i].u.Memory.Start.QuadPart;
        UINT length=r->Memory[i].u.Memory.Length;
        if(r->Mapping[i] && length>=4 && address>=base && (UINT64)address-base<=length-4) {
            /* CPU/DDR/ASID ownership stays with its existing driver. Fuses
             * are read-only; no write to the PLLP query aperture is mapped.
             */
            if(write && (i==TgGpuMcAsid || i>=TgGpuHidrev))return NULL;
            return (PULONG)((PUCHAR)r->Mapping[i]+(UINT64)address-base);
        }
    }
    return NULL;
}
static UINT32 read32(void *context,UINT32 address)
{
    TG_GPU_PLATFORM *p=context;PULONG reg;
    if(address==0x600060a0) {
        struct tg_gpu_pmic_request op={0,TG_GPU_PMIC_QUERY_PLLP,0,0};
        return NT_SUCCESS(TgGpuPlatformRequest(p,&op))?op.value:UINT32_MAX;
    }
    reg=location(p,address,FALSE);
    if(!reg){failure(p,STATUS_DEVICE_CONFIGURATION_ERROR);return UINT32_MAX;}
    return READ_REGISTER_ULONG(reg);
}
static void write32(void *context,UINT32 address,UINT32 value)
{
    TG_GPU_PLATFORM *p=context;PULONG reg=location(p,address,TRUE);
    if(!reg){failure(p,STATUS_DEVICE_CONFIGURATION_ERROR);return;}
    KeMemoryBarrier();WRITE_REGISTER_ULONG(reg,value);KeMemoryBarrier();
}
static bool read_pmic(void *context,UINT8 address,UINT8 reg,UINT8 *value)
{
    struct tg_gpu_pmic_request op={address,reg,0,0};
    if(!NT_SUCCESS(TgGpuPlatformRequest(context,&op)))return false;
    *value=(UINT8)op.value;return true;
}
static bool write_pmic(void *context,UINT8 address,UINT8 reg,UINT8 value)
{
    struct tg_gpu_pmic_request op={address,reg,1,value};
    return NT_SUCCESS(TgGpuPlatformRequest(context,&op));
}
static void delay(void *context,UINT32 us)
{
    LARGE_INTEGER interval;UNREFERENCED_PARAMETER(context);
    if(us<1000){KeStallExecutionProcessor(us);return;}
    interval.QuadPart=-(LONGLONG)us*10;(VOID)KeDelayExecutionThread(KernelMode,FALSE,&interval);
}
struct tg_gpu_power_io TgGpuPlatformIo(TG_GPU_PLATFORM *p)
{ return (struct tg_gpu_power_io){p,read32,write32,read_pmic,write_pmic,delay}; }
