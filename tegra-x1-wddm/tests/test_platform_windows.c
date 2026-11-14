/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include "../windows/platform_io.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>
static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
#define CHECK(x) do {if(!(x))ExitProcess(__LINE__);}while(0)
void *memset(void *p,int c,size_t n){UCHAR *d=p;while(n--)*d++=(UCHAR)c;return p;}
void *memcpy(void *p,const void *s,size_t n){UCHAR *d=p;const UCHAR *a=s;while(n--)*d++=*a++;return p;}

/* Real WDK provider with explicit file/MMIO/wait substitutes. Kernel-mode
 * requestor access is verified separately by the physical signed probe.
 */
static TG_GPU_RESOURCES resources;
static TG_GPU_PLATFORM platform;
static ULONG registers[TgGpuApertureCount][4],lic[T214_GPU_LIC_SIZE/4];
static const ULONG bases[]={T214_GPU_REG_BASE,T214_GPU_BAR1_BASE,T214_GPU_AUX_BASE,
#define BASE(b,s) b,
    T214_GPU_EXTRA_RESOURCES(BASE)
#undef BASE
};
static struct {
    NTSTATUS open,close,io,wait,completion;
    ULONG_PTR bytes;
    ULONG address,temp,opens,closes,requests,waits,stalls,sleeps;
} model;
static NTSTATUS NTAPI Open(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES attrs,
    PIO_STATUS_BLOCK iosb,PLARGE_INTEGER size,ULONG attr,ULONG share,ULONG disposition,
    ULONG options,PVOID ea,ULONG ea_size)
{
    static const WCHAR name[]=L"\\Device\\SwitchBacklight";
    CHECK(handle && iosb && !size && attr==FILE_ATTRIBUTE_NORMAL && !ea && !ea_size);
    CHECK(access==(GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE) && share==(FILE_SHARE_READ|FILE_SHARE_WRITE));
    CHECK(disposition==FILE_OPEN && options==(FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT));
    CHECK(attrs->Attributes==(OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE) && !attrs->RootDirectory);
    CHECK(attrs->ObjectName->Length==sizeof(name)-sizeof(WCHAR));
    for(UINT i=0;i<attrs->ObjectName->Length/sizeof(WCHAR);++i)CHECK(attrs->ObjectName->Buffer[i]==name[i]);
    ++model.opens;if(NT_SUCCESS(model.open))*handle=(HANDLE)123;return model.open;
}
static NTSTATUS NTAPI Close(HANDLE handle)
{CHECK(handle==(HANDLE)123);++model.closes;return model.close;}
static NTSTATUS NTAPI Ioctl(HANDLE file,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,
    PIO_STATUS_BLOCK iosb,ULONG code,PVOID in,ULONG in_bytes,PVOID out,ULONG out_bytes)
{
    struct tg_gpu_pmic_request *op=in;
    CHECK(file==(HANDLE)123 && !event && !apc && !context && code==TG_GPU_IOCTL_PMIC);
    CHECK(in==out && in_bytes==sizeof(*op) && out_bytes==sizeof(*op));++model.requests;
    if(op->reg==TG_GPU_PMIC_QUERY_ADDRESS)op->value=model.address;
    else if(op->reg==TG_GPU_PMIC_QUERY_TEMP)op->value=model.temp;
    else if(op->reg==TG_GPU_PMIC_QUERY_PLLP)op->value=0x12345678;
    else if(op->reg==TG_GPU_PMIC_QUERY_DVFS_POLICY)op->value=TG_GPU_DVFS_POLICY_PACK(1,153600,460800,2);
    else if(op->reg==TG_GPU_PMIC_QUERY_EMC_KHZ)op->value=1600000;
    else if(op->reg==TG_GPU_PMIC_REPORT_GPU_STATUS)CHECK(op->write==1);
    else {CHECK(op->address==0x33 && op->reg==0x23);if(!op->write)op->value=80;}
    iosb->Status=model.completion;iosb->Information=model.bytes;return model.io;
}
static NTSTATUS NTAPI Wait(HANDLE h,BOOLEAN alertable,PLARGE_INTEGER timeout)
{CHECK(h==(HANDLE)123 && !alertable && !timeout);++model.waits;return model.wait;}
static VOID NTAPI Stall(ULONG us){CHECK(us==999);++model.stalls;}
static NTSTATUS NTAPI Sleep(KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER interval)
{CHECK(mode==KernelMode && !alertable && interval->QuadPart==-10000);++model.sleeps;return STATUS_SUCCESS;}
NTSTATUS (NTAPI * const __imp_ZwCreateFile)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,
    PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG)=Open;
NTSTATUS (NTAPI * const __imp_ZwClose)(HANDLE)=Close;
NTSTATUS (NTAPI * const __imp_ZwDeviceIoControlFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,
    ULONG,PVOID,ULONG,PVOID,ULONG)=Ioctl;
NTSTATUS (NTAPI * const __imp_ZwWaitForSingleObject)(HANDLE,BOOLEAN,PLARGE_INTEGER)=Wait;
VOID (NTAPI * const __imp_KeStallExecutionProcessor)(ULONG)=Stall;
NTSTATUS (NTAPI * const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=Sleep;

static void Setup(void)
{
    CHECK(!platform.Bridge && !platform.Resources);
    memset(&platform,0,sizeof(platform));memset(&model,0,sizeof(model));
    model.bytes=sizeof(struct tg_gpu_pmic_request);model.address=0x33;model.temp=42;
}
void start(void)
{
    static const char pass[]="PASS: WDM platform bridge, declared MMIO bounds/ownership and failed close retry (controlled substitutes)\r\n";
    ULONG written;UCHAR address=0,value=0;struct tg_gpu_power_io io;
    for(UINT i=0;i<TgGpuApertureCount;++i) {
        resources.Memory[i].Type=CmResourceTypeMemory;
        resources.Memory[i].u.Memory.Start.QuadPart=bases[i];
        resources.Memory[i].u.Memory.Length=sizeof(registers[i]);resources.Mapping[i]=registers[i];
    }
    resources.LicMapping=lic;
    Setup();CHECK(TgGpuOpenPlatform(NULL,&resources)==STATUS_INVALID_PARAMETER);
    resources.Mapping[4]=NULL;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_DEVICE_CONFIGURATION_ERROR && !model.opens);
    resources.Mapping[4]=registers[4];model.open=STATUS_ACCESS_DENIED;
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_ACCESS_DENIED && !platform.Bridge && !platform.Resources);
    model.open=STATUS_SUCCESS;CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    CHECK(platform.Resources==&resources && platform.IoStatus==STATUS_SUCCESS);
    CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_INVALID_PARAMETER);
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && address==0x33);
    io=TgGpuPlatformIo(&platform);CHECK(io.context==&platform);
    CHECK(io.read_pmic(io.context,0x33,0x23,&value) && value==80);
    CHECK(io.write_pmic(io.context,0x33,0x23,81));
    CHECK(io.read32(io.context,0x600060a0)==0x12345678);
    io.write32(io.context,T214_GPU_REG_BASE,0xabcdef01);
    io.write32(io.context,T214_GPU_REG_BASE+12,0x89abcdef);
    CHECK(registers[0][0]==0xabcdef01 && io.read32(io.context,T214_GPU_REG_BASE+12)==0x89abcdef);
    io.write32(io.context,T214_GPU_LIC_BASE+T214_GPU_LIC_SIZE-4,7);
    CHECK(io.read32(io.context,T214_GPU_LIC_BASE+T214_GPU_LIC_SIZE-4)==7);
    ULONG saved[TgGpuApertureCount][4],saved_lic[T214_GPU_LIC_SIZE/4];
    memcpy(saved,registers,sizeof(saved));memcpy(saved_lic,lic,sizeof(saved_lic));
    io.write32(io.context,T214_GPU_REG_BASE+16,1);io.write32(io.context,T214_GPU_REG_BASE+1,1);
    for(UINT i=TgGpuMcAsid;i<TgGpuApertureCount;++i)io.write32(io.context,bases[i],1);
    io.write32(io.context,0x600060a0,1);io.write32(io.context,T214_GPU_LIC_BASE+T214_GPU_LIC_SIZE,1);
    CHECK(io.read32(io.context,T214_GPU_REG_BASE+16)==UINT32_MAX);
    for(UINT i=0;i<TgGpuApertureCount;++i)for(UINT j=0;j<4;++j)CHECK(saved[i][j]==registers[i][j]);
    for(UINT i=0;i<T214_GPU_LIC_SIZE/4;++i)CHECK(saved_lic[i]==lic[i]);
    CHECK(platform.IoStatus==STATUS_DEVICE_CONFIGURATION_ERROR);
    io.delay_us(io.context,999);io.delay_us(io.context,1000);CHECK(model.stalls==1 && model.sleeps==1);
    model.close=STATUS_UNSUCCESSFUL;
    CHECK(TgGpuClosePlatform(&platform)==STATUS_UNSUCCESSFUL && platform.Bridge && platform.Resources==&resources);
    model.close=STATUS_SUCCESS;CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS && !platform.Bridge && !platform.Resources);
    CHECK(io.read32(io.context,T214_GPU_REG_BASE)==UINT32_MAX);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    Setup();CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    model.temp=80;address=0;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_NOT_READY && !address);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    Setup();CHECK(TgGpuOpenPlatform(&platform,&resources)==STATUS_SUCCESS);
    model.address=0x100;CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_PROTOCOL_ERROR);
    model.address=0x33;model.io=STATUS_PENDING;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_SUCCESS && model.waits==2);
    model.completion=STATUS_ACCESS_DENIED;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_ACCESS_DENIED);
    model.completion=STATUS_SUCCESS;model.wait=STATUS_TIMEOUT;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_IO_TIMEOUT);
    model.io=STATUS_SUCCESS;model.bytes-=1;
    CHECK(TgGpuPlatformPreflight(&platform,&address)==STATUS_DEVICE_PROTOCOL_ERROR);
    model.bytes=sizeof(struct tg_gpu_pmic_request);platform.IoStatus=STATUS_SUCCESS;
    struct tg_gpu_pmic_request opt={0,TG_GPU_PMIC_QUERY_DVFS_POLICY,0,0};
    model.io=STATUS_INVALID_PARAMETER;
    CHECK(TgGpuPlatformTryRequest(&platform,&opt)==STATUS_INVALID_PARAMETER && platform.IoStatus==STATUS_SUCCESS);
    model.io=STATUS_NOT_SUPPORTED;
    CHECK(TgGpuPlatformTryRequest(&platform,&opt)==STATUS_NOT_SUPPORTED && platform.IoStatus==STATUS_SUCCESS);
    model.io=STATUS_SUCCESS;
    CHECK(TgGpuPlatformTryRequest(&platform,&opt)==STATUS_SUCCESS && TG_GPU_DVFS_POLICY_MODE(opt.value)==1);
    CHECK(TG_GPU_DVFS_POLICY_MIN_KHZ(opt.value)==153600 && TG_GPU_DVFS_POLICY_MAX_KHZ(opt.value)==460800);
    CHECK(TgGpuClosePlatform(&platform)==STATUS_SUCCESS);
    CHECK(WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL));ExitProcess(0);
}
