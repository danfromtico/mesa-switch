/* SPDX-License-Identifier: MIT */
/* Read loaded driver identities through public ETW image rundown and TDH.
 * Never read kernel memory or start/stop an existing tracing session. */
#define UNICODE 1
#define _UNICODE 1
#include <windows.h>
#include <objbase.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static const GUID image_guid = {0x2cb15d1d,0x5fc1,0x11d2,{0xab,0xe1,0x00,0xa0,0xc9,0x11,0xf5,0x18}};
static unsigned matches, failures;

static ULONG property(EVENT_RECORD *event, const WCHAR *name, void *out,
                      ULONG capacity, ULONG *bytes)
{
    PROPERTY_DATA_DESCRIPTOR d = {0};
    d.PropertyName = (ULONGLONG)(ULONG_PTR)name;
    d.ArrayIndex = ULONG_MAX;
    ULONG result = TdhGetPropertySize(event, 0, NULL, 1, &d, bytes);
    if (result) return result;
    if (!*bytes || *bytes > capacity) return ERROR_INSUFFICIENT_BUFFER;
    return TdhGetProperty(event, 0, NULL, 1, &d, *bytes, out);
}

static VOID WINAPI record(EVENT_RECORD *event)
{
    if (!IsEqualGUID(&event->EventHeader.ProviderId, &image_guid)) return;
    if (event->EventHeader.EventDescriptor.Opcode != 3 &&
        event->EventHeader.EventDescriptor.Opcode != 4) return;
    WCHAR path[1024] = {0}; ULONG bytes = 0;
    if (property(event, L"FileName", path, sizeof(path)-sizeof(WCHAR), &bytes)) {
        ++failures; return;
    }
    WCHAR *name = wcsrchr(path, L'\\'); name = name ? name+1 : path;
    const char *label = !_wcsicmp(name,L"switchfan.sys") ? "switchfan.sys" :
                        !_wcsicmp(name,L"tegragpurender.sys") ? "tegragpurender.sys" : NULL;
    if (!label) return;
    ULONG checksum=0, stamp=0, pid=0; ULONGLONG size=0;
    if (property(event,L"ImageChecksum",&checksum,sizeof(checksum),&bytes) || bytes!=4 ||
        property(event,L"TimeDateStamp",&stamp,sizeof(stamp),&bytes) || bytes!=4 ||
        property(event,L"ProcessId",&pid,sizeof(pid),&bytes) || bytes!=4 ||
        property(event,L"ImageSize",&size,sizeof(size),&bytes) || (bytes!=4 && bytes!=8)) {
        ++failures; return;
    }
    ++matches;
    printf("{\"image\":\"%s\",\"checksum\":%lu,\"timestamp\":%lu,\"image_size\":%llu,\"pid\":%lu,\"opcode\":%u}\n",
           label,(unsigned long)checksum,(unsigned long)stamp,
           (unsigned long long)size,(unsigned long)pid,event->EventHeader.EventDescriptor.Opcode);
}

int wmain(int argc, WCHAR **argv)
{
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    if (argc!=2 || wcslen(argv[1])>=MAX_PATH) return 2;
    if (GetFileAttributesW(argv[1])!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) {
        fprintf(stderr,"Output must be a new file in an existing directory\n"); return 2;
    }
    const ULONG capacity=sizeof(EVENT_TRACE_PROPERTIES)+2*MAX_PATH*sizeof(WCHAR);
    EVENT_TRACE_PROPERTIES *p=calloc(1,capacity);
    if (!p) return 3;
    p->Wnode.BufferSize=capacity; p->Wnode.Flags=WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext=1;
    if (FAILED(CoCreateGuid(&p->Wnode.Guid))) {free(p);return 3;}
    /* Image rundown is a burst, before the file writer can drain the pool.
     * The native first-boot capture lost17 events with at most1 MiB. Reserve
     * 2 MiB up front and cap expansion at4 MiB; loss still fails verification. */
    p->BufferSize=64; p->MinimumBuffers=32; p->MaximumBuffers=64;
    p->MaximumFileSize=8;
    p->LogFileMode=EVENT_TRACE_FILE_MODE_SEQUENTIAL|EVENT_TRACE_SYSTEM_LOGGER_MODE;
    p->EnableFlags=EVENT_TRACE_FLAG_IMAGE_LOAD|EVENT_TRACE_FLAG_NO_SYSCONFIG;
    p->LoggerNameOffset=sizeof(*p); p->LogFileNameOffset=sizeof(*p)+MAX_PATH*sizeof(WCHAR);
    WCHAR name[MAX_PATH]; swprintf(name,MAX_PATH,L"NvdecImageIdentity-%lu",GetCurrentProcessId());
    wcscpy((WCHAR *)((BYTE *)p+p->LogFileNameOffset),argv[1]);
    TRACEHANDLE session=0;
    ULONG result=StartTraceW(&session,name,p);
    if (result) {fprintf(stderr,"StartTrace=%lu\n",result);free(p);return 4;}
    Sleep(250);
    result=ControlTraceW(session,name,p,EVENT_TRACE_CONTROL_STOP);
    fprintf(stderr,"StopTrace=%lu events_lost=%lu buffers_lost=%lu session=%ls\n",
            result,p->EventsLost,p->LogBuffersLost,name);
    if (result || p->EventsLost || p->LogBuffersLost) {free(p);return 5;}
    free(p);
    EVENT_TRACE_LOGFILEW log={0}; log.LogFileName=argv[1];
    log.ProcessTraceMode=PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback=record;
    TRACEHANDLE reader=OpenTraceW(&log);
    if (reader==INVALID_PROCESSTRACE_HANDLE) {fprintf(stderr,"OpenTrace=%lu\n",GetLastError());return 6;}
    result=ProcessTrace(&reader,1,NULL,NULL);
    ULONG close_result=CloseTrace(reader);
    fprintf(stderr,"ProcessTrace=%lu CloseTrace=%lu matches=%u property_errors=%u\n",
            result,close_result,matches,failures);
    return result || close_result || !matches || failures ? 7 : 0;
}
