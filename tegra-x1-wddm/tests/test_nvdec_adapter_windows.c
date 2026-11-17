/* SPDX-License-Identifier: MIT */
/* Actual service/context code with explicit OS, clock and codec substitutes.
 * This exercises ownership and failure paths, never native DMA or decoding. */
#include "../windows/adapter.h"
#include "../windows/nvdec_control.h"
#include "nvdec_escape_abi.h"
#include <ntimage.h>
static ULONG_PTR cookie=0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&cookie
};
__declspec(dllimport) __declspec(noreturn) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
__declspec(dllimport) HANDLE __stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(HANDLE,ULONG,SIZE_T);
__declspec(dllimport) BOOL __stdcall HeapFree(HANDLE,ULONG,void *);
#define CHECK(c) do { if (!(c)) ExitProcess(__LINE__); } while (0)
void *memset(void *p,int v,size_t n) { UCHAR *d=p;while(n--)*d++=(UCHAR)v;return p; }
void *memcpy(void *p,const void *q,size_t n) { UCHAR *d=p;const UCHAR *s=q;while(n--)*d++=*s++;return p; }
static TG_GPU_ADAPTER adapter;
static TG_GPU_PROCESS process;
static TG_GPU_DEVICE *first,*second;
static ULONG pool_live, acquires, releases, creates, destroys, decodes;
static BOOLEAN fail_pool, valid_firmware=TRUE, fail_stop, fail_release, fail_decode, held;
static TG_HOST1X_QUIESCE quiesce;
static PVOID quiesce_context;
static struct tg_nvdec_platform_owner saved_owner;
static _Alignas(8) UCHAR packet[132000];
static ULONG nvregs[TgNvdecApertureCount][0x40000/4], display[0x4000/4], mc[2], hidrev;
static BOOLEAN change_counter, change_source;
static UINT host_rejections;
#define TEST_SYNCPT_OFFSET (0x2100u+0xf80u+4u*191u)
static int backend_object;
static KIRQL level;
static PVOID NTAPI alloc(POOL_FLAGS f,SIZE_T n,ULONG tag)
{
    (void)tag; CHECK(level==PASSIVE_LEVEL && (f==POOL_FLAG_PAGED || f==POOL_FLAG_NON_PAGED));
    if(fail_pool)return NULL;
    PVOID p=HeapAlloc(GetProcessHeap(),8,n);if(p)++pool_live;return p;
}
static VOID NTAPI free_pool(PVOID p,ULONG tag)
{ (void)tag;CHECK(pool_live && HeapFree(GetProcessHeap(),0,p));--pool_live; }
PVOID (NTAPI *const __imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=alloc;
VOID (NTAPI *const __imp_ExFreePoolWithTag)(PVOID,ULONG)=free_pool;
static VOID NTAPI init_mutex(PRKMUTEX m,ULONG l) { CHECK(!l);m->Header.SignalState=1; }
static NTSTATUS NTAPI wait_object(PVOID p,KWAIT_REASON r,KPROCESSOR_MODE m,BOOLEAN a,PLARGE_INTEGER t)
{ (void)r;CHECK(m==KernelMode && !a && !t && !level);PKMUTEX gate=p;CHECK(gate->Header.SignalState==1);gate->Header.SignalState=0;return STATUS_SUCCESS; }
static LONG NTAPI release_mutex(PRKMUTEX m,BOOLEAN w)
{ CHECK(!w && !level && !m->Header.SignalState);m->Header.SignalState=1;return 0; }
static KIRQL NTAPI lock_spin(PKSPIN_LOCK p) { CHECK(!*p && !level);*p=1;level=DISPATCH_LEVEL;return PASSIVE_LEVEL; }
static VOID NTAPI unlock_spin(PKSPIN_LOCK p,KIRQL old) { CHECK(*p && !old);*p=0;level=old; }
static NTSTATUS NTAPI delay(KPROCESSOR_MODE m,BOOLEAN a,PLARGE_INTEGER t)
{
    CHECK(m==KernelMode && !a && t->QuadPart==-500000);
    if(change_counter)++nvregs[TgNvdecHost1x][TEST_SYNCPT_OFFSET/4];
    if(change_source)nvregs[TgNvdecHostSource][0]=0x80000008u;
    return STATUS_SUCCESS;
}
static VOID NTAPI stall(ULONG n) { CHECK(n<=50); }
VOID (NTAPI *const __imp_KeInitializeMutex)(PRKMUTEX,ULONG)=init_mutex;
NTSTATUS (NTAPI *const __imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=wait_object;
LONG (NTAPI *const __imp_KeReleaseMutex)(PRKMUTEX,BOOLEAN)=release_mutex;
KIRQL (NTAPI *const __imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=lock_spin;
VOID (NTAPI *const __imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=unlock_spin;
NTSTATUS (NTAPI *const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=delay;
VOID (NTAPI *const __imp_KeStallExecutionProcessor)(ULONG)=stall;

NTSTATUS TgHost1xClockAcquire(TG_GPU_PLATFORM *p,TG_HOST1X_QUIESCE q,PVOID ctx,TG_HOST1X_CLOCK_LEASE **out,const char **stage)
{
    CHECK(p==&adapter.Platform && !held && !*out && stage && *stage);++acquires;
    *stage="fixture Host1x lease admitted";
    held=TRUE;quiesce=q;quiesce_context=ctx;*out=(TG_HOST1X_CLOCK_LEASE *)&held;return STATUS_SUCCESS;
}
static ULONG core_floor_uv=775000u;
BOOLEAN TgHost1xClockHeld(TG_HOST1X_CLOCK_LEASE *p,ULONG *pllp,ULONG *core_floor)
{ if(!p || !held)return FALSE;if(pllp)*pllp=0x48115408u;if(core_floor)*core_floor=core_floor_uv;return TRUE; }
NTSTATUS TgHost1xClockRelease(TG_HOST1X_CLOCK_LEASE **p)
{
    if(!*p)return STATUS_SUCCESS;
    CHECK(adapter.Queue.DispatchGate.Header.SignalState==1);
    CHECK(adapter.Video.Gate.Header.SignalState==0);++releases;
    NTSTATUS s=quiesce(quiesce_context);
    if(s!=STATUS_SUCCESS)return s;
    if(fail_release)return STATUS_IO_TIMEOUT;
    *p=NULL;held=FALSE;return STATUS_SUCCESS;
}
BOOLEAN TgNvdecFirmwareValid(const void *data,SIZE_T bytes,const UINT8 sha[32])
{ CHECK(data && sha && bytes==128000u);return valid_firmware; }
NTSTATUS TgNvdecCreate(const DXGKRNL_INTERFACE *d,const struct tg_nvdec_platform_io *io,
    const struct tg_nvdec_platform_owner *o,UINT32 ch,UINT32 sync,struct tg_nvdec_win **out)
{
    CHECK(d==&adapter.Engine.Dxgk && io->ctx==&adapter && ch==11 && sync==191 && !*out);
    CHECK(o->acquire_host1x(o->ctx) && o->host1x_live(o->ctx));saved_owner=*o;
    ++creates;*out=(struct tg_nvdec_win *)&backend_object;return STATUS_SUCCESS;
}
NTSTATUS TgNvdecQuiesce(struct tg_nvdec_win *w)
{ CHECK(!adapter.Queue.DispatchGate.Header.SignalState);return w && fail_stop?STATUS_DEVICE_BUSY:STATUS_SUCCESS; }
NTSTATUS TgNvdecDestroy(struct tg_nvdec_win *w)
{
    if(!w)return STATUS_SUCCESS;
    if(TgNvdecQuiesce(w)!=STATUS_SUCCESS || !saved_owner.release_host1x(saved_owner.ctx))return STATUS_DEVICE_BUSY;
    ++destroys;return STATUS_SUCCESS;
}
NTSTATUS TgNvdecLoadFirmware(struct tg_nvdec_win *w,const void *data,SIZE_T n,const UINT8 sha[32])
{ CHECK(w && data && n==128000 && sha && held);return STATUS_SUCCESS; }
NTSTATUS TgNvdecConfigure(struct tg_nvdec_win *w,UINT32 width,UINT32 height,UINT32 *pitch,UINT32 *ah,UINT32 *size)
{ CHECK(w && width==16 && height==16);*pitch=256;*ah=16;*size=6144;return STATUS_SUCCESS; }
NTSTATUS TgNvdecDecode(struct tg_nvdec_win *w,const struct tg_nvdec_h264_pic *p,const UINT8 *b,UINT32 n,
    const UINT32 *offsets,UINT32 count,void *out,SIZE_T size,struct tg_nvdec_status *st,UINT32 *expected,UINT32 *observed)
{
    CHECK(w && p && b && n==3 && count==1 && size==6144);
    CHECK(!((ULONG_PTR)offsets&3u) && offsets[0]==0 && offsets[1]==3);++decodes;
    memset(out,0x80,size);st->mbs_correctly_decoded=1;*expected=8;*observed=fail_decode?7:8;
    return fail_decode?STATUS_IO_TIMEOUT:STATUS_SUCCESS;
}
const char *TgNvdecLastError(struct tg_nvdec_win *w) { (void)w;return "CPU fixture"; }
BOOLEAN TgNvdecFirmwareReady(struct tg_nvdec_win *w) { return w!=NULL; }
BOOLEAN TgNvdecSurfacesReady(struct tg_nvdec_win *w) { return w!=NULL; }
void TgNvdecGetConfig(struct tg_nvdec_win *w,UINT32 *width,UINT32 *height,UINT32 *pitch,UINT32 *ah,UINT32 *size)
{ if(!w)return;*width=16;*height=16;*pitch=256;*ah=16;*size=6144; }

static struct tg_nvdec_escape *make(UINT cmd,UINT in,UINT out,UINT64 session)
{
    memset(packet,0,sizeof(packet));
    struct tg_nvdec_escape *h=(void *)packet;
    *h=(struct tg_nvdec_escape){TG_NVDEC_ESCAPE_MAGIC,TG_NVDEC_ESCAPE_VERSION,
        (UINT)sizeof(*h)+(in>out?in:out),cmd,in,out,0,0,session};return h;
}
static NTSTATUS call(DRIVER_INITIALIZATION_DATA *d,TG_GPU_DEVICE *device)
{
    struct tg_nvdec_escape *h=(void *)packet;
    DXGKARG_ESCAPE e={.hDevice=device,.Flags={.Value=1},.pPrivateDriverData=packet,.PrivateDriverDataSize=h->size};
    return d->DxgkDdiEscape(&adapter,&e);
}
static void prepare_load(void)
{
    struct tg_nvdec_escape *h=make(TG_NVDEC_ESCAPE_LOAD,sizeof(struct tg_nvdec_load_request)+128000,
        sizeof(struct tg_nvdec_load_result),0);
    struct tg_nvdec_load_request *q=(void *)(h+1);q->version=1;q->size=128000;
}
static TG_GPU_DEVICE *device(void)
{
    TG_GPU_DEVICE *d=alloc(POOL_FLAG_NON_PAGED,sizeof(*d),0);
    d->Engine=&adapter.Engine;d->Process=&process;++process.Devices;
    InsertTailList(&adapter.Engine.Devices,&d->Link);return d;
}
static const char *check_host_failure(DRIVER_INITIALIZATION_DATA *ddi,ULONG check,ULONG index)
{
    prepare_load();
    struct tg_nvdec_escape *h=(void *)packet;
    CHECK(call(ddi,first)==0 && h->result==STATUS_DEVICE_NOT_READY);
    CHECK(adapter.Video.HostCheck==check && adapter.Video.HostIndex==index);
    CHECK(!held && !creates && !first->VideoReferences && !adapter.Video.Device && !adapter.Video.Clock);
    CHECK(pool_live==2);++host_rejections;
    const struct tg_nvdec_load_result *r=(void *)(h+1);
    CHECK(r->error[TG_NVDEC_ERROR_SIZE-1u]==0);
    return r->error;
}
static BOOLEAN text_equal(const char *a,const char *b)
{ while(*a && *a==*b){++a;++b;}return *a==*b; }
void start(void)
{
    DRIVER_INITIALIZATION_DATA ddi={0};
    InitializeListHead(&adapter.Engine.Devices);process.Engine=&adapter.Engine;
    adapter.EngineReady=adapter.QueueReady=TRUE;adapter.State=TgGpuAdapterReady;
    init_mutex(&adapter.Queue.DispatchGate,0);TgNvdecAdapterInitialize(&adapter);TgNvdecRegisterEscape(&ddi);
    first=device();second=device();
    struct tg_nvdec_escape *h=(void *)packet;
    prepare_load();CHECK(call(&ddi,(TG_GPU_DEVICE *)0xbad)==STATUS_INVALID_HANDLE && !acquires);
    valid_firmware=FALSE;CHECK(call(&ddi,first)==0 && h->result==STATUS_INVALID_PARAMETER && !acquires);
    valid_firmware=TRUE;prepare_load();CHECK(call(&ddi,first)==0 && h->result==STATUS_DEVICE_NOT_READY && !acquires);
    for(UINT i=0;i<TgNvdecApertureCount;++i)adapter.Resources.NvdecMapping[i]=nvregs[i];
    adapter.Resources.Mapping[TgGpuHidrev]=&hidrev;adapter.Resources.Mapping[TgGpuMcFlush]=mc;
    adapter.Resources.DisplayRegisters=display;hidrev=0x12127;
    nvregs[TgNvdecHostSource][0]=0x80000003; /* Windows readback:163.2MHz */
    for(UINT c=0;c<12;++c)nvregs[TgNvdecHost1x][c*0x4000/4]=1u<<11;
    prepare_load();h->input_bytes=MAXULONG;CHECK(call(&ddi,first)==STATUS_INVALID_PARAMETER && !acquires);
    prepare_load();fail_pool=TRUE;CHECK(call(&ddi,first)==STATUS_NO_MEMORY && first->VideoReferences==0);fail_pool=FALSE;
    core_floor_uv=637500u;prepare_load();
    CHECK(call(&ddi,first)==0 && h->result==STATUS_DEVICE_NOT_READY && !held && !creates && !first->VideoReferences);
    core_floor_uv=775000u;
    /* Native channel0 snapshot: FIFO empty, stopped, unequal GET/PUT.
     * It must not block or be rewritten by the channel11 decoder. */
    ULONG *foreign=nvregs[TgNvdecHost1x];
    foreign[0]=0x80100840u;foreign[0x18/4]=0;foreign[0x1c/4]=0x21d84cf0u;foreign[0x24/4]=1;
    /* An unrelated active channel also stays outside this reservation. */
    ULONG *active=nvregs[TgNvdecHost1x]+0x4000u/4u;
    active[0]=0;active[0x18/4]=0x200u;active[0x1c/4]=0x100u;active[0x24/4]=0;
    ULONG *channel=nvregs[TgNvdecHost1x]+11u*0x4000u/4u;
    channel[0]=0;channel[0x18/4]=0x01020304u;channel[0x1c/4]=0x05060708u;channel[0x24/4]=1;
    channel[0x14/4]=0x90000000u;channel[0x20/4]=0x90004004u;
    CHECK(text_equal(check_host_failure(&ddi,TgNvdecHostChannelFifo,11),
        "Host1x check=00000003 index=0000000b a=00000000 b=01020304 c=05060708 d=00000001 e=90000000 f=90004004 pll=48115408 src=80000003 uv=000bd358"));
    CHECK(adapter.Video.HostValue[4]==0x90000000u && adapter.Video.HostValue[5]==0x90004004u);
    channel[0]=1u<<11;channel[0x18/4]=channel[0x1c/4]=0;
    const ULONG bad_ctrl[]={0u,5u,MAXULONG};
    for (UINT i=0;i<RTL_NUMBER_OF(bad_ctrl);++i) {
        channel[0x24/4]=bad_ctrl[i];
        check_host_failure(&ddi,TgNvdecHostChannelDmaState,11);
        CHECK(!adapter.Video.HostValue[1] && !adapter.Video.HostValue[2] && adapter.Video.HostValue[3]==bad_ctrl[i]);
    }
    CHECK(adapter.Video.HostValue[4]==0x90000000u && adapter.Video.HostValue[5]==0x90004004u);
    /* The actual native stopped first-use state must reach the backend,
     * which separately verifies reset/gated engines and initializes its ring. */
    channel[0x18/4]=0;channel[0x1c/4]=0x00121010u;channel[0x24/4]=1;
    channel[0x14/4]=0x108e00a8u;channel[0x20/4]=0xa800a800u;
    ULONG *irq=&nvregs[TgNvdecHost1x][(0x2100u+0xf20u+4u*(191u/32u))/4u];
    *irq=1u<<31;check_host_failure(&ddi,TgNvdecHostSyncpointIrq,191);CHECK(adapter.Video.HostValue[0]==*irq);*irq=0;
    CHECK(!adapter.Video.HostValue[4] && !adapter.Video.HostValue[5]);
    ULONG *counter=&nvregs[TgNvdecHost1x][TEST_SYNCPT_OFFSET/4];
    *counter=MAXULONG;check_host_failure(&ddi,TgNvdecHostSyncpointValue,191);*counter=7;
    change_counter=TRUE;check_host_failure(&ddi,TgNvdecHostSyncpointChanged,191);
    CHECK(adapter.Video.HostValue[0]==7 && adapter.Video.HostValue[1]==8);change_counter=FALSE;
    change_source=TRUE;check_host_failure(&ddi,TgNvdecHostClockChanged,0x180u);
    CHECK(adapter.Video.HostValue[0]==0x80000008u);change_source=FALSE;nvregs[TgNvdecHostSource][0]=0x80000003u;
    display[0x028]=(1u<<8)|191u;check_host_failure(&ddi,TgNvdecHostDisplaySyncpoint,0x028u);display[0x028]=0;
    adapter.Resources.DisplayRegisters=NULL;check_host_failure(&ddi,TgNvdecHostNoDisplay,0);adapter.Resources.DisplayRegisters=display;
    prepare_load();CHECK(call(&ddi,first)==0 && h->result==0 && h->session && held && creates==1);
    CHECK(!adapter.Video.HostCheck); /* Successful admission clears stale evidence. */
    CHECK(channel[0x1c/4]==0x00121010u && channel[0x14/4]==0x108e00a8u && channel[0x20/4]==0xa800a800u);
    UINT64 session=h->session;CHECK(first->VideoReferences==1);
    make(TG_NVDEC_ESCAPE_CLOSE,0,0,session);CHECK(call(&ddi,second)==0 && h->result==STATUS_INVALID_HANDLE && held);
    make(TG_NVDEC_ESCAPE_CLOSE,0,0,session+1);CHECK(call(&ddi,first)==0 && h->result==STATUS_INVALID_HANDLE && held);
    for(UINT bad=0;bad<2;++bad) {
        h=make(TG_NVDEC_ESCAPE_DECODE,sizeof(struct tg_nvdec_decode_request)+3+8,
            sizeof(struct tg_nvdec_decode_result)+6144,session);
        struct tg_nvdec_decode_request *q=(void *)(h+1);q->version=1;q->bitstream_size=3;q->slice_count=1;q->output_size=6144;
        UINT32 offsets[2]={0,3};memcpy((UCHAR *)(q+1)+3,offsets,sizeof(offsets));fail_decode=bad!=0;
        CHECK(call(&ddi,first)==0);
        struct tg_nvdec_decode_result *r=(void *)(h+1);
        CHECK(r->bytes_written==(bad?0u:6144u) && h->result==(bad?STATUS_IO_TIMEOUT:STATUS_SUCCESS));
        CHECK(((UCHAR *)(r+1))[0]==(bad?0:0x80));
    }
    CHECK(decodes==2);
    fail_stop=TRUE;CHECK(TgNvdecAdapterStop(&adapter)==STATUS_DEVICE_BUSY && held && first->VideoReferences==1 && !destroys);
    fail_stop=FALSE;fail_release=TRUE;
    CHECK(TgGpuDdiDestroyDevice(first)==STATUS_DEVICE_BUSY && first->Closing && first->VideoReferences==1 && held);
    fail_release=FALSE;CHECK(TgGpuDdiDestroyDevice(first)==0 && !held && destroys==1 && !adapter.Video.Device);
    prepare_load();CHECK(call(&ddi,second)==0 && h->result==0 && h->session!=session && held);
    CHECK(TgGpuDdiDestroyDevice(second)==0 && !held && destroys==2 && !process.Devices && !pool_live);
    CHECK(foreign[0]==0x80100840u && !foreign[0x18/4] && foreign[0x1c/4]==0x21d84cf0u && foreign[0x24/4]==1);
    CHECK(!active[0] && active[0x18/4]==0x200u && active[0x1c/4]==0x100u && !active[0x24/4]);
    CHECK(acquires==3+host_rejections && releases==4+host_rejections);
    ULONG written;const char ok[]="PASS: NVDEC escape ownership, packet bounds, retained stop failures and lock order (CPU only)\n";
    WriteFile(GetStdHandle((ULONG)-11),ok,sizeof(ok)-1,&written,NULL);ExitProcess(0);
}
