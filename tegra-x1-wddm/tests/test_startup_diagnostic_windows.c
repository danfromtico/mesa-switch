/* SPDX-License-Identifier: MIT */
#include "../windows/adapter.h"
#include <ntimage.h>

static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);

#define CHECK(x) do{if(!(x))ExitProcess(__LINE__);}while(0)

void *memset(void *p,int c,size_t n)
{
    UCHAR *d=p;
    while(n--)*d++=(UCHAR)c;
    return p;
}
void *memcpy(void *p,const void *s,size_t n)
{
    UCHAR *d=p;
    const UCHAR *a=s;
    while(n--)*d++=*a++;
    return p;
}

static DEVICE_OBJECT physical,other;
static TG_GPU_ADAPTER adapter;
static TG_GPU_STARTUP_DIAGNOSTIC_EVENT trace;
static ULONG trace_count;

void TgGpuTraceStartupDiagnostic(const TG_GPU_STARTUP_DIAGNOSTIC_EVENT *event)
{
    trace=*event;
    trace_count++;
}

static void check_string(const CHAR *result,const CHAR *expected)
{
    while(*expected)CHECK(*result++==*expected++);
    CHECK(!*result);
}

static void check_case(DXGK_DIAGNOSTICINFO_TYPE type,PDEVICE_OBJECT pdo,HANDLE context,
    UINT capacity,BOOLEAN has_buffer,NTSTATUS status,BOOLEAN available,
    enum tg_gpu_startup_stage stage,NTSTATUS start_status)
{
    UCHAR bytes[sizeof(TG_GPU_STARTUP_DIAGNOSTIC)+12];
    memset(bytes,0xa5,sizeof(bytes));
    DXGKARG_COLLECTDIAGNOSTICINFO args;
    memset(&args,0xa5,sizeof(args));
    args.Type=type;
    args.hAdapter=context;
    args.pBuffer=has_buffer?bytes+1:NULL;
    args.BufferSizeIn=capacity;
    args.pReserved=&other;
    const ULONG previous_traces=trace_count;
    CHECK(TgGpuDdiCollectDiagnosticInfo(pdo,&args)==status);
    CHECK(args.pReserved==&other && args.BufferSizeIn==capacity && args.hAdapter==context);
    CHECK(args.pBuffer==(has_buffer?bytes+1:NULL) && args.Type==type);

    UINT written=0;
    if (status==STATUS_SUCCESS) {
        TG_GPU_STARTUP_DIAGNOSTIC record;
        memcpy(&record,bytes+1,sizeof(record));
        written=sizeof(record);
        CHECK(record.Version==1 && record.Type==(ULONG)type);
        CHECK(record.AdapterAvailable==available && record.Stage==(ULONG)stage);
        CHECK(record.StartStatus==start_status);
        check_string(args.BucketingString,type==DXGK_DI_ADDDEVICE?
            "tegragpurender_adddevice":"tegragpurender_startdevice");
        check_string(args.DescriptionString,available?
            "adapter_context_available":"adapter_context_unavailable");
        CHECK(trace_count==previous_traces+1);
        CHECK(trace.BufferSizeIn==capacity && trace.BufferSizeOut==written);
        CHECK(trace.Snapshot.Version==record.Version && trace.Snapshot.Type==record.Type);
        CHECK(trace.Snapshot.AdapterAvailable==available && trace.Snapshot.Stage==(ULONG)stage);
        CHECK(trace.Snapshot.StartStatus==start_status);
    } else {
        CHECK(trace_count==previous_traces && !args.BucketingString[0] &&
            !args.DescriptionString[0]);
    }
    CHECK(args.BufferSizeOut==written && bytes[0]==0xa5);
    for (size_t i=written+1;i<sizeof(bytes);++i)CHECK(bytes[i]==0xa5);
}

void start(void)
{
    CHECK(TgGpuDdiCollectDiagnosticInfo(&physical,NULL)==STATUS_INVALID_PARAMETER);
    adapter.PhysicalDevice=&physical;
    TgGpuRecordStartup(&adapter,TgGpuStartupAdded,STATUS_SUCCESS);
    const UINT size=sizeof(TG_GPU_STARTUP_DIAGNOSTIC);
    check_case(DXGK_DI_ADDDEVICE,&physical,NULL,size,TRUE,STATUS_SUCCESS,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_ADDDEVICE,&physical,&adapter,size,TRUE,STATUS_SUCCESS,
        TRUE,TgGpuStartupAdded,STATUS_SUCCESS);
    TgGpuRecordStartup(&adapter,TgGpuStartupPermit,STATUS_DEVICE_NOT_READY);
    check_case(DXGK_DI_STARTDEVICE,&physical,&adapter,size+8,TRUE,STATUS_SUCCESS,
        TRUE,TgGpuStartupPermit,STATUS_DEVICE_NOT_READY);
    check_case(DXGK_DI_STARTDEVICE,&physical,NULL,size,TRUE,STATUS_SUCCESS,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_STARTDEVICE,&other,&adapter,size,TRUE,STATUS_SUCCESS,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_STARTDEVICE,NULL,&adapter,size,TRUE,STATUS_SUCCESS,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_STARTDEVICE,&physical,&adapter,size-1,TRUE,STATUS_BUFFER_TOO_SMALL,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_STARTDEVICE,&physical,NULL,0,FALSE,STATUS_BUFFER_TOO_SMALL,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_STARTDEVICE,&physical,NULL,size,FALSE,STATUS_INVALID_PARAMETER,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_STARTDEVICE,&physical,NULL,size-1,FALSE,STATUS_INVALID_PARAMETER,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);
    check_case(DXGK_DI_BLACKSCREEN,&physical,&adapter,size,TRUE,STATUS_NOT_SUPPORTED,
        FALSE,TgGpuStartupUnavailable,STATUS_SUCCESS);

    static const CHAR message[]="startup diagnostic model: ADDDEVICE STARTDEVICE NULL context "
        "unsupported type short buffer exact size canaries passed; output bytes=20\n";
    ULONG written;
    CHECK(WriteFile(GetStdHandle((ULONG)-11),message,sizeof(message)-1,&written,NULL));
    CHECK(written==sizeof(message)-1);
    ExitProcess(0);
}
