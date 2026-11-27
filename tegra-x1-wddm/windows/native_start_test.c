/* SPDX-License-Identifier: MIT */
#include <ntifs.h>
#include "native_start_test.h"
#ifndef TG_GPU_NATIVE_TEST_VERSION
#error Native test entry requires an explicit package version
#endif
#define TG_STRING_(x) #x
#define TG_STRING(x) TG_STRING_(x)
#define TG_WIDE_(x) L##x
#define TG_WIDE(x) TG_WIDE_(x)

/* Explicit native boot authorization survives repeated StartDevice calls and
 * Windows restarts. Without it, retain the one-use desktop test gate. */
NTSTATUS TgGpuConsumeNativeStartPermit(void)
{
    static WCHAR native_name[]=
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\tegragpurender\\AllowNativeBoot";
    static WCHAR grant_name[]=
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\tegragpurender\\AllowDesktopStart";
    static WCHAR path_name[]=
        L"\\SystemRoot\\System32\\drivers\\TgGpuBootTest"
        TG_WIDE(TG_STRING(TG_GPU_NATIVE_TEST_VERSION)) L".bin";
    UNICODE_STRING grant={sizeof(grant_name)-sizeof(WCHAR),sizeof(grant_name),grant_name};
    UNICODE_STRING native={sizeof(native_name)-sizeof(WCHAR),sizeof(native_name),native_name};
    UNICODE_STRING path={sizeof(path_name)-sizeof(WCHAR),sizeof(path_name),path_name};
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    LARGE_INTEGER offset={0};
    HANDLE handle;
    ULONG permit=0;
    PAGED_CODE();
    InitializeObjectAttributes(&attributes,&native,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    NTSTATUS status=ZwOpenKey(&handle,KEY_QUERY_VALUE,&attributes);
    if(NT_SUCCESS(status)) {
        ZwClose(handle);
#ifdef TG_GPU_PAGING_STOP_PROBE
        return STATUS_ACCESS_DENIED;
#else
        return STATUS_SUCCESS;
#endif
    }
    if(status!=STATUS_OBJECT_NAME_NOT_FOUND && status!=STATUS_OBJECT_PATH_NOT_FOUND)
        return status;
    InitializeObjectAttributes(&attributes,&grant,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    status=ZwOpenKey(&handle,KEY_QUERY_VALUE,&attributes);
    if(!NT_SUCCESS(status))return status;
    ZwClose(handle);
    InitializeObjectAttributes(&attributes,&path,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    status=ZwCreateFile(&handle,FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|SYNCHRONIZE,
        &attributes,&iosb,NULL,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE|FILE_WRITE_THROUGH,NULL,0);
    if(!NT_SUCCESS(status))return status;
    status=ZwQueryInformationFile(handle,&iosb,&info,sizeof(info),FileStandardInformation);
    if(!NT_SUCCESS(status))goto done;
    if(info.EndOfFile.QuadPart!=4096){status=STATUS_INVALID_BUFFER_SIZE;goto done;}
    status=ZwReadFile(handle,NULL,NULL,NULL,&iosb,&permit,sizeof(permit),&offset,NULL);
    if(!NT_SUCCESS(status))goto done;
    if(iosb.Information!=sizeof(permit)){status=STATUS_END_OF_FILE;goto done;}
    if(permit!=1){status=STATUS_ACCESS_DENIED;goto done;}
    permit=0;
    status=ZwWriteFile(handle,NULL,NULL,NULL,&iosb,&permit,sizeof(permit),&offset,NULL);
    if(!NT_SUCCESS(status))goto done;
    if(iosb.Information!=sizeof(permit)){status=STATUS_DEVICE_DATA_ERROR;goto done;}
    status=ZwFlushBuffersFile(handle,&iosb);
done:
    ZwClose(handle);
    return status;
}
