// SPDX-License-Identifier: MIT
// Stream one application window's DXGI and DxgKrnl receipts over an existing pipe.
// The controller fsyncs each EVENT before replying "ACK n". RECEIVED timestamps
// that reply on the same Windows QPC clock as ETW. Records before an ACK have
// no measured upper bound on off-device persistence. No driver changes are made.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <evntprov.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const GUID dxgi = {0xca11c036,0x0102,0x4a2d,{0xa6,0xad,0xf0,0x3c,0xfe,0xd5,0xd3,0xc9}};
static const GUID dxgkrnl = {0x802ec45a,0x1e99,0x4b83,{0x99,0x20,0x87,0xc9,0x82,0x77,0xba,0x9d}};
static const wchar_t prefix[] = L"SwitchPresentLive-";

typedef struct {
    EVENT_TRACE_PROPERTIES properties;
    wchar_t name[96];
} SessionProperties;

typedef struct {
    SRWLOCK output_lock;
    HANDLE stopping;
    HANDLE target_process;
    TRACEHANDLE reader;
    volatile LONG target_pid;
    volatile LONG failed;
    LONG published;
    LONG acknowledged;
    BOOL draining;
    BOOL completed;
    BOOL focused_blt;
    DWORD process_trace_status;
    HANDLE journal;
    char journal_token[13];
    LONG journal_sequence;
    DWORD journal_status;
    LONGLONG journal_start;
    LONGLONG journal_finish;
} Receiver;

static LONGLONG ticks(void)
{
    LARGE_INTEGER result;
    QueryPerformanceCounter(&result);
    return result.QuadPart;
}

static void fail(Receiver *r)
{
    InterlockedExchange(&r->failed, 1);
    SetEvent(r->stopping);
}

static void properties(SessionProperties *p)
{
    memset(p, 0, sizeof(*p));
    p->properties.Wnode.BufferSize = sizeof(*p);
    p->properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->properties.Wnode.ClientContext = 1;
    p->properties.LoggerNameOffset = offsetof(SessionProperties, name);
}

static ULONG enable(TRACEHANDLE session, const GUID *guid, USHORT first, USHORT second)
{
    struct { BOOLEAN filter_in; UCHAR reserved; USHORT count; USHORT ids[2]; }
        ids = {TRUE, 0, 2, {first, second}};
    EVENT_FILTER_DESCRIPTOR descriptor = {0};
    descriptor.Ptr = (ULONGLONG)(uintptr_t)&ids;
    descriptor.Size = sizeof(ids);
    descriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS parameters = {0};
    parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    parameters.EnableFilterDesc = &descriptor;
    parameters.FilterDescCount = 1;
    return EnableTraceEx2(session, guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_VERBOSE, 0, 0, 2000, &parameters);
}

// Place one new file beside the signed observer in its nonce-named staging directory.
static HANDLE create_blt_journal(const wchar_t *token)
{
    static const wchar_t owner_prefix[] = L"WinPresentLive-";
    wchar_t path[MAX_PATH];
    DWORD count = GetModuleFileNameW(NULL, path, _countof(path));
    if (!count) return INVALID_HANDLE_VALUE;
    if (count >= _countof(path)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return INVALID_HANDLE_VALUE;
    }
    wchar_t *leaf = wcsrchr(path, L'\\');
    if (leaf) *leaf = 0;
    const wchar_t *parent = wcsrchr(path, L'\\');
    if (!leaf || !parent || wcsncmp(parent + 1, owner_prefix, _countof(owner_prefix)-1) != 0 ||
        wcscmp(parent + _countof(owner_prefix), token) != 0) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return INVALID_HANDLE_VALUE;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        wcslen(path) + _countof(L"\\blt-receipt.bin") > _countof(path) ||
        wcscat_s(path, _countof(path), L"\\blt-receipt.bin")) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
}

static uint32_t record_crc32(const BYTE *bytes, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

typedef struct {
    char magic[8], token[12];
    uint32_t sequence, pid, tid;
    uint16_t event_id;
    uint8_t version, reserved;
    uint16_t payload_bytes, header_bytes;
    int64_t source_qpc, callback_qpc;
} BltRecordHeader;
_Static_assert(sizeof(BltRecordHeader) == 56 && offsetof(BltRecordHeader, source_qpc) == 40,
               "Blt receipt layout must match its little-endian file format");

// The CRC covers the header and verbatim ETW payload. The consumer and subsequent
// FlushFileBuffers return use the same Windows QPC as the public application.
static DWORD persist_blt(Receiver *r, const EVENT_HEADER *h, LONGLONG callback,
                         USHORT length, const BYTE *payload)
{
    BltRecordHeader header = {0};
    BYTE frame[sizeof(header) + 512 + sizeof(uint32_t)];
    if (length > 512) return ERROR_INSUFFICIENT_BUFFER;
    memcpy(header.magic, "SWBLT1", 6);
    memcpy(header.token, r->journal_token, sizeof(header.token));
    header.sequence = (uint32_t)r->published;
    header.pid = h->ProcessId;
    header.tid = h->ThreadId;
    header.event_id = h->EventDescriptor.Id;
    header.version = h->EventDescriptor.Version;
    header.payload_bytes = length;
    header.header_bytes = sizeof(header);
    header.source_qpc = h->TimeStamp.QuadPart;
    header.callback_qpc = callback;
    memcpy(frame, &header, sizeof(header));
    if (length) memcpy(frame + sizeof(header), payload, length);
    DWORD body = (DWORD)sizeof(header) + length;
    uint32_t crc = record_crc32(frame, body);
    memcpy(frame + body, &crc, sizeof(crc));
    DWORD written = 0, size = body + (DWORD)sizeof(crc);
    r->journal_start = ticks();
    if (!WriteFile(r->journal, frame, size, &written, NULL)) return GetLastError();
    if (written != size) return ERROR_WRITE_FAULT;
    if (!FlushFileBuffers(r->journal)) return GetLastError();
    r->journal_finish = ticks();
    return ERROR_SUCCESS;
}

static void WINAPI on_event(PEVENT_RECORD record)
{
    Receiver *r = record->UserContext;
    const EVENT_HEADER *h = &record->EventHeader;
    LONG target = InterlockedCompareExchange(&r->target_pid, 0, 0);
    if (!target || (DWORD)target != h->ProcessId)
        return;
    const USHORT id = h->EventDescriptor.Id;
    const char *provider;
    if (!r->focused_blt && IsEqualGUID(&h->ProviderId, &dxgi) && (id == 42 || id == 43))
        provider = "DXGI";
    else if (IsEqualGUID(&h->ProviderId, &dxgkrnl) &&
             (id == 166 || id == (r->focused_blt ? 286 : 184)))
        provider = "DxgKrnl";
    else
        return;

    static const char digits[] = "0123456789abcdef";
    char hex[1025];
    if (record->UserDataLength > (sizeof(hex) - 1) / 2) {
        fail(r);
        return;
    }
    const BYTE *bytes = record->UserData;
    for (USHORT i = 0; i < record->UserDataLength; ++i) {
        hex[2*i] = digits[bytes[i] >> 4];
        hex[2*i+1] = digits[bytes[i] & 15];
    }
    hex[2*record->UserDataLength] = 0;
    AcquireSRWLockExclusive(&r->output_lock);
    if (r->draining || InterlockedCompareExchange(&r->failed, 0, 0)) {
        ReleaseSRWLockExclusive(&r->output_lock);
        return;
    }
    if (r->published >= 48) {
        fail(r);
    } else {
        ++r->published;
        LONGLONG callback = ticks();
        if (id == 166 && r->journal != INVALID_HANDLE_VALUE && !r->journal_sequence) {
            r->journal_sequence = r->published;
            r->journal_status = persist_blt(r, h, callback, record->UserDataLength, bytes);
        }
        int n = printf("EVENT %ld %s %u %u %lu %lu %lld %lld %u %s\n",
                       r->published, provider, id, h->EventDescriptor.Version,
                       h->ProcessId, h->ThreadId, (long long)h->TimeStamp.QuadPart,
                       (long long)callback, record->UserDataLength, hex);
        if (n < 0 || fflush(stdout) != 0)
            fail(r);
    }
    ReleaseSRWLockExclusive(&r->output_lock);
}

static DWORD WINAPI read_events(void *arg)
{
    Receiver *r = arg;
    r->process_trace_status = ProcessTrace(&r->reader, 1, NULL, NULL);
    if (r->process_trace_status != ERROR_SUCCESS &&
        r->process_trace_status != ERROR_CANCELLED)
        fail(r);
    SetEvent(r->stopping);
    return 0;
}

static DWORD WINAPI read_acks(void *arg)
{
    Receiver *r = arg;
    char line[64];
    while (WaitForSingleObject(r->stopping, 0) == WAIT_TIMEOUT &&
           fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "DRAIN") == 0) {
            AcquireSRWLockExclusive(&r->output_lock);
            if (r->draining || r->published == 0) fail(r);
            else {
                r->draining = TRUE;
                if (printf("DRAINED %ld\n", r->published) < 0 || fflush(stdout)) fail(r);
            }
            ReleaseSRWLockExclusive(&r->output_lock);
            continue;
        }
        if (strcmp(line, "DONE") == 0) {
            AcquireSRWLockExclusive(&r->output_lock);
            r->completed = r->draining && r->acknowledged > 0 && r->acknowledged == r->published;
            if (!r->completed) fail(r);
            ReleaseSRWLockExclusive(&r->output_lock);
            SetEvent(r->stopping);
            return 0;
        }
        if (strncmp(line, "ACK ", 4) != 0) { fail(r); return 1; }
        char *end;
        long seq = strtol(line + 4, &end, 10);
        AcquireSRWLockExclusive(&r->output_lock);
        if (seq <= 0 || *end || seq != r->acknowledged + 1 || seq > r->published) {
            fail(r);
        } else {
            ++r->acknowledged;
            DWORD state = WaitForSingleObject(r->target_process, 0);
            if (state == WAIT_FAILED || printf("RECEIVED %ld %lld %lu\n", seq,
                                             (long long)ticks(), state) < 0 || fflush(stdout))
                fail(r);
        }
        ReleaseSRWLockExclusive(&r->output_lock);
        if (InterlockedCompareExchange(&r->failed, 0, 0)) return 1;
    }
    if (WaitForSingleObject(r->stopping, 0) == WAIT_TIMEOUT) fail(r);
    return 1;
}

// The signed fixture writes client-pid immediately after spawning the child.
// Bind that PID to the executable inside this exact case directory.
static HANDLE bind_target(const wchar_t *pid_path, const wchar_t *image, LONG *pid)
{
    HANDLE f = CreateFileW(pid_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    char number[24] = {0};
    DWORD n = 0;
    BOOL read_ok = ReadFile(f, number, sizeof(number) - 1, &n, NULL);
    CloseHandle(f);
    if (!read_ok || !n || n == sizeof(number) - 1) return NULL;
    while (n > 0 && (number[n - 1] == '\r' || number[n - 1] == '\n')) number[--n] = 0;
    char *end;
    unsigned long id = strtoul(number, &end, 10);
    if (!id || *end || !isdigit((unsigned char)number[0]) || id > LONG_MAX) return NULL;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id);
    if (!process) return NULL;
    wchar_t path[MAX_PATH];
    DWORD chars = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, path, &chars) || _wcsicmp(image, path) != 0) {
        CloseHandle(process);
        return NULL;
    }
    *pid = (LONG)id;
    return process;
}

int wmain(int argc, wchar_t **argv)
{
    const size_t length = _countof(prefix)-1;
    if ((argc != 3 && argc != 4) || wcsncmp(argv[1], prefix, length) ||
        wcslen(argv[1]+length) != 12 || wcsspn(argv[1]+length, L"0123456789abcdef") != 12)
        return 2;
    BOOL early_flush = argc == 4;
    BOOL local_blt = early_flush && wcscmp(argv[3], L"--local-blt-receipt") == 0;
    BOOL focused_blt = early_flush && wcscmp(argv[3], L"--focused-blt-receipt") == 0;
    BOOL precise = local_blt || focused_blt ||
        (early_flush && wcscmp(argv[3], L"--precise-early-flush") == 0);
    if (early_flush && !precise && wcscmp(argv[3], L"--early-flush") != 0) return 2;
    wchar_t directory[MAX_PATH], pid_path[MAX_PATH], image[MAX_PATH];
    DWORD count = GetFullPathNameW(argv[2], MAX_PATH, directory, NULL);
    if (!count || count >= MAX_PATH) return 2;
    while (count > 0 && directory[count - 1] == L'\\') directory[--count] = 0;
    if (!count || count + _countof(L"\\step4-present.exe") > MAX_PATH ||
        wcscpy_s(pid_path, MAX_PATH, directory) || wcscat_s(pid_path, MAX_PATH, L"\\client-pid") ||
        wcscpy_s(image, MAX_PATH, directory) || wcscat_s(image, MAX_PATH, L"\\step4-present.exe"))
        return 2;

    Receiver receiver = {0};
    Receiver *r = &receiver;
    r->focused_blt = focused_blt;
    r->journal = INVALID_HANDLE_VALUE;
    r->stopping = CreateEventW(NULL, TRUE, FALSE, NULL);
    LARGE_INTEGER freq;
    if (!r->stopping) return 3;
    if (!QueryPerformanceFrequency(&freq)) { CloseHandle(r->stopping); return 3; }
    HANDLE pulse = NULL;
    if (precise) {
        pulse = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       SYNCHRONIZE | TIMER_MODIFY_STATE);
        if (!pulse) {
            fprintf(stderr, "CreateWaitableTimerEx status=%lu\n", GetLastError());
            CloseHandle(r->stopping);
            return 3;
        }
    }
    SessionProperties p;
    properties(&p);
    p.properties.BufferSize = 32;
    p.properties.MinimumBuffers = 12;
    p.properties.MaximumBuffers = 48;
    p.properties.FlushTimer = 1;
    p.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    TRACEHANDLE session = 0;
    ULONG status = StartTraceW(&session, argv[1], &p.properties);
    if (status) {
        fprintf(stderr, "StartTrace status=%lu\n", status);
        if (pulse) CloseHandle(pulse);
        CloseHandle(r->stopping);
        return 3;
    }
    if (local_blt) {
        for (size_t i = 0; i < sizeof(r->journal_token)-1; ++i)
            r->journal_token[i] = (char)argv[1][length+i];
        r->journal = create_blt_journal(argv[1]+length);
        if (r->journal == INVALID_HANDLE_VALUE) {
            status = GetLastError();
            fprintf(stderr, "CreateFile journal status=%lu\n", status);
        }
    }
    EVENT_TRACE_LOGFILEW log = {0};
    log.LoggerName = argv[1];
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                           PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log.EventRecordCallback = on_event;
    log.Context = r;
    r->reader = OpenTraceW(&log);
    HANDLE reader = NULL, acks = NULL, process = NULL;
    if (r->reader == INVALID_PROCESSTRACE_HANDLE) status = GetLastError();
    if (!status && !focused_blt) status = enable(session, &dxgi, 42, 43);
    if (!status) status = enable(session, &dxgkrnl, 166, focused_blt ? 286 : 184);
    if (!status && !(reader = CreateThread(NULL, 0, read_events, r, 0, NULL))) status = GetLastError();
    if (!status && !(acks = CreateThread(NULL, 0, read_acks, r, 0, NULL))) status = GetLastError();
    ULONGLONG early_deadline = 0;
    ULONG early_count = 0;
    const DWORD early_interval = focused_blt ? 1 : 2;
    LONGLONG first_early_qpc = 0, last_early_qpc = 0;
    if (!status) {
        if (printf("READY %lu %lld %lld\n", GetCurrentProcessId(), (long long)freq.QuadPart,
                   (long long)ticks()) < 0 || fflush(stdout)) fail(r);
        ULONGLONG deadline = GetTickCount64() + 90000;
        while (WaitForSingleObject(r->stopping, 0) == WAIT_TIMEOUT) {
            if (GetTickCount64() >= deadline) { status = ERROR_TIMEOUT; break; }
            if (!process) {
                LONG pid = 0;
                process = bind_target(pid_path, image, &pid);
                if (process) {
                    if (early_flush) early_deadline = GetTickCount64() + 2000;
                    AcquireSRWLockExclusive(&r->output_lock);
                    r->target_process = process;
                    InterlockedExchange(&r->target_pid, pid);
                    ReleaseSRWLockExclusive(&r->output_lock);
                }
            }
            // The optional short interval covers at most the first two seconds
            // after PID binding. Only our private real-time session is flushed.
            BOOL in_early_window = process && early_flush && GetTickCount64() < early_deadline;
            if (process) {
                SessionProperties flush;
                properties(&flush);
                if (in_early_window && !early_count) first_early_qpc = ticks();
                status = FlushTraceW(session, NULL, &flush.properties);
                if (status) break;
                if (in_early_window) {
                    ++early_count;
                    last_early_qpc = ticks();
                }
            }
            DWORD interval = in_early_window ? early_interval : (process ? 40 : 10);
            DWORD wait;
            if (pulse && in_early_window) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)interval * 10000;
                if (!SetWaitableTimerEx(pulse, &due, 0, NULL, NULL, NULL, 0)) {
                    status = GetLastError();
                    break;
                }
                HANDLE wake[2] = {r->stopping, pulse};
                wait = WaitForMultipleObjects(2, wake, FALSE, 250);
                if (wait == WAIT_TIMEOUT) { status = ERROR_TIMEOUT; break; }
            } else {
                wait = WaitForSingleObject(r->stopping, interval);
            }
            if (wait == WAIT_FAILED) { status = GetLastError(); break; }
        }
    }
    SetEvent(r->stopping);
    properties(&p);
    ULONG stopped = ControlTraceW(session, NULL, &p.properties, EVENT_TRACE_CONTROL_STOP);
    // STOP lets ProcessTrace deliver the final buffers. Close a still-active
    // reader only on a failed stop or drain timeout, and report that as a failure.
    BOOL closed_reader = FALSE;
    if (reader && (stopped || WaitForSingleObject(reader, 5000) != WAIT_OBJECT_0)) {
        fail(r);
        CloseTrace(r->reader);
        closed_reader = TRUE;
        if (WaitForSingleObject(reader, 5000) != WAIT_OBJECT_0) ExitProcess(4);
    }
    if (!closed_reader && r->reader != INVALID_PROCESSTRACE_HANDLE) CloseTrace(r->reader);
    if (acks) {
        ULONGLONG drain_deadline = GetTickCount64() + 5000;
        while (WaitForSingleObject(acks, 50) != WAIT_OBJECT_0) {
            CancelSynchronousIo(acks);
            if (GetTickCount64() >= drain_deadline) ExitProcess(4);
        }
    }
    printf("END %lu %lu %lu %lu %lu %lu %ld %ld\n", status, stopped,
           r->process_trace_status, p.properties.EventsLost, p.properties.RealTimeBuffersLost,
           log.EventsLost, r->published, r->acknowledged);
    fflush(stdout);
    if (early_flush)
        fprintf(stderr, "EARLY_FLUSH %lu 2000 %lu %lld %lld\n",
                early_interval, early_count, (long long)first_early_qpc, (long long)last_early_qpc);
    if (pulse) {
        fprintf(stderr, "EARLY_TIMER high_resolution\n");
        CloseHandle(pulse);
    }
    if (r->journal != INVALID_HANDLE_VALUE) {
        fprintf(stderr, "LOCAL_BLT %ld %lu %lld %lld\n", r->journal_sequence, r->journal_status,
                (long long)r->journal_start, (long long)r->journal_finish);
        CloseHandle(r->journal);
    }
    if (acks) CloseHandle(acks);
    if (reader) CloseHandle(reader);
    if (process) CloseHandle(process);
    CloseHandle(r->stopping);
    return status || stopped || r->process_trace_status || r->failed || !r->completed || r->journal_status || p.properties.EventsLost ||
           p.properties.RealTimeBuffersLost || log.EventsLost || !r->published ||
           r->published != r->acknowledged ? 4 : 0;
}
