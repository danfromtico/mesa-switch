// SPDX-License-Identifier: MIT
// Bounded scheduler and miniport receipts for the independent DMA attempt journal.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <evntprov.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef EVENT_FILTER_TYPE_EVENT_ID
#define EVENT_FILTER_TYPE_EVENT_ID 0x80000200u
#endif

static const GUID dxg = {0x802ec45a,0x1e99,0x4b83,{0x99,0x20,0x87,0xc9,0x82,0x77,0xba,0x9d}};
static const GUID kmd = {0xc27c1f9b,0x62e8,0x4e21,{0xb2,0x9b,0x6b,0xb2,0xd5,0x85,0xc6,0xd0}};
static const wchar_t prefix[] = L"SwitchDdiLive-";
enum { Limit = 8192, MaxPayload = 512 };

typedef struct {
    EVENT_TRACE_PROPERTIES props;
    wchar_t name[96];
    wchar_t path[MAX_PATH];
} TraceProperties;

typedef struct {
    TRACEHANDLE reader;
    SRWLOCK output;
    HANDLE quit;
    LONG published, acknowledged;
    volatile LONG error;
    DWORD trace_status;
} Receiver;

static LONGLONG qpc(void)
{
    LARGE_INTEGER when;
    QueryPerformanceCounter(&when);
    return when.QuadPart;
}

static void error(Receiver *r)
{
    InterlockedExchange(&r->error, 1);
    SetEvent(r->quit);
}

static LONG failed(Receiver *r)
{
    return InterlockedCompareExchange(&r->error, 0, 0);
}

static void init_properties(TraceProperties *value)
{
    memset(value, 0, sizeof(*value));
    value->props.Wnode.BufferSize = sizeof(*value);
    value->props.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    value->props.Wnode.ClientContext = 1;
    value->props.LoggerNameOffset = offsetof(TraceProperties, name);
}

static ULONG enable(TRACEHANDLE session)
{
    struct {
        BOOLEAN include;
        UCHAR reserved;
        USHORT count;
        USHORT ids[12];
    } filter = {TRUE, 0, 12, {27,29,30,31,32,110,175,176,177,178,180,433}};
    EVENT_FILTER_DESCRIPTOR descriptor = {0};
    descriptor.Ptr = (ULONGLONG)(uintptr_t)&filter;
    descriptor.Size = sizeof(filter);
    descriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS ep = {0};
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    ep.EnableFilterDesc = &descriptor;
    ep.FilterDescCount = 1;
    ULONG rc = EnableTraceEx2(session, &dxg, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                               TRACE_LEVEL_VERBOSE, 0x843, 0, 3000, &ep);
    if (rc) return rc;
    return EnableTraceEx2(session, &dxg, EVENT_CONTROL_CODE_CAPTURE_STATE,
                          TRACE_LEVEL_VERBOSE, 0x843, 0, 3000, NULL);
}

static ULONG enable_kmd(TRACEHANDLE session)
{
    struct {
        BOOLEAN include;
        UCHAR reserved;
        USHORT count;
        USHORT ids[1];
    } filter = {TRUE, 0, 1, {7}};
    EVENT_FILTER_DESCRIPTOR descriptor = {0};
    descriptor.Ptr = (ULONGLONG)(uintptr_t)&filter;
    descriptor.Size = sizeof(filter);
    descriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS ep = {0};
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    ep.EnableFilterDesc = &descriptor;
    ep.FilterDescCount = 1;
    return EnableTraceEx2(session, &kmd, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_INFORMATION, 2, 0, 3000, &ep);
}

static int wanted(const EVENT_RECORD *e)
{
    const EVENT_HEADER *h = &e->EventHeader;
    const USHORT id = h->EventDescriptor.Id, bytes = e->UserDataLength;
    const BYTE ver = h->EventDescriptor.Version;
    if (IsEqualGUID(&kmd, &h->ProviderId))
        return id == 7 && ver == 2 && bytes == 32;
    if (!IsEqualGUID(&dxg, &h->ProviderId)) return 0;
    unsigned offset = 8;
    switch (id) {
    case 27:
    case 29:
        return ver == 2 && bytes == 60 ? 1 : -1;
    case 30:
    case 31:
    case 32:
        return ver == 0 && bytes == 64 ? 1 : -1;
    case 110:
        return ver == 1 && bytes >= 56 && bytes <= MaxPayload ? 1 : -1;
    case 433:
        return ver == 1 && bytes == 32 ? 1 : -1;
    case 175:
        if (ver != 1 || bytes != 44) return -1;
        offset = 16;
        break;
    case 176:
        if (ver != 0 || bytes != 28) return -1;
        break;
    case 177:
        if (ver != 1 || bytes != 52) return -1;
        break;
    case 178:
        if (ver != 1 || bytes != 60) return -1;
        break;
    case 180:
        if (ver != 1 || bytes != 32) return -1;
        break;
    default:
        return 0;
    }
    uint32_t type;
    memcpy(&type, (const BYTE *)e->UserData + offset, sizeof(type));
    return type == 0;
}

static void WINAPI event_record(PEVENT_RECORD e)
{
    Receiver *r = e->UserContext;
    int match = wanted(e);
    if (match == 0) return;
    if (match < 0 || e->UserDataLength > MaxPayload) {
        error(r);
        return;
    }
    const LONGLONG callback = qpc();
    static const char digits[] = "0123456789abcdef";
    char hex[2*MaxPayload+1];
    const BYTE *data = e->UserData;
    for (USHORT i = 0; i < e->UserDataLength; ++i) {
        hex[2*i] = digits[data[i] >> 4];
        hex[2*i+1] = digits[data[i] & 15];
    }
    hex[2*e->UserDataLength] = '\0';
    const EVENT_HEADER *h = &e->EventHeader;
    AcquireSRWLockExclusive(&r->output);
    if (r->published >= Limit || failed(r)) {
        error(r);
    } else if (printf("EVENT %ld %u %u %lu %lu %lld %lld %u %s\n",
                      ++r->published, h->EventDescriptor.Id, h->EventDescriptor.Version,
                      h->ProcessId, h->ThreadId, (long long)h->TimeStamp.QuadPart,
                      (long long)callback, e->UserDataLength, hex) < 0 || fflush(stdout)) {
        error(r);
    }
    ReleaseSRWLockExclusive(&r->output);
}

static DWORD WINAPI read_events(void *arg)
{
    Receiver *r = arg;
    r->trace_status = ProcessTrace(&r->reader, 1, NULL, NULL);
    if (r->trace_status) error(r);
    return 0;
}

static DWORD WINAPI read_acks(void *arg)
{
    Receiver *r = arg;
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        if (strcmp(line, "CLOSE\n") == 0 || strcmp(line, "CLOSE\r\n") == 0)
            return 0;
        if (strncmp(line, "ACK ", 4)) {
            error(r);
            return 1;
        }
        char *end;
        long n = strtol(line+4, &end, 10);
        AcquireSRWLockExclusive(&r->output);
        const int newline = *end == '\n' || (*end == '\r' && end[1] == '\n');
        if (!newline || end == line+4 || n != r->acknowledged + 1 ||
            n > r->published) {
            error(r);
        } else {
            ++r->acknowledged;
            if (printf("PERSISTED %ld %lld\n", n, (long long)qpc()) < 0 || fflush(stdout))
                error(r);
        }
        ReleaseSRWLockExclusive(&r->output);
        if (failed(r)) return 1;
    }
    if (WaitForSingleObject(r->quit, 0) != WAIT_OBJECT_0) error(r);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    const size_t plen = _countof(prefix)-1;
    if (argc != 4 || wcsncmp(argv[1], prefix, plen) || wcslen(argv[1]+plen) != 12 ||
        wcsspn(argv[1]+plen,L"0123456789abcdef") != 12) return 2;
    wchar_t *end;
    unsigned long ms = wcstoul(argv[3], &end, 10);
    if (wcsspn(argv[3], L"0123456789") != wcslen(argv[3]) ||
        *end || ms < 300 || ms > 12000) return 2;
    TraceProperties p;
    init_properties(&p);
    const DWORD count = GetFullPathNameW(argv[2], _countof(p.path), p.path, NULL);
    if (!count || count >= _countof(p.path) || GetFileAttributesW(p.path) != INVALID_FILE_ATTRIBUTES)
        return 2;
    p.props.LogFileNameOffset = offsetof(TraceProperties, path);
    p.props.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_REAL_TIME_MODE |
                          EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    p.props.MaximumFileSize = 64;
    p.props.BufferSize = 32;
    p.props.MinimumBuffers = 8;
    p.props.MaximumBuffers = 32;
    p.props.FlushTimer = 1;
    Receiver r = {0};
    HANDLE scheduler = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             SYNCHRONIZE | TIMER_MODIFY_STATE);
    r.quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    LARGE_INTEGER frequency;
    if (!scheduler || !r.quit || !QueryPerformanceFrequency(&frequency)) return 3;
    TRACEHANDLE session = 0;
    ULONG status = StartTraceW(&session, argv[1], &p.props);
    if (status) {
        fprintf(stderr, "StartTrace status=%lu\n", status);
        return 3;
    }
    EVENT_TRACE_LOGFILEW log = {0};
    log.LoggerName = argv[1];
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                           PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = event_record;
    log.Context = &r;
    r.reader = OpenTraceW(&log);
    if (r.reader == INVALID_PROCESSTRACE_HANDLE) status = GetLastError();
    HANDLE reader = NULL, acks = NULL;
    if (!status) status = enable(session);
    if (!status) status = enable_kmd(session);
    if (!status && (printf("READY %lu %lld %lld\n", GetCurrentProcessId(),
                          (long long)frequency.QuadPart, (long long)qpc()) < 0 || fflush(stdout)))
        status = ERROR_WRITE_FAULT;
    if (!status && !(acks = CreateThread(NULL, 0, read_acks, &r, 0, NULL))) status = GetLastError();
    if (!status && !(reader = CreateThread(NULL, 0, read_events, &r, 0, NULL))) status = GetLastError();
    const ULONGLONG deadline = GetTickCount64() + ms;
    ULONG flushes = 0;
    while (!status && !failed(&r) && GetTickCount64() < deadline) {
        LARGE_INTEGER due;
        due.QuadPart = -40000;
        if (!SetWaitableTimerEx(scheduler,&due,0,NULL,NULL,NULL,0)) {
            status = GetLastError();
            break;
        }
        HANDLE handles[2] = {r.quit,scheduler};
        if (WaitForMultipleObjects(2,handles,FALSE,250) != WAIT_OBJECT_0 + 1) {
            if (!failed(&r)) status = ERROR_TIMEOUT;
            break;
        }
        TraceProperties flush;
        init_properties(&flush);
        status = FlushTraceW(session, NULL, &flush.props);
        ++flushes;
    }
    init_properties(&p);
    ULONG stop = ControlTraceW(session, NULL, &p.props, EVENT_TRACE_CONTROL_STOP);
    if (reader && (stop || WaitForSingleObject(reader, 5000) != WAIT_OBJECT_0)) {
        error(&r);
        CloseTrace(r.reader);
        if (WaitForSingleObject(reader,5000) != WAIT_OBJECT_0) ExitProcess(4);
    } else if (r.reader != INVALID_PROCESSTRACE_HANDLE) CloseTrace(r.reader);
    AcquireSRWLockExclusive(&r.output);
    if (printf("DRAINED %ld\n",r.published) < 0 || fflush(stdout)) error(&r);
    ReleaseSRWLockExclusive(&r.output);
    if (acks && WaitForSingleObject(acks,5000) != WAIT_OBJECT_0) {
        error(&r);
        CancelSynchronousIo(acks);
        if (WaitForSingleObject(acks,2000) != WAIT_OBJECT_0) ExitProcess(4);
    }
    printf("END %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld\n",status,stop,r.trace_status,
           p.props.EventsLost,p.props.RealTimeBuffersLost,p.props.LogBuffersLost,flushes,
           r.published,r.acknowledged,r.error);
    fflush(stdout);
    if (reader) CloseHandle(reader);
    if (acks) CloseHandle(acks);
    CloseHandle(r.quit);
    CloseHandle(scheduler);
    return status || stop || r.trace_status || r.error || !flushes || !r.published ||
           r.acknowledged != r.published || p.props.EventsLost || p.props.LogBuffersLost ||
           p.props.RealTimeBuffersLost ? 4 : 0;
}
