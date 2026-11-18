/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntprov.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

static const GUID kernel_process = {0x22fb2cd6,0x0e7b,0x422b,{0xa0,0xc7,0x2f,0xad,0x1f,0xd0,0xe7,0x16}};
static const GUID tegra = {0x5f0c4c6e,0x7a0b,0x4b52,{0x9d,0x3a,0x2f,0x6d,0x1e,0x8b,0x4a,0x11}};

template<size_t N>
static ULONG enable(TRACEHANDLE session, const GUID &provider, ULONGLONG keywords, const USHORT (&ids)[N])
{
    struct Filter { BOOLEAN include; UCHAR reserved; USHORT count; USHORT events[N]; };
    Filter f{TRUE,0,N,{}};
    std::memcpy(f.events,ids,sizeof(ids));
    EVENT_FILTER_DESCRIPTOR d{};
    d.Ptr = reinterpret_cast<ULONG_PTR>(&f);
    d.Size = offsetof(Filter,events)+sizeof(ids);
    d.Type = 0x80000200; // EVENT_FILTER_TYPE_EVENT_ID
    ENABLE_TRACE_PARAMETERS p{};
    p.Version=ENABLE_TRACE_PARAMETERS_VERSION_2; p.FilterDescCount=1; p.EnableFilterDesc=&d;
    return EnableTraceEx2(session,&provider,EVENT_CONTROL_CODE_ENABLE_PROVIDER,TRACE_LEVEL_VERBOSE,keywords,0,0,&p);
}

int wmain(int argc,wchar_t **argv)
{
    const wchar_t prefix[]=L"SwitchSystemD3D9-";
    if(argc!=4 || std::wcsncmp(argv[2],prefix,ARRAYSIZE(prefix)-1)!=0) return 2;
    size_t ns=(std::wcslen(argv[2])+1)*sizeof(wchar_t), fs=(std::wcslen(argv[3])+1)*sizeof(wchar_t);
    std::vector<BYTE> data(sizeof(EVENT_TRACE_PROPERTIES)+ns+fs);
    auto p=reinterpret_cast<EVENT_TRACE_PROPERTIES*>(data.data());
    p->Wnode.BufferSize=ULONG(data.size());
    p->LoggerNameOffset=sizeof(*p); p->LogFileNameOffset=ULONG(sizeof(*p)+ns);
    std::memcpy(data.data()+p->LoggerNameOffset,argv[2],ns);
    std::memcpy(data.data()+p->LogFileNameOffset,argv[3],fs);
    if(std::wcscmp(argv[1],L"stop")==0) {
        auto stop=ControlTraceW(0,argv[2],p,EVENT_TRACE_CONTROL_STOP);
        std::printf("stop=%lu lost=%lu lost_buffers=%lu buffers=%lu\n",stop,p->EventsLost,p->LogBuffersLost,p->BuffersWritten);
        return stop || p->EventsLost || p->LogBuffersLost ? 4 : 0;
    }
    if(std::wcscmp(argv[1],L"start") || GetFileAttributesW(argv[3])!=INVALID_FILE_ATTRIBUTES) return 2;
    p->Wnode.Flags=WNODE_FLAG_TRACED_GUID; p->Wnode.ClientContext=1;
    p->LogFileMode=EVENT_TRACE_FILE_MODE_SEQUENTIAL; p->BufferSize=64;
    p->MinimumBuffers=6; p->MaximumBuffers=24; p->MaximumFileSize=16; p->FlushTimer=1;
    TRACEHANDLE handle=0;
    auto start=StartTraceW(&handle,argv[2],p);
    if(start){std::printf("start=%lu\n",start);return 3;}
    const USHORT image_ids[]={5,6}, umd_ids[]={1,2,3,4,5,31};
    auto image=enable(handle,kernel_process,0x40,image_ids);
    auto umd=enable(handle,tegra,1,umd_ids);
    if(image || umd)ControlTraceW(handle,nullptr,p,EVENT_TRACE_CONTROL_STOP);
    std::printf("start=%lu image=%lu umd=%lu\n",start,image,umd);
    return image || umd ? 3 : 0;
}
