/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static const GUID provider={0xc27c1f9b,0x62e8,0x4e21,{0xb2,0x9b,0x6b,0xb2,0xd5,0x85,0xc6,0xd0}};
static ULONG records;
static void WINAPI record(EVENT_RECORD *e){
 if(memcmp(&e->EventHeader.ProviderId,&provider,sizeof(provider)))return;
 ++records;
 printf("{\"event\":%u,\"version\":%u,\"pid\":%lu,\"tid\":%lu,\"qpc\":%llu,\"bytes\":%u,\"hex\":\"",e->EventHeader.EventDescriptor.Id,e->EventHeader.EventDescriptor.Version,e->EventHeader.ProcessId,e->EventHeader.ThreadId,(unsigned long long)e->EventHeader.TimeStamp.QuadPart,e->UserDataLength);
 const unsigned char *b=e->UserData;for(USHORT i=0;i<e->UserDataLength;i++)printf("%02x",b[i]);puts("\"}");
}
int wmain(int argc,wchar_t **argv){
 if(argc==3 && !wcscmp(argv[1],L"read")){
  EVENT_TRACE_LOGFILEW f={0};f.LogFileName=argv[2];f.ProcessTraceMode=PROCESS_TRACE_MODE_EVENT_RECORD|PROCESS_TRACE_MODE_RAW_TIMESTAMP;f.EventRecordCallback=record;
  TRACEHANDLE h=OpenTraceW(&f);if(h==INVALID_PROCESSTRACE_HANDLE){fprintf(stderr,"OpenTrace %lu\n",GetLastError());return 1;}
  ULONG s=ProcessTrace(&h,1,NULL,NULL);CloseTrace(h);printf("{\"read_status\":%lu,\"provider_records\":%lu}\n",s,records);return s?1:0;
 }
 if(argc!=2 && argc!=3){fputs("usage: paging-etw.exe start <absolute-etl> | stop | read <etl>\n",stderr);return 2;}
 const BOOL start=!wcscmp(argv[1],L"start");if((start && argc!=3)||(!start && wcscmp(argv[1],L"stop")))return 2;
 struct buffers {EVENT_TRACE_PROPERTIES p;wchar_t name[256],file[1024];} b={0};
 b.p.Wnode.BufferSize=sizeof(b);b.p.Wnode.ClientContext=1;b.p.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
 b.p.BufferSize=64;b.p.MinimumBuffers=4;b.p.MaximumBuffers=16;b.p.MaximumFileSize=16;b.p.FlushTimer=1;
 b.p.LogFileMode=EVENT_TRACE_FILE_MODE_SEQUENTIAL;b.p.LoggerNameOffset=(ULONG)offsetof(struct buffers,name);b.p.LogFileNameOffset=(ULONG)offsetof(struct buffers,file);
 const wchar_t *name=L"SwitchWOA-Pal4DailyPaging";TRACEHANDLE h=0;ULONG status;
 if(start){
  if(wcslen(argv[2])>=1024)return 2;wcscpy(b.file,argv[2]);
  status=StartTraceW(&h,name,&b.p);
  if(!status){status=EnableTraceEx2(h,&provider,EVENT_CONTROL_CODE_ENABLE_PROVIDER,TRACE_LEVEL_ERROR,1,0,5000,NULL);if(status)ControlTraceW(h,name,&b.p,EVENT_TRACE_CONTROL_STOP);}
 }else status=ControlTraceW(0,name,&b.p,EVENT_TRACE_CONTROL_STOP);
 printf("{\"action\":\"%s\",\"status\":%lu,\"events_lost\":%lu,\"log_buffers_lost\":%lu,\"buffers_written\":%lu}\n",start?"start":"stop",status,b.p.EventsLost,b.p.LogBuffersLost,b.p.BuffersWritten);
 return status?1:0;
}
