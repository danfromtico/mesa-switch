/* SPDX-License-Identifier: MIT */
/* Exercise the actual diagnostic cleanup with failed OS/backend operations.
 * No kernel thread, GPU register or real GPU memory is used by this fixture. */
#define TG_GPU_DIAGNOSTIC_CLEANUP_FIXTURE
#include "../windows/diagnostic_backend.c"
#include <ntimage.h>
static ULONG_PTR image_cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&image_cookie
};
__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
#define CHECK(x) do{if(!(x))ExitProcess(__LINE__);}while(0)
void *memset(void *p,int c,size_t n){UCHAR *d=p;while(n--)*d++=(UCHAR)c;return p;}
void *memcpy(void *p,const void *s,size_t n){UCHAR *d=p;const UCHAR *a=s;while(n--)*d++=*a++;return p;}
static TG_GPU_DIAGNOSTIC_BACKEND backend;
static struct {UINT fail,step;BOOLEAN freed,isolated;} model;
static NTSTATUS step(UINT n){CHECK(n>=model.step);model.step=n;return model.fail==n?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;}
static VOID NTAPI pool_free(PVOID p,ULONG tag)
{CHECK(p==&backend && tag==TAG && !backend.Allocations);model.freed=TRUE;}
VOID (NTAPI * const __imp_ExFreePoolWithTag)(PVOID,ULONG)=pool_free;
NTSTATUS TgGpuStopWorker(TG_GPU_WORKER *w)
{NTSTATUS s=step(1);if(NT_SUCCESS(s))w->Thread=NULL;return s;}
NTSTATUS TgGpuResetWorkQueue(TG_GPU_WORK_QUEUE *q,const struct tg_gpu_power_io *io,
    struct tg_gpu_power *p,struct tg_gpu_clock *c)
{(void)q;(void)io;(void)p;(void)c;CHECK(!backend.Worker.Thread);return step(2);}
NTSTATUS TgGpuStopWorkQueue(TG_GPU_WORK_QUEUE *q)
{NTSTATUS s=step(3);if(NT_SUCCESS(s))q->Engine=NULL;return s;}
NTSTATUS TgGpuStopHardware(TG_GPU_HARDWARE *h)
{
    NTSTATUS s=step(4);CHECK(!backend.Worker.Thread && !backend.QueueInitialized);
    if(!h->Engine)return STATUS_SUCCESS;
    if(!NT_SUCCESS(s))return s;
    model.isolated=TRUE;h->Isolated=TRUE;h->Engine=NULL;backend.Allocations-=2;
    if(model.fail==11){h->Status.restore_result=0x10000;return STATUS_DEVICE_DATA_ERROR;}
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY TgGpuDdiDestroyContext(HANDLE c)
{NTSTATUS s=step(5);CHECK(c && model.isolated);if(NT_SUCCESS(s))--backend.Allocations;return s;}
NTSTATUS APIENTRY TgGpuDdiDestroyDevice(HANDLE d)
{CHECK(d && !backend.Context && model.isolated);return step(6);}
NTSTATUS APIENTRY TgGpuDdiDestroyProcess(HANDLE e,HANDLE p)
{NTSTATUS s=step(7);CHECK(e==&backend.Engine && p && !backend.Device && model.isolated);if(NT_SUCCESS(s))--backend.Allocations;return s;}
NTSTATUS TgGpuStopContextEngine(TG_GPU_CONTEXT_ENGINE *e)
{NTSTATUS s=step(8);CHECK(e==&backend.Engine && !backend.Process && model.isolated);if(NT_SUCCESS(s))backend.Allocations-=2;return s;}
NTSTATUS TgGpuFreePrivateMemory(const DXGKRNL_INTERFACE *d,TG_GPU_PRIVATE_MEMORY *m)
{NTSTATUS s=step(9);CHECK(d==&backend.Dxgk && m==&backend.Data && model.isolated);if(!m->Handle)return STATUS_SUCCESS;if(NT_SUCCESS(s)){m->Handle=NULL;--backend.Allocations;}return s;}
NTSTATUS TgGpuDestroyTableSegment(const DXGKRNL_INTERFACE *d,TG_GPU_TABLE_SEGMENT *t)
{NTSTATUS s=step(10);CHECK(d==&backend.Dxgk && t==&backend.Tables && model.isolated);if(NT_SUCCESS(s)){t->Memory.Handle=NULL;--backend.Allocations;}return s;}
void start(void)
{
    static const char pass[]="PASS: diagnostic cleanup retains backing on failed join/reset/isolation/free and retries safely (controlled substitutes)\r\n";
    ULONG written;UINT failure;
    for(failure=0;failure<=11;++failure) {
        TG_GPU_DIAGNOSTIC_BACKEND *owner=&backend;struct tg_gpu_status s={0};NTSTATUS status;
        memset(&backend,0,sizeof(backend));memset(&model,0,sizeof(model));model.fail=failure;
        backend.Worker.Thread=(HANDLE)1;backend.QueueInitialized=TRUE;backend.EngineInitialized=TRUE;
        backend.Hardware.Engine=&backend.Engine;backend.Queue.Engine=&backend.Engine;
        backend.Context=(TG_GPU_CONTEXT *)1;backend.Device=(TG_GPU_DEVICE *)2;backend.Process=(TG_GPU_PROCESS *)3;
        backend.Data.Handle=(HANDLE)4;backend.Tables.Memory.Handle=(HANDLE)5;backend.Allocations=8;
        status=TgGpuStopDiagnosticBackend(&owner,&s);
        if(!failure || failure==11) {
            CHECK(!owner && model.freed && !backend.Allocations && !s.resources_owned);
            CHECK(status==(failure?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS));
            if(failure)CHECK(s.restore_result==0x10000);
        } else {
            CHECK(!NT_SUCCESS(status) && owner==&backend && !model.freed && s.resources_owned);
            if(failure<=4)CHECK(!model.isolated && backend.Allocations==8);
            model.fail=model.step=0;
            CHECK(TgGpuStopDiagnosticBackend(&owner,&s)==STATUS_SUCCESS);
            CHECK(!owner && model.freed && !backend.Allocations && !s.resources_owned);
        }
    }
    CHECK(WriteFile(GetStdHandle((ULONG)-11),pass,sizeof(pass)-1,&written,NULL));ExitProcess(0);
}
