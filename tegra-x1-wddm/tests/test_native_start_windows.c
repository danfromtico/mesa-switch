/* SPDX-License-Identifier: MIT */
/* Exercise the kernel entry's permit consumption with controlled file/key
 * operations. No registry, file, hardware or live driver is changed. */
#define TG_GPU_NATIVE_TEST_VERSION 146
#include "../windows/native_start_test.c"
#include <ntimage.h>
static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) int __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
#define CHECK(x) do{if(!(x))ExitProcess(__LINE__);}while(0)
void *memset(void *p,int c,size_t n){UCHAR *d=p;while(n--)*d++=(UCHAR)c;return p;}
void *memcpy(void *p,const void *s,size_t n){UCHAR *d=p;const UCHAR *a=s;while(n--)*d++=*a++;return p;}
static struct {ULONG failure,handles,opens,writes,flushes,permit;BOOLEAN native;NTSTATUS native_error;} model;
static BOOLEAN named(const UNICODE_STRING *name,PCWSTR expected)
{
    USHORT i=0;while(expected[i]){if(i>=name->Length/sizeof(WCHAR)||name->Buffer[i]!=expected[i])return FALSE;++i;}
    return i==name->Length/sizeof(WCHAR);
}
static NTSTATUS NTAPI open_key(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES a)
{
    CHECK(access==KEY_QUERY_VALUE && a->Attributes==(OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE));
    if(named(a->ObjectName,L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\tegragpurender\\AllowNativeBoot")) {
        if(model.native_error)return model.native_error;
        if(!model.native)return STATUS_OBJECT_NAME_NOT_FOUND;
        *handle=(HANDLE)3;++model.handles;return STATUS_SUCCESS;
    }
    CHECK(named(a->ObjectName,L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\tegragpurender\\AllowDesktopStart"));
    if(model.failure==1)return STATUS_OBJECT_NAME_NOT_FOUND;
    *handle=(HANDLE)1;++model.handles;return STATUS_SUCCESS;
}
static NTSTATUS NTAPI create_file(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES a,
    PIO_STATUS_BLOCK io,PLARGE_INTEGER size,ULONG attributes,ULONG share,ULONG disposition,
    ULONG options,PVOID ea,ULONG ea_bytes)
{
    (void)io;CHECK(!size && !ea && !ea_bytes && !model.handles);
    CHECK(access==(FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|SYNCHRONIZE));
    CHECK(attributes==FILE_ATTRIBUTE_NORMAL && share==FILE_SHARE_READ && disposition==FILE_OPEN);
    CHECK(options==(FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE|FILE_WRITE_THROUGH));
    CHECK(named(a->ObjectName,L"\\SystemRoot\\System32\\drivers\\TgGpuBootTest146.bin"));
    ++model.opens;if(model.failure==2)return STATUS_OBJECT_NAME_NOT_FOUND;
    *handle=(HANDLE)2;++model.handles;return STATUS_SUCCESS;
}
static NTSTATUS NTAPI query_file(HANDLE h,PIO_STATUS_BLOCK io,PVOID data,ULONG bytes,FILE_INFORMATION_CLASS type)
{
    (void)io;CHECK(h==(HANDLE)2 && bytes==sizeof(FILE_STANDARD_INFORMATION) && type==FileStandardInformation);
    if(model.failure==3)return STATUS_UNSUCCESSFUL;
    ((FILE_STANDARD_INFORMATION *)data)->EndOfFile.QuadPart=model.failure==4?4095:4096;
    return STATUS_SUCCESS;
}
static NTSTATUS NTAPI read_file(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,
    PIO_STATUS_BLOCK io,PVOID data,ULONG bytes,PLARGE_INTEGER offset,PULONG key)
{
    CHECK(h==(HANDLE)2 && !event && !apc && !context && !key && offset && !offset->QuadPart && bytes==4);
    if(model.failure==5)return STATUS_UNSUCCESSFUL;
    io->Information=model.failure==6?3:4;*(ULONG *)data=model.permit;return STATUS_SUCCESS;
}
static NTSTATUS NTAPI write_file(HANDLE h,HANDLE event,PIO_APC_ROUTINE apc,PVOID context,
    PIO_STATUS_BLOCK io,PVOID data,ULONG bytes,PLARGE_INTEGER offset,PULONG key)
{
    CHECK(h==(HANDLE)2 && !event && !apc && !context && !key && offset && !offset->QuadPart && bytes==4);
    CHECK(!*(ULONG *)data && model.permit==1);++model.writes;
    if(model.failure==8)return STATUS_UNSUCCESSFUL;
    io->Information=model.failure==9?3:4;model.permit=0;return STATUS_SUCCESS;
}
static NTSTATUS NTAPI flush_file(HANDLE h,PIO_STATUS_BLOCK io)
{(void)io;CHECK(h==(HANDLE)2 && model.permit==0);++model.flushes;return model.failure==10?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;}
static NTSTATUS NTAPI close_handle(HANDLE h)
{CHECK((h==(HANDLE)1||h==(HANDLE)2||h==(HANDLE)3) && model.handles==1);--model.handles;return STATUS_SUCCESS;}
NTSTATUS (NTAPI * const __imp_ZwOpenKey)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES)=open_key;
NTSTATUS (NTAPI * const __imp_ZwCreateFile)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,
    PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG)=create_file;
NTSTATUS (NTAPI * const __imp_ZwQueryInformationFile)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS)=query_file;
NTSTATUS (NTAPI * const __imp_ZwReadFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,PLARGE_INTEGER,PULONG)=read_file;
NTSTATUS (NTAPI * const __imp_ZwWriteFile)(HANDLE,HANDLE,PIO_APC_ROUTINE,PVOID,PIO_STATUS_BLOCK,PVOID,ULONG,PLARGE_INTEGER,PULONG)=write_file;
NTSTATUS (NTAPI * const __imp_ZwFlushBuffersFile)(HANDLE,PIO_STATUS_BLOCK)=flush_file;
NTSTATUS (NTAPI * const __imp_ZwClose)(HANDLE)=close_handle;
void start(void)
{
    static const char pass[]="PASS: persistent native boot tolerates repeated starts; absent authorization retains durable one-use gating; errors close handles (controlled substitutes)\r\n";
    for(ULONG failure=0;failure<=10;++failure){
        memset(&model,0,sizeof(model));model.failure=failure;model.permit=failure==7?0:1;
        NTSTATUS status=TgGpuConsumeNativeStartPermit();
        CHECK(!model.handles);
        if(!failure){
            CHECK(status==STATUS_SUCCESS && !model.permit && model.writes==1 && model.flushes==1);
            CHECK(TgGpuConsumeNativeStartPermit()==STATUS_ACCESS_DENIED && !model.handles && model.writes==1);
        }else{
            CHECK(!NT_SUCCESS(status));
            if(failure<8)CHECK(!model.writes && !model.flushes);
            if(failure==1)CHECK(!model.opens);
            if(failure==8||failure==9)CHECK(!model.flushes);
        }
    }
    memset(&model,0,sizeof(model));model.native=TRUE;model.failure=1;
    CHECK(TgGpuConsumeNativeStartPermit()==STATUS_SUCCESS && !model.handles);
    CHECK(TgGpuConsumeNativeStartPermit()==STATUS_SUCCESS && !model.handles);
    CHECK(!model.opens && !model.writes && !model.flushes);
    model.native=FALSE;
    CHECK(TgGpuConsumeNativeStartPermit()==STATUS_OBJECT_NAME_NOT_FOUND);
    model.failure=0;model.permit=1;model.native_error=STATUS_ACCESS_DENIED;
    CHECK(TgGpuConsumeNativeStartPermit()==STATUS_ACCESS_DENIED && !model.handles && !model.opens);
    ULONG written;CHECK(WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL));ExitProcess(0);
}
