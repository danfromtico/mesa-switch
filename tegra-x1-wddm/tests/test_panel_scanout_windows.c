/* SPDX-License-Identifier: MIT
 * Real WDK DDIs and display backend; explicit allocation/MMIO/time substitutes.
 * This does not drive a physical GPU or demonstrate visible hardware rotation.
 */
#include "../windows/adapter.h"
#include "../windows/allocation.h"
#include "../windows/panel_snapshot.h"
#include "../platform/t214_resources.h"
#include <ntimage.h>

__declspec(dllimport) void __stdcall ExitProcess(ULONG);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(ULONG);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE,const void *,ULONG,ULONG *,void *);
static ULONG_PTR cookie=0x2b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
    .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONGLONG)&cookie};
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
void *memset(void *p,int value,size_t n)
{ UCHAR *d=p;while(n--)*d++=(UCHAR)value;return p; }
void *memcpy(void *p,const void *s,size_t n)
{ UCHAR *d=p;const UCHAR *b=s;while(n--)*d++=*b++;return p; }
#define DC_CHECK CHECK
#include "dc_model.h"

KUSER_SHARED_DATA ki_user_shared_data;
static struct display_model model;
static TG_GPU_ADAPTER adapter;
static KIRQL level;
static BOOLEAN auto_latch=TRUE;
static BOOLEAN corrupt_active_size_on_latch=FALSE;
static BOOLEAN drop_target_active_on_latch=FALSE;
static UINT stalls, notifications, sync_calls;
static ULONGLONG reported_present;
static BOOLEAN debug_privilege = TRUE;
static BOOLEAN NTAPI privilege(LUID value, KPROCESSOR_MODE mode)
{ CHECK(value.LowPart==SE_DEBUG_PRIVILEGE && !value.HighPart && mode==UserMode);return debug_privilege; }
static KPROCESSOR_MODE NTAPI previous_mode(VOID) { return UserMode; }
BOOLEAN (NTAPI *const __imp_SeSinglePrivilegeCheck)(LUID,KPROCESSOR_MODE)=privilege;
KPROCESSOR_MODE (NTAPI *const __imp_ExGetPreviousMode)(VOID)=previous_mode;

static KIRQL NTAPI current_irql(VOID) { return level; }
KIRQL (NTAPI *const __imp_KeGetCurrentIrql)(VOID)=current_irql;
static void advance(ULONGLONG ticks)
{
    /* The test variants use the WDK's relocatable shared-time support. */
    *(volatile ULONGLONG *)&ki_user_shared_data.InterruptTime+=ticks;
    if(auto_latch)latch(&model,true);
    if(corrupt_active_size_on_latch) {
        model.assembly[T214_DC_ACTIVE_SIZE]=0u;
        model.active[T214_DC_ACTIVE_SIZE]=0u;
    }
    if(drop_target_active_on_latch)
        adapter.Panel.TargetActive=FALSE;
}
static VOID NTAPI stall(ULONG us)
{ CHECK((level==PASSIVE_LEVEL || level==15) && us==1);++stalls;advance(1000); }
static NTSTATUS NTAPI delay(KPROCESSOR_MODE mode,BOOLEAN alert,PLARGE_INTEGER interval)
{ CHECK(level==PASSIVE_LEVEL && mode==KernelMode && !alert && interval->QuadPart==-10000);advance(10000);return STATUS_SUCCESS; }
VOID (NTAPI *const __imp_KeStallExecutionProcessor)(ULONG)=stall;
NTSTATUS (NTAPI *const __imp_KeDelayExecutionThread)(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=delay;
static NTSTATUS APIENTRY synchronize(HANDLE handle,PKSYNCHRONIZE_ROUTINE routine,
    PVOID context,ULONG message,PBOOLEAN result)
{
    CHECK(handle==&adapter && !message && level<=DISPATCH_LEVEL);
    KIRQL old=level;level=3;++sync_calls;*result=routine(context);level=old;
    return STATUS_SUCCESS;
}
static VOID APIENTRY interrupt(HANDLE handle,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data)
{
    CHECK(handle==&adapter && level==3 && data->InterruptType==DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2);
    CHECK(data->CrtcVsyncWithMultiPlaneOverlay2.VidPnTargetId==7);
    CHECK(data->CrtcVsyncWithMultiPlaneOverlay2.PhysicalAdapterMask==1);
    CHECK(data->Flags.Value==1); /* Valid mask; ordinary VBlank, not HSync. */
    const UINT count=data->CrtcVsyncWithMultiPlaneOverlay2.MultiPlaneOverlayVsyncInfoCount;
    CHECK(count<=1);
    reported_present=count?data->CrtcVsyncWithMultiPlaneOverlay2.pMultiPlaneOverlayVsyncInfo[0].PresentId:0;
    ++notifications;
}
static BOOLEAN APIENTRY queue_dpc(HANDLE handle)
{ CHECK(handle==&adapter && level==3);return TRUE; }

/* Child and scanout DDIs are exercised through the display registration. */
NTSTATUS TgGpuCreateScanoutSegment(const DXGKRNL_INTERFACE *dxgk,TG_GPU_TABLE_SEGMENT *tables)
{ (void)dxgk;(void)tables;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }
NTSTATUS APIENTRY TgGpuDdiGetStandardAllocationDriverData(HANDLE context,DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *args)
{ (void)context;(void)args;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }
void TgGpuRegisterPanelVidPn(DRIVER_INITIALIZATION_DATA *ddis)
{ (void)ddis; /* Exercised by test-panel-timings. */ }
NTSTATUS TgGpuDdiStopDevice(PVOID context)
{ (void)context;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }
NTSTATUS TgGpuDdiRemoveDevice(PVOID context)
{ (void)context;CHECK(FALSE);return STATUS_NOT_IMPLEMENTED; }

static TG_GPU_PRIMARY_ALLOCATION_REQUEST allocations[4];
/* Real metadata ownership/rejection is covered by test-context. This fixture
 * supplies four known immutable KMD handles to exercise display DDIs. */
NTSTATUS TgGpuGetScanoutSurface(TG_GPU_CONTEXT_ENGINE *engine,HANDLE handle,
    TG_GPU_PRIMARY_ALLOCATION_REQUEST *out)
{
    CHECK(engine==&adapter.Engine && out);
    for(UINT i=0;i<4;++i)if(handle==&allocations[i]){*out=allocations[i];return STATUS_SUCCESS;}
    return STATUS_INVALID_PARAMETER;
}
NTSTATUS TgGpuResolveScanoutAddress(const TG_GPU_TABLE_SEGMENT *tables,
    UINT64 address,UINT64 bytes,UINT64 *out)
{
    CHECK(tables==&adapter.Tables && bytes==T214_GPU_POST_FB_SIZE);
    if((address&4095) || address<0x98000000 || address>0x99000000-bytes)
        return STATUS_INVALID_ADDRESS;
    *out=address;return STATUS_SUCCESS;
}

static void signal_vblank(const DRIVER_INITIALIZATION_DATA *d)
{
    model.assembly[T214_DC_INT_STATUS]|=T214_DC_VBLANK_INT;
    KIRQL old=level;level=3;
    CHECK(d->DxgkDdiInterruptRoutine(&adapter,0));
    level=old;
}
static void active_is(UINT64 address,BOOLEAN visible)
{
    struct tg_t214_dc_scanout active;
    CHECK(tg_t214_dc_latched_surface(&adapter.Panel.Dc,&active));
    CHECK(active.address==address && active.visible==(visible!=FALSE));
    CHECK(!(model.active[T214_DC_WIN_A_OPTIONS]&0x15));
    CHECK(model.active[T214_DC_WIN_A_STRIDE]==2880u);
}

static void snapshot_controls(const DRIVER_INITIALIZATION_DATA *d)
{
    static UINT32 pixels[T214_GPU_POST_FB_SIZE/4];
    struct tg_gpu_panel_snapshot s={.magic=TG_GPU_PANEL_SNAPSHOT_MAGIC,
        .version=TG_GPU_PANEL_SNAPSHOT_VERSION,.size=sizeof(s)};
    CHECK(sizeof(s)==192);
    DXGKARG_ESCAPE request={.Flags={.HardwareAccess=1},.pPrivateDriverData=&s,
        .PrivateDriverDataSize=sizeof(s)};
    const UINT before=model.writes;
    debug_privilege=FALSE;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_PRIVILEGE_NOT_HELD && model.writes==before);
    debug_privilege=TRUE;
    request.Flags.HardwareAccess=0;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_INVALID_PARAMETER && model.writes==before);
    request.Flags.HardwareAccess=1;
    --request.PrivateDriverDataSize;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_INVALID_PARAMETER && model.writes==before);
    ++request.PrivateDriverDataSize;
    ++s.version;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_GRAPHICS_DRIVER_MISMATCH && model.writes==before);
    --s.version;
    adapter.Tables.Scanout=(TG_GPU_PRIVATE_MEMORY){.Handle=&adapter,.CpuAddress=pixels,
        .SystemPhysicalAddress={.QuadPart=0x98c00000},.Bytes=sizeof(pixels)};
    const UINT32 colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffff00};
    for(UINT i=0;i<4;++i)pixels[(i&2?960:320)*720+(i&1?540:180)]=colors[i];
    model.assembly[T214_DC_INT_STATUS]=T214_DC_VBLANK_INT | (1u<<8);
    model.assembly[T214_DC_WIN_A_UNDERFLOW]=13;
    /* Assembly is not active scanout. The reader must select and restore the
     * active bank without copying it to armed/active or consuming an IRQ. */
    UINT saved=model.assembly[T214_DC_WIN_A_ADDR];
    model.assembly[T214_DC_WIN_A_ADDR]=0xdead0000;
    const TG_GPU_PANEL_FLIP displayed=adapter.Panel.Displayed;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_SUCCESS);
    CHECK(s.flags==7 && s.address==0x98c00000 && s.rotation==0);
    CHECK(s.width==720 && s.height==1280 && s.pitch==2880 && !s.v_offset);
    for(UINT i=0;i<4;++i)CHECK(s.pixels[i].bgra==colors[i]);
    CHECK(s.underflow==13 && s.interrupt_status==(T214_DC_VBLANK_INT | (1u<<8)));
    CHECK(model.writes==before+2 && model.assembly[T214_DC_STATE_ACCESS]==0);
    CHECK(model.assembly[T214_DC_WIN_A_ADDR]==0xdead0000 &&
        model.assembly[T214_DC_INT_STATUS]==s.interrupt_status &&
        adapter.Panel.Displayed.PresentId==displayed.PresentId && !adapter.Panel.Pending.Address);
    model.assembly[T214_DC_STATE_CONTROL]=T214_DC_WIN_A_ACT_REQ;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_SUCCESS &&
        s.flags==TG_GPU_PANEL_SNAPSHOT_LAYOUT && !s.pixels[0].bgra);
    model.assembly[T214_DC_STATE_CONTROL]=0;
    ++adapter.Tables.Scanout.SystemPhysicalAddress.QuadPart;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_SUCCESS && s.flags==3 && !s.pixels[0].bgra);
    --adapter.Tables.Scanout.SystemPhysicalAddress.QuadPart;
    --adapter.Tables.Scanout.Bytes;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_SUCCESS && s.flags==3);
    adapter.Tables.Scanout=(TG_GPU_PRIVATE_MEMORY){0};
    model.assembly[T214_DC_WIN_A_ADDR]=saved;
    model.assembly[T214_DC_INT_STATUS]=0;
    const UINT stopped_writes=model.writes;
    adapter.Panel.Stopping=TRUE;
    CHECK(d->DxgkDdiEscape(&adapter,&request)==STATUS_DEVICE_NOT_READY && model.writes==stopped_writes);
    adapter.Panel.Stopping=FALSE;
}

static NTSTATUS APIENTRY create_monitor_mode(D3DKMDT_HMONITORSOURCEMODESET set,
    D3DKMDT_MONITOR_SOURCE_MODE **mode)
{
    *mode=(D3DKMDT_MONITOR_SOURCE_MODE *)set;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY add_monitor_mode(D3DKMDT_HMONITORSOURCEMODESET set,
    D3DKMDT_MONITOR_SOURCE_MODE *mode)
{
    CHECK((PVOID)set==(PVOID)mode && mode->Preference==D3DKMDT_MP_PREFERRED);
    return STATUS_SUCCESS;
}

static void check_reported_child(DRIVER_INITIALIZATION_DATA *d)
{
    struct { DXGK_CHILD_DESCRIPTOR slots[2]; ULONG guard; } out;
    const ULONG slot=sizeof(out.slots[0]);
    DXGK_CHILD_STATUS status={.Type=StatusConnection,.ChildUid=adapter.Panel.Boot.TargetId};
    DXGK_DEVICE_DESCRIPTOR desc={0};
    const UINT writes=model.writes;
    memset(&out,0xa5,sizeof(out));
    CHECK(d->DxgkDdiQueryChildRelations(&adapter,out.slots,slot-1)==STATUS_BUFFER_TOO_SMALL);
    CHECK(d->DxgkDdiQueryChildRelations(&adapter,out.slots,slot)==STATUS_BUFFER_TOO_SMALL);
    CHECK(d->DxgkDdiQueryChildRelations(&adapter,out.slots,slot+1)==STATUS_BUFFER_TOO_SMALL);
    for(UINT i=0;i<sizeof(out);++i) CHECK(((UCHAR *)&out)[i]==0xa5);
    CHECK(d->DxgkDdiQueryChildRelations(&adapter,out.slots,slot*2)==STATUS_SUCCESS);
    CHECK(out.slots[0].ChildDeviceType==TypeVideoOutput && out.slots[0].ChildUid==status.ChildUid &&
          out.slots[0].AcpiUid==adapter.Panel.Boot.AcpiId &&
          out.slots[0].ChildCapabilities.HpdAwareness==HpdAwarenessAlwaysConnected);
    for(UINT i=0;i<slot;++i) CHECK(((UCHAR *)&out.slots[1])[i]==0);
    CHECK(out.guard==0xa5a5a5a5u);
    memset(&out,0xa5,sizeof(out));
    CHECK(d->DxgkDdiQueryChildRelations(&adapter,out.slots,sizeof(out))==STATUS_SUCCESS);
    for(UINT i=0;i<slot;++i) CHECK(((UCHAR *)&out.slots[1])[i]==0);
    CHECK(out.guard==0xa5a5a5a5u);
    CHECK(d->DxgkDdiQueryChildStatus(&adapter,&status,TRUE)==STATUS_SUCCESS && status.HotPlug.Connected);
    CHECK(d->DxgkDdiQueryDeviceDescriptor(&adapter,status.ChildUid,&desc)==STATUS_MONITOR_NO_DESCRIPTOR);
    DXGKARG_UPDATEMONITORLINKINFO link={.VideoPresentTargetId=status.ChildUid};
    link.MonitorLinkInfo.Capabilities.Value=MAXULONG;
    CHECK(d->DxgkDdiUpdateMonitorLinkInfo(&adapter,&link)==STATUS_SUCCESS &&
          !link.MonitorLinkInfo.Capabilities.Value);
    D3DKMDT_MONITOR_SOURCE_MODE mode={0};
    DXGK_MONITORSOURCEMODESET_INTERFACE modes={.pfnCreateNewModeInfo=create_monitor_mode,.pfnAddMode=add_monitor_mode};
    DXGKARG_RECOMMENDMONITORMODES request={.VideoPresentTargetId=status.ChildUid,
        .hMonitorSourceModeSet=(D3DKMDT_HMONITORSOURCEMODESET)&mode,.pMonitorSourceModeSetInterface=&modes};
    CHECK(d->DxgkDdiRecommendMonitorModes(&adapter,&request)==STATUS_SUCCESS &&
          mode.VideoSignalInfo.ActiveSize.cx==TgGpuPanelSignal.ActiveSize.cx);
    request.VideoPresentTargetId=link.VideoPresentTargetId=++status.ChildUid;
    CHECK(d->DxgkDdiUpdateMonitorLinkInfo(&adapter,&link)==STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET);
    CHECK(d->DxgkDdiRecommendMonitorModes(&adapter,&request)==STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET);
    CHECK(d->DxgkDdiQueryChildStatus(&adapter,&status,TRUE)==STATUS_INVALID_PARAMETER);
    CHECK(d->DxgkDdiQueryDeviceDescriptor(&adapter,status.ChildUid,&desc)==STATUS_INVALID_PARAMETER);
    CHECK(model.writes==writes);
}

void start(void)
{
    DRIVER_INITIALIZATION_DATA d={0};TgGpuRegisterPanelLifecycle(&d);
    DXGK_CHILD_DESCRIPTOR unknown={0};
    DXGK_CHILD_STATUS unknown_status={.Type=StatusConnection};
    DXGK_DEVICE_DESCRIPTOR unknown_desc={0};
    CHECK(d.DxgkDdiQueryChildRelations(&adapter,&unknown,sizeof(unknown))==STATUS_DEVICE_NOT_READY);
    CHECK(d.DxgkDdiQueryChildStatus(&adapter,&unknown_status,TRUE)==STATUS_DEVICE_NOT_READY);
    CHECK(d.DxgkDdiQueryDeviceDescriptor(&adapter,0,&unknown_desc)==STATUS_DEVICE_NOT_READY);
    initialize(&model);
    const struct tg_t214_dc_io io={&model,read_reg,write_reg};
    CHECK(tg_t214_dc_acquire(&adapter.Panel.Dc,&io)==TG_T214_DC_OK);
    latch(&model,true);
    adapter.Panel.Boot.TargetId=7;adapter.Panel.Boot.AcpiId=0x1122;
    adapter.Panel.ChildReported=TRUE; /* The display StartDevice returned one child. */
    adapter.Panel.SourceVisible=adapter.Panel.TargetActive=TRUE;
    adapter.Panel.Displayed=(TG_GPU_PANEL_FLIP){.Address=T214_GPU_POST_FB_BASE,.Visible=TRUE};
    adapter.Engine.Dxgk=(DXGKRNL_INTERFACE){.DeviceHandle=&adapter,
        .DxgkCbSynchronizeExecution=synchronize,.DxgkCbNotifyInterrupt=interrupt,.DxgkCbQueueDpc=queue_dpc};
    check_reported_child(&d);
    CHECK(d.DxgkDdiControlInterrupt(&adapter,DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY2,TRUE)==STATUS_SUCCESS);
    for(UINT r=0;r<4;++r) {
        allocations[r]=(TG_GPU_PRIMARY_ALLOCATION_REQUEST){
            .Buffer={TG_GPU_ALLOCATION_ABI,TG_GPU_ALLOCATION_CPU_VISIBLE|TG_GPU_ALLOCATION_DISPLAY_PRIMARY,T214_GPU_POST_FB_SIZE},
            .Format=D3DDDIFMT_A8R8G8B8,.Rotation=D3DDDI_ROTATION_IDENTITY,.Width=720,.Height=1280,
            .Pitch=2880};
        DXGKARG_SETVIDPNSOURCEADDRESS a={.PrimarySegment=TG_GPU_SCANOUT_SEGMENT,
            .PrimaryAddress={.QuadPart=0x98000000+r*0x400000},.hAllocation=&allocations[r],.ContextCount=1};
        const UINT before=sync_calls;
        level=3;CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&a)==STATUS_SUCCESS);level=PASSIVE_LEVEL;
        CHECK(sync_calls==before && adapter.Panel.Pending.Address==(UINT64)a.PrimaryAddress.QuadPart);
        CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&a)==STATUS_DEVICE_BUSY);
        latch(&model,false);
        CHECK(adapter.Panel.Pending.Address && model.assembly[T214_DC_STATE_CONTROL]);
        latch(&model,true);signal_vblank(&d);
        active_is((UINT64)a.PrimaryAddress.QuadPart,TRUE);
        CHECK(!adapter.Panel.Pending.Address && adapter.Panel.Displayed.Address==(UINT64)a.PrimaryAddress.QuadPart);
    }

    snapshot_controls(&d);

    DXGK_PRIMARYCONTEXTDATA primary={.hAllocation=&allocations[1],.SegmentId=TG_GPU_SCANOUT_SEGMENT,
        .SegmentAddress={.QuadPart=0x98000000}};
    DXGK_PRIMARYCONTEXTDATA *contexts[]={&primary};
    DXGK_MULTIPLANE_OVERLAY_PLANE3 plane={.ContextCount=1,.ppContextData=contexts,
        .InputFlags={.Enabled=1},.PlaneAttributes={.DstRect={0,0,720,1280},
        .ColorSpaceType=D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709}};
    DXGK_MULTIPLANE_OVERLAY_PLANE3 *planes[]={&plane};
    DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3 args={.PlaneCount=1,.ppPlanes=planes};
    for(UINT r=0;r<4;++r) {
        const UINT slot=(r&1)?1:0;
        primary.hAllocation=&allocations[slot];
        plane.PlaneAttributes.SrcRect=(RECT){0,0,(LONG)allocations[slot].Width,(LONG)allocations[slot].Height};
        plane.PlaneAttributes.Rotation=(D3DDDI_ROTATION)(r+1);
        plane.PresentId=100+r;
        DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE2 check_plane={.hAllocation=primary.hAllocation,
            .PlaneAttributes=plane.PlaneAttributes};
        DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE2 *check_planes[]={&check_plane};
        DXGKARG_CHECKMULTIPLANEOVERLAYSUPPORT3 check={.PlaneCount=1,.ppPlanes=check_planes};
        UINT writes=model.writes;
        CHECK(d.DxgkDdiCheckMultiPlaneOverlaySupport3(&adapter,&check)==STATUS_SUCCESS && check.Supported==(r==0) && writes==model.writes);
        if(r) {
            CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_INVALID_PARAMETER);
            CHECK(writes==model.writes);
            continue;
        }
        CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
        signal_vblank(&d);CHECK(reported_present!=plane.PresentId);
        latch(&model,true);
        model.active[T214_DC_WIN_A_STRIDE]^=64;
        signal_vblank(&d);CHECK(reported_present!=plane.PresentId && adapter.Panel.Pending.Address);
        model.active[T214_DC_WIN_A_STRIDE]^=64;
        signal_vblank(&d);CHECK(reported_present==plane.PresentId && !adapter.Panel.Pending.Address);
        active_is(0x98000000,TRUE);
        check_plane.PlaneAttributes.SrcRect.right--;
        CHECK(d.DxgkDdiCheckMultiPlaneOverlaySupport3(&adapter,&check)==STATUS_SUCCESS && !check.Supported);
    }
    /* Same backing with a new receipt still needs a completed activation. */
    plane.PlaneAttributes.Rotation=D3DDDI_ROTATION_IDENTITY;plane.PresentId=200;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    latch(&model,false);signal_vblank(&d);CHECK(reported_present!=200);
    latch(&model,true);signal_vblank(&d);CHECK(reported_present==200);
    active_is(0x98000000,TRUE);

    UINT writes=model.writes;
    allocations[1].Pitch=5120;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_INVALID_PARAMETER && model.writes==writes);
    allocations[1].Pitch=2880;
    primary.SegmentAddress.QuadPart=0x98fff000;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_INVALID_ADDRESS && model.writes==writes);
    primary.SegmentAddress.QuadPart=0x98400000;
    plane.InputFlags.FlipImmediate=1;plane.PresentId=201;
    level=DISPATCH_LEVEL;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_RETRY && args.OutputFlags.PrePresentNeeded);
    CHECK(model.writes==writes);level=PASSIVE_LEVEL;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS && stalls && !args.OutputFlags.Value);
    CHECK(adapter.Panel.Displayed.PresentId==201 && !adapter.Panel.Pending.Address);
    active_is(0x98400000,TRUE);

    /* No latch means no premature completion, including a new PresentId at the
     * same address. A later retry can finish after the exact state is active. */
    auto_latch=FALSE;plane.PresentId=202;plane.PlaneAttributes.Rotation=D3DDDI_ROTATION_IDENTITY;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_INVALID_PARAMETER);
    CHECK(adapter.Panel.Displayed.PresentId==201 && adapter.Panel.Pending.PresentId==202);
    auto_latch=TRUE;latch(&model,true);
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    CHECK(adapter.Panel.Displayed.PresentId==202 && !adapter.Panel.Pending.Address);

    /* Visibility changes stop/start the assigned primary. They do not replace
     * it with POST: no new flip is required to recover the same surface. */
    plane.InputFlags.FlipImmediate=0;plane.PresentId=203;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS && !adapter.Panel.Pending.Address);
    active_is(0x98400000,FALSE);
    signal_vblank(&d);CHECK(reported_present==203);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(0x98400000,TRUE);
    signal_vblank(&d);CHECK(reported_present==203);

    /* Target timings/power and source visibility are independent gates.
     * Re-enabling a target must preserve the OS's hidden source state. */
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS);
    CHECK(TgGpuPanelSetTargetActive(&adapter,FALSE)==STATUS_SUCCESS);
    CHECK(TgGpuPanelSetTargetActive(&adapter,TRUE)==STATUS_SUCCESS);
    CHECK(!adapter.Panel.SourceVisible && adapter.Panel.TargetActive);
    active_is(0x98400000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    CHECK(TgGpuPanelSetTargetActive(&adapter,FALSE)==STATUS_SUCCESS);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    CHECK(adapter.Panel.SourceVisible && !adapter.Panel.TargetActive);
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    latch(&model,true);signal_vblank(&d);
    active_is(0x98400000,FALSE);
    CHECK(TgGpuPanelSetTargetActive(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(0x98400000,TRUE);
    CHECK(reported_present==203);

    /* MPO disable also has no replacement allocation: only its enable bit
     * changes. Source visibility must still know which primary to show. */
    plane.InputFlags.Enabled=0;plane.ContextCount=0;plane.ppContextData=NULL;
    plane.PresentId=204;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    latch(&model,true);signal_vblank(&d);
    active_is(0x98400000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(0x98400000,TRUE);
    plane.InputFlags.Enabled=1;plane.ContextCount=1;plane.ppContextData=contexts;

    /* A replacement primary can be bound while the source is hidden. Keep
     * fetch disabled until the separate source-visibility DDI enables it. */
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS);
    static DXGKARG_SETVIDPNSOURCEADDRESS hidden={.PrimarySegment=TG_GPU_SCANOUT_SEGMENT,
        .Flags={.ModeChange=1},.PrimaryAddress={.QuadPart=0x98800000},.hAllocation=&allocations[2]};
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&hidden)==STATUS_SUCCESS);
    active_is(0x98800000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(0x98800000,TRUE);

    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS);
    plane.PresentId=205;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    latch(&model,true);
    const UINT hidden_notifications=notifications;
    signal_vblank(&d);
    CHECK(notifications==hidden_notifications+1 && reported_present==205);
    active_is(0x98400000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(0x98400000,TRUE);

    /* A blanking timeout does not pretend the old scanout has stopped. */
    auto_latch=FALSE;
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_IO_TIMEOUT);
    CHECK(model.active[T214_DC_WIN_A_OPTIONS]&T214_DC_WIN_ENABLE);
    auto_latch=TRUE;latch(&model,true);
    CHECK(TgGpuPanelSetVisibility(&adapter,FALSE)==STATUS_SUCCESS);
    active_is(0x98400000,FALSE);
    CHECK(TgGpuPanelSetVisibility(&adapter,TRUE)==STATUS_SUCCESS);
    active_is(0x98400000,TRUE);

    /* Unlike a scheduled flip, a modeset returns only after the new primary
     * is active. Reuse the same two portrait slots across mode changes. */
    const UINT orientations[] = {0,1,0,1,0};
    DXGKARG_SETVIDPNSOURCEADDRESS modeset = {
        .PrimarySegment=TG_GPU_SCANOUT_SEGMENT,.Flags={.ModeChange=1}};
    for (UINT i=0;i<sizeof(orientations)/sizeof(orientations[0]);++i) {
        UINT r=orientations[i];
        modeset.hAllocation=&allocations[r];
        modeset.PrimaryAddress.QuadPart=0x98000000+r*0x400000;
        CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_SUCCESS);
        CHECK(!adapter.Panel.Pending.Address);
        active_is((UINT64)modeset.PrimaryAddress.QuadPart,TRUE);
        CHECK(!(model.assembly[T214_DC_REG_ACT_CONTROL]&T214_DC_WIN_A_HCOUNTER));
    }
    /* Drain an older queued MPO flip instead of returning DEVICE_BUSY to the
     * synchronous mode change, then confirm the replacement's full layout. */
    plane.PresentId=206;
    CHECK(d.DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3(&adapter,&args)==STATUS_SUCCESS);
    CHECK(adapter.Panel.Pending.Address);
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_SUCCESS);
    CHECK(!adapter.Panel.Pending.Address);
    active_is(0x98000000,TRUE);

    writes=model.writes;level=DISPATCH_LEVEL;
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_INVALID_PARAMETER && model.writes==writes);
    level=PASSIVE_LEVEL;auto_latch=FALSE;
    modeset.hAllocation=&allocations[1];modeset.PrimaryAddress.QuadPart=0x98400000;
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_IO_TIMEOUT);
    CHECK(adapter.Panel.Pending.Address==0x98400000 && adapter.Panel.Displayed.Address==0x98000000);
    auto_latch=TRUE;modeset.hAllocation=&allocations[0];modeset.PrimaryAddress.QuadPart=0x98000000;
    CHECK(d.DxgkDdiSetVidPnSourceAddress(&adapter,&modeset)==STATUS_SUCCESS);
    CHECK(!adapter.Panel.Pending.Address);
    active_is(0x98000000,TRUE);

    /* B03: Display A VBlank returns TRUE immediately without probing GPU PRI. */
    {
        static ULONG fake_gpu[0x200 / sizeof(ULONG)] = {0};
        static ULONG fake_lic[8] = {0};
        adapter.Engine.FaultIsr.GpuRegs = fake_gpu;
        adapter.Engine.FaultIsr.LicRegs = fake_lic;
        InterlockedExchange(&adapter.Engine.FaultIsr.Armed, 1);
        InterlockedExchange(&adapter.Engine.FaultIsr.IsrProbeEnterCount, 0);
        signal_vblank(&d);
        CHECK(adapter.Engine.FaultIsr.IsrProbeEnterCount == 0);
        InterlockedExchange(&adapter.Engine.FaultIsr.Armed, 0);
        adapter.Engine.FaultIsr.GpuRegs = NULL;
        adapter.Engine.FaultIsr.LicRegs = NULL;
    }

    /* DxgkDdiSystemDisplayEnable / Write: HIGH_LEVEL takeover onto POST FB,
     * strict ACT_REQ/active-bank verification (M3/N2), and 4-angle rotation. */
    {
        static ULONG fake_post_fb[T214_DC_PANEL_WIDTH * T214_DC_PANEL_HEIGHT];
        static ULONG src_patch[4 * 4];
        DXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS sflags = {0};
        UINT sw = 0, sh = 0; D3DDDIFORMAT sfmt = 0;
        CHECK(d.DxgkDdiSystemDisplayEnable && d.DxgkDdiSystemDisplayWrite);
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
        adapter.Panel.PostCpuAddress = fake_post_fb;
        fake_post_fb[0] = 0x11223344u;
        level = 15; /* HIGH_LEVEL */

        /* M3 counterexample: when ACT_REQ stays pending and active address stays
         * on 0x98000000, SystemDisplayEnable MUST return STATUS_IO_TIMEOUT! */
        auto_latch = FALSE;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_IO_TIMEOUT);
        CHECK(model.active[T214_DC_WIN_A_ADDR] == 0x98000000u);
        auto_latch = TRUE;
        latch(&model, true);

        /* N2 / T3 counterexamples: when DSI output is disabled, scanout mode is
         * non-continuous, or TargetActive is FALSE, SystemDisplayEnable MUST
         * return STATUS_DEVICE_NOT_READY (even though Window A can latch)! */
        const ULONG saved_out = model.assembly[T214_DC_OUTPUT_OPTIONS];
        model.assembly[T214_DC_OUTPUT_OPTIONS] = saved_out & ~T214_DC_DSI_ENABLE;
        model.active[T214_DC_OUTPUT_OPTIONS] = model.assembly[T214_DC_OUTPUT_OPTIONS];
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
        model.assembly[T214_DC_OUTPUT_OPTIONS] = saved_out;
        model.active[T214_DC_OUTPUT_OPTIONS] = saved_out;

        const ULONG saved_cmd = model.assembly[T214_DC_COMMAND];
        model.assembly[T214_DC_COMMAND] = saved_cmd & ~T214_DC_MODE_MASK;
        model.active[T214_DC_COMMAND] = model.assembly[T214_DC_COMMAND];
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
        model.assembly[T214_DC_COMMAND] = saved_cmd;
        model.active[T214_DC_COMMAND] = saved_cmd;

        adapter.Panel.TargetActive = FALSE;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
        adapter.Panel.TargetActive = TRUE;

        /* N2 v5 counterexample 1: geometry (ACTIVE_SIZE=0) or TargetActive
         * invalidated DURING latch must fail post-latch verification! */
        corrupt_active_size_on_latch = TRUE;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
        corrupt_active_size_on_latch = FALSE;
        model.assembly[T214_DC_ACTIVE_SIZE] = T214_DC_PANEL_ACTIVE;
        model.active[T214_DC_ACTIVE_SIZE] = T214_DC_PANEL_ACTIVE;

        drop_target_active_on_latch = TRUE;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
        drop_target_active_on_latch = FALSE;
        adapter.Panel.TargetActive = TRUE;

        /* N2 v5/v6 counterexamples: active/submitted GPU work without mapped
         * CarX/Clamp/McFlush or with undrained MC status MUST be rejected
         * BEFORE touching clamp/reset or setting Isolated; with stable MC drain
         * (initial + 5 reads) + clamp + reset, SystemDisplayEnable isolates GPU
         * work at HIGH_LEVEL with zero GPU BAR0 PRI reads! */
        {
            static ULONG fake_clamp[1] = {0};
            static ULONG fake_car[6] = {0};
            static ULONG fake_mc_flush[2] = {0, 1u << 2};
            adapter.Hardware.Power.owned = true;
            adapter.Hardware.Power.ready = true;
            adapter.Hardware.Isolated = FALSE;
            adapter.Platform.PreservedGpuIsolated = FALSE;
            adapter.Engine.Channel.referenced = true;
            adapter.Engine.Channel.submitted = true;
            InterlockedExchange(&adapter.Engine.FaultIsr.IsrProbeEnterCount, 0);
            /* Missing CarX/Clamp -> cannot prove GPU isolation -> reject! */
            CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
            /* N2 v6: Clamp + CarX mapped, but McFlush missing -> MUST reject! */
            adapter.Resources.Mapping[TgGpuClamp] = fake_clamp;
            adapter.Resources.Memory[TgGpuClamp].u.Memory.Length = sizeof(fake_clamp);
            adapter.Resources.Mapping[TgGpuCarX] = fake_car;
            adapter.Resources.Memory[TgGpuCarX].u.Memory.Length = sizeof(fake_car);
            adapter.Resources.Mapping[TgGpuMcFlush] = NULL;
            fake_clamp[0] = 0u;
            fake_car[3] = 1u << 24;
            CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_NOT_READY);
            CHECK(!adapter.Hardware.Isolated && fake_clamp[0] == 0u);
            /* Map SoC MC_FLUSH, test persistently undrained MC_FLUSH_STATUS == 0 -> TIMEOUT before clamp! */
            adapter.Resources.Mapping[TgGpuMcFlush] = fake_mc_flush;
            adapter.Resources.Memory[TgGpuMcFlush].u.Memory.Length = sizeof(fake_mc_flush);
            fake_mc_flush[0] = 0u;
            fake_mc_flush[1] = 0u;
            CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_IO_TIMEOUT);
            CHECK(!adapter.Hardware.Isolated && fake_clamp[0] == 0u && (fake_mc_flush[0] & (1u << 2)) != 0u);
            /* Now provide stable MC_FLUSH_STATUS bit 2, but let CAR_RESET_X stay 0 -> DATA_ERROR! */
            fake_mc_flush[1] = 1u << 2;
            fake_clamp[0] = 0u;
            fake_car[3] = 0u;
            CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_DEVICE_DATA_ERROR);
            CHECK(!adapter.Hardware.Isolated && (fake_clamp[0] & 1u) == 1u && (fake_car[4] & (1u << 24)) != 0u);
            /* Now reflect asserted CAR_RESET_X bit 24 -> isolation proven -> succeeds! */
            fake_car[3] = 1u << 24;
            CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_SUCCESS);
            CHECK(adapter.Hardware.Isolated && adapter.Platform.PreservedGpuIsolated &&
                  !adapter.Hardware.Power.ready &&
                  adapter.Engine.FaultIsr.IsrProbeEnterCount == 0 &&
                  (fake_mc_flush[0] & (1u << 2)) != 0u && (fake_car[2] & (1u << 24)) != 0u);
            /* Already-isolated GPU re-entry also succeeds with 0 BAR0 PRI reads: */
            CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_SUCCESS);
            memset(&adapter.Hardware, 0, sizeof(adapter.Hardware));
            memset(&adapter.Engine.Channel, 0, sizeof(adapter.Engine.Channel));
            adapter.Platform.PreservedGpuIsolated = FALSE;
            adapter.Engine.Stopping = FALSE;
            InterlockedExchange(&adapter.Engine.FaultIsr.GpuUnreachable, 0);
            adapter.Resources.Mapping[TgGpuClamp] = NULL;
            adapter.Resources.Mapping[TgGpuCarX] = NULL;
            adapter.Resources.Mapping[TgGpuMcFlush] = NULL;
        }

        /* Landscape 90-degree handheld mapping: 1280x720, (lx=2, ly=1) -> (px=718, py=2). */
        adapter.Panel.PathRotation = D3DKMDT_VPPR_ROTATE90;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_SUCCESS);
        CHECK(sw == T214_DC_PANEL_HEIGHT && sh == T214_DC_PANEL_WIDTH && sfmt == D3DDDIFMT_A8R8G8B8);
        CHECK(fake_post_fb[0] == 0u);
        active_is(T214_GPU_POST_FB_BASE, TRUE);
        memset(src_patch, 0, sizeof(src_patch));
        src_patch[1 * 4 + 2] = 0xAABBCCDDu;
        d.DxgkDdiSystemDisplayWrite(&adapter, src_patch, 4, 4, 4 * sizeof(ULONG), 0, 0);
        CHECK(fake_post_fb[(SIZE_T)2 * T214_DC_PANEL_WIDTH + (T214_DC_PANEL_WIDTH - 1u - 1u)] == 0xAABBCCDDu);

        /* 270-degree mapping: 1280x720, (lx=2, ly=1) -> (px=1, py=1277). */
        adapter.Panel.PathRotation = D3DKMDT_VPPR_ROTATE270;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_SUCCESS);
        CHECK(sw == T214_DC_PANEL_HEIGHT && sh == T214_DC_PANEL_WIDTH);
        src_patch[1 * 4 + 2] = 0x55667788u;
        d.DxgkDdiSystemDisplayWrite(&adapter, src_patch, 4, 4, 4 * sizeof(ULONG), 0, 0);
        CHECK(fake_post_fb[(SIZE_T)(T214_DC_PANEL_HEIGHT - 1u - 2u) * T214_DC_PANEL_WIDTH + 1u] == 0x55667788u);

        /* Portrait IDENTITY mapping: 720x1280, (lx=2, ly=1) -> (px=2, py=1). */
        adapter.Panel.PathRotation = D3DKMDT_VPPR_IDENTITY;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_SUCCESS);
        CHECK(sw == T214_DC_PANEL_WIDTH && sh == T214_DC_PANEL_HEIGHT);
        src_patch[1 * 4 + 2] = 0x13572468u;
        d.DxgkDdiSystemDisplayWrite(&adapter, src_patch, 4, 4, 4 * sizeof(ULONG), 0, 0);
        CHECK(fake_post_fb[(SIZE_T)1 * T214_DC_PANEL_WIDTH + 2u] == 0x13572468u);

        /* Portrait ROTATE180 mapping: 720x1280, (lx=2, ly=1) -> (px=717, py=1278). */
        adapter.Panel.PathRotation = D3DKMDT_VPPR_ROTATE180;
        CHECK(d.DxgkDdiSystemDisplayEnable(&adapter, 7, &sflags, &sw, &sh, &sfmt) == STATUS_SUCCESS);
        CHECK(sw == T214_DC_PANEL_WIDTH && sh == T214_DC_PANEL_HEIGHT);
        src_patch[1 * 4 + 2] = 0x99887766u;
        d.DxgkDdiSystemDisplayWrite(&adapter, src_patch, 4, 4, 4 * sizeof(ULONG), 0, 0);
        CHECK(fake_post_fb[(SIZE_T)(T214_DC_PANEL_HEIGHT - 1u - 1u) * T214_DC_PANEL_WIDTH +
                           (T214_DC_PANEL_WIDTH - 1u - 2u)] == 0x99887766u);

        adapter.Panel.PostCpuAddress = NULL;
        level = PASSIVE_LEVEL;
    }

    const struct tg_t214_dc_scanout post={T214_GPU_POST_FB_BASE,true};
    CHECK(tg_t214_dc_queue_surface(&adapter.Panel.Dc,&post,false)==TG_T214_DC_OK);
    latch(&model,true);
    CHECK(TgGpuPanelStop(&adapter)==STATUS_SUCCESS && !adapter.Panel.Dc.owned);
    check_reported_child(&d); /* A released DC still identifies the same outlet. */
    CHECK(model.active[T214_DC_WIN_A_STRIDE]==2880 && !(model.active[T214_DC_WIN_A_OPTIONS]&0x15));
    CHECK(notifications && level==PASSIVE_LEVEL);
    const char text[]="PASS: WDDM portrait scanout/MPO transform rejection, source visibility and hidden rebinding, modeset retirement, full-layout latch and POST recovery (model only)\r\n";
    ULONG written;WriteFile(GetStdHandle((ULONG)-11),text,sizeof(text)-1,&written,NULL);
    ExitProcess(0);
}
