// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Native ARM64 Windows test: only device I/O is substituted. JSON is written
 * through real WriteFile/stdout. No GPU operation or physical pass is implied.
 */
#define UNICODE
#define _UNICODE
#define WINVER 0x0a00
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <windows/control.h>
#include <core/execute.h>

static unsigned fixture,opened;
static DWORD wantedCode,lastError;
static HANDLE WINAPI FixtureOpen(LPCWSTR name,DWORD access,DWORD share,
 LPSECURITY_ATTRIBUTES sa,DWORD disposition,DWORD flags,HANDLE templateFile) {
 (void)share;(void)sa;(void)disposition;(void)flags;(void)templateFile;
 if(lstrcmpW(name,L"\\\\.\\TegraGpuProbe") ||
    access!=(wantedCode==TG_GPU_IOCTL_STATUS?GENERIC_READ:GENERIC_WRITE))ExitProcess(101);
 ++opened;
 if(fixture==3){lastError=ERROR_ACCESS_DENIED;return INVALID_HANDLE_VALUE;}
 return (HANDLE)(ULONG_PTR)123;
}
static BOOL WINAPI FixtureControl(HANDLE handle,DWORD code,LPVOID in,DWORD inBytes,
 LPVOID out,DWORD outBytes,LPDWORD bytes,LPOVERLAPPED overlapped) {
 struct tg_gpu_status s={0};
 struct tg_gpu_request *request=in;
 (void)overlapped;
 if(handle!=(HANDLE)(ULONG_PTR)123 || code!=wantedCode || outBytes!=sizeof(s))ExitProcess(102);
 if(code==TG_GPU_IOCTL_RUN || code==TG_GPU_IOCTL_GR_RUN || code==TG_GPU_IOCTL_IRQ_RUN ||
    code==TG_GPU_IOCTL_3D_RUN || code==TG_GPU_IOCTL_CLEAR_RUN || code==TG_GPU_IOCTL_DRAW_RUN ||
    code==TG_GPU_IOCTL_BACKEND_RUN){
  if(inBytes!=sizeof(*request) || request->version!=TG_GPU_CONTROL_VERSION ||
     request->target_khz!=TG_GPU_CONTROL_DEFAULT_KHZ)ExitProcess(103);
 }else if(in || inBytes)ExitProcess(104);
 s.version=TG_GPU_CONTROL_VERSION+(fixture==4);
 s.tested=fixture!=0;s.target_khz=TG_GPU_CONTROL_DEFAULT_KHZ;
 s.measured_khz=729600;s.rail_uv=800000;s.boot0=0x12e000b1;
 s.stage=fixture==2?306:304;s.execution_result=fixture==2?0:6;
 s.expected[0]=0x46a7712c;s.expected[1]=~s.expected[0];
 s.observed[0]=s.observed[1]=0xdeadbeef;
 if(fixture==2){s.observed[0]=s.expected[0];s.observed[1]=s.expected[1];}
 s.pbdma_enable_before=0;s.pbdma_enable_after=1;s.pbdma_runlist_map=1;
 s.channel_instance_after=0x800fde33;s.runlist_status=2;s.pbdma_status=0x10000004;
 s.pbdma_gp_fetch=0xffffffff;s.frontend.stage=11;s.frontend.ltc_count=2;
 s.frontend.fb_flushes=2;s.frontend.l2_flushes=2;s.frontend.l2_invalidations=2;
 s.frontend.gr_enable=0xd8013120;s.frontend.gr_fifo_access=0x10001;
 s.frontend.gr_gate_ctrl[0]=0xaaaa0010;s.frontend.gr_gate_ctrl[1]=0xbbbb0010;
 s.gr.stage=15;s.gr.fecs_mailbox=0x9000;s.gr.last_register=0x4188ac;s.gr.last_value=2;
 s.gr.context_bytes=0x17000;s.gr.zcull_bytes=0x4000;s.gr.pm_bytes=0x9000;
 s.gr.fecs_host_interrupt=0x10000;s.gr.fecs_ctxsw_status=0x1234;
 s.gr.fecs_error_mailbox=0x18;s.gr.new_context=0x800c0040;
 if(fixture>=6){
  s.gr.priv_security=1;s.acr.stage=9;s.acr.security=1;s.acr.wpr_base=0xffc00000;s.acr.wpr_size=0x200000;
  s.stage=310;s.execution_result=14; /* Authentication alone cannot pass FIFO. */
  if(fixture>=7){
   s.stage=306;s.execution_result=0;s.observed[0]=s.expected[0];s.observed[1]=s.expected[1];
   s.pmu.stage=6;
   if(fixture==7)s.acr_firmware_result=2; /* A format failure cannot be hidden by later fields. */
   if(fixture==8){s.pmu.stage=5;s.pmu.result=10;s.pmu.reply_code=1;}
   if(fixture==10)s.gr.stage=8; /* Ready alone is not context-state initialization. */
   if(fixture==11)s.gr.context_bytes=0;
   if(fixture==12)s.gr.stage=13; /* FECS queries alone omit GPC translation setup. */
  }
 }
 if((fixture>=13 && fixture<=19) || fixture>=30){
  s.graphics.stage=9;s.graphics.context_bytes=s.gr.context_bytes;
  s.graphics.bind_mailbox=0x10;s.graphics.save_mailbox=1;
  s.graphics.image_magic=0x600dc0de;s.graphics.image_changed_words=123;
  if(fixture==14)s.graphics.stage=7;
  if(fixture==15)s.graphics.result=TG_GPU_GR_CTX_FECS_REJECTED;
  if(fixture==16)s.graphics.image_changed_words=0;
  if(fixture==17)s.graphics.image_magic=0xa5a5a5a5;
  if(fixture==18)s.graphics.save_mailbox=3;
  if(fixture==19)s.graphics.bind_mailbox=0x30;
 }
 if(fixture>=20 && fixture<=29){
  s.irq_gsi=T214_GPU_NONSTALL_GSI;s.irq_expected=2;
  s.interrupt.interrupts=2;s.irq_dpcs=2;
  s.interrupt.stage=TG_GPU_IRQ_RESTORE;
  s.interrupt.last_register=T214_GPU_REG_BASE+0x144;s.interrupt.last_value=0;
  if(fixture==21)s.interrupt.interrupts=0;
  if(fixture==22)s.interrupt.interrupts=1;
  if(fixture==23)s.irq_dpcs=1;
  if(fixture==24)s.interrupt.result=TG_GPU_IRQ_UNEXPECTED;
  if(fixture==25)s.interrupt.restore_result=TG_GPU_IRQ_REGISTER;
  if(fixture==26)s.irq_expected=0;
  if(fixture==27)s.irq_gsi=0;
  if(fixture==28)s.resources_owned=1;
  if(fixture==29){
   s.execution_result=TG_GPU_EXEC_IRQ;s.interrupt.result=TG_GPU_IRQ_REGISTER;
   s.interrupt.stage=TG_GPU_IRQ_SNAPSHOT;
   s.interrupt.last_register=T214_GPU_REG_BASE+0x644;s.interrupt.last_value=0xffffffff;
  }
 }
 if(fixture>=30){
  s.gr.fecs_error_mailbox=s.gr.fecs_host_interrupt=0;
  s.gr_channel.image_ready=1;s.gr_channel.class_id=TG_GPU_GR_ENGINE_CLASS;
  s.gr_channel.current_context=0x800c0040;s.gr_channel.restore_ops=1;
  if(fixture==31)s.gr_channel.image_result=TG_GPU_GR_CTX_VISIBILITY;
  if(fixture==32)s.gr_channel.image_ready=0;
  if(fixture==33)s.gr_channel.image_ready=2;
  if(fixture==34)s.gr_channel.class_id=0xb06f;
  if(fixture==35){
   s.gr.gr_interrupt=0x100000;
   s.method_error.status=0x24;s.method_error.trapped_address=0x80001558;
   s.method_error.data_lo=1;
  }
  if(fixture==36)s.gr.fecs_error_mailbox=1;
  if(fixture==37)s.resources_owned=1;
  if(fixture==38)s.observed[1]=0xdeadbeef;
  if(fixture==39)s.graphics.stage=8;
  if(fixture==40)s.graphics.image_changed_words=0;
  if(fixture==41)s.gr.fecs_host_interrupt=0x10000;
 }
 if(fixture>=42){
  s.clear=(struct tg_gpu_clear_status){.checked=1,.width=16,.height=16,.pitch=128,.format=0xd5,
      .pixels=256,.background=0xffff0000,.foreground=0xff0000ff,.image_hash=0x12345678};
  if(fixture==43)s.clear.checked=0;
  if(fixture==44)s.clear.errors=1;
  if(fixture==45)s.clear.guard_errors=1;
  if(fixture==46)s.clear.background=0xdeadbeef;
  if(fixture==47)s.clear.foreground=0xdeadbeef;
  if(fixture==48)s.clear.pixels=255;
  if(fixture==49)s.clear.format=0;
  if(fixture==50)s.clear.width=32;
  if(fixture==51)s.clear.pitch=64;
  if(fixture==52)s.gr_channel.image_ready=0;
 }
 if(fixture>=53){
  s.draw=(struct tg_gpu_draw_status){1,1,53,TG_GPU_DRAW_VS_CODE_BYTES,TG_GPU_DRAW_FS_CODE_BYTES,
      TG_GPU_DRAW_VS_GPRS,TG_GPU_DRAW_FS_GPRS,TG_GPU_DRAW_CODE_HASH};
  if(fixture==54)s.draw.requested=0;if(fixture==55)s.draw.checked=0;
  if(fixture==56)s.draw.sm=50;if(fixture==57)s.draw.vs_bytes=0;if(fixture==58)s.draw.fs_bytes=0;
  if(fixture==59)s.draw.vs_gprs=0;if(fixture==60)s.draw.fs_gprs=0;if(fixture==61)s.draw.code_hash^=1;
  if(fixture==62)s.clear.errors=1;if(fixture==63)s.clear.guard_errors=1;
  if(fixture==64)s.resources_owned=1;if(fixture==65)s.gr_channel.image_ready=0;
  if(fixture==66)s.observed[1]=0xdeadbeef;
 }
 if(fixture>=67){
  s.backend=(struct tg_gpu_backend_status){.requested=1,.stage=TG_GPU_BACKEND_COMPLETE_STAGE,
      .worker_started=1,.submissions=6,.notifications=4,.userd_gp_get=2,.userd_gp_put=2,
      .kernel_priv=1,.context_generation=2,.completion_fence=9,
      .fence_observed=6,.fence_expected=6,.channel_stage=7,.fence_cpu=6,
      .preemptions=2,.preemption_notifications=2,.preemption_completed=7,.cancelled_report=TG_GPU_CLEAR_SENTINEL,
      .resets=1,.restarts=1,.reset_isolated=1,.lost_context_rejected=1,
      .snapshot_uploads=4,.snapshot_bytes=1024,.snapshot_rejections=2,.source_mutations=6,
      .paging_fence=1,.paging_bytes=4000};
  s.gr_channel.restore_ops=2;s.gr_channel.current_context=0x000c0040;
  if(fixture==68)s.backend.requested=0;if(fixture==69)s.backend.stage=7;
  if(fixture==70)s.backend.result=0xc0000001;if(fixture==71)s.backend.worker_started=0;
  if(fixture==72)s.backend.submissions=1;if(fixture==73)s.backend.notifications=1;
  if(fixture==74)s.backend.recovery_requests=1;if(fixture==75)s.backend.userd_gp_get=1;
  if(fixture==76)s.backend.userd_gp_put=1;if(fixture==77)s.backend.user_priv=1;
  if(fixture==78)s.backend.kernel_priv=0;if(fixture==79)s.backend.allocations_remaining=1;
  if(fixture==80)s.backend.context_generation=1;if(fixture==81)s.backend.completion_fence=1;
  if(fixture==82)s.gr_channel.restore_ops=1;if(fixture==83)s.gr_channel.current_context|=0x80000000;
  if(fixture==84)s.fifo_interrupt=0x100;if(fixture==85)s.mmu_fault=1;
  if(fixture==86)s.pbdma_interrupt=1;if(fixture==87)s.pbdma_interrupt_1=1;
  if(fixture==88)s.backend.fence_observed=1;if(fixture==89)s.backend.fence_expected=1;
  if(fixture==90)s.backend.channel_stage=4;
  if(fixture==91)s.backend.fence_cpu=1;
  if(fixture==92)s.backend.preemptions=0;if(fixture==93)s.backend.preemption_notifications=0;
  if(fixture==94)s.backend.preemption_completed=2;if(fixture==95)s.backend.cancelled_report=0;
  if(fixture==96)s.backend.resets=0;if(fixture==97)s.backend.restarts=0;
  if(fixture==98)s.backend.reset_isolated=0;if(fixture==99)s.backend.lost_context_rejected=0;
  if(fixture==100)s.backend.snapshot_uploads=0;if(fixture==101)s.backend.snapshot_bytes=0;
  if(fixture==102)s.backend.snapshot_rejections=0;if(fixture==103)s.backend.source_mutations=0;
  if(fixture==104)s.backend.paging_fence=0;if(fixture==105)s.backend.paging_bytes=3996;
  if(fixture==106)s.backend.paging_errors=1;
 }
 *(struct tg_gpu_status *)out=s;*bytes=sizeof(s)-(fixture==5?4:0);
 return TRUE;
}
static BOOL WINAPI FixtureClose(HANDLE h){(void)h;return TRUE;}
static DWORD WINAPI FixtureError(void){return lastError;}

#define CreateFileW FixtureOpen
#define DeviceIoControl FixtureControl
#define CloseHandle FixtureClose
#define GetLastError FixtureError
#define start UiEntry
#include "../performance-check.c"
#undef start

void start(void) {
 LPWSTR query[]={L"fixture.exe",L"--gpu-json"};
 LPWSTR run[]={L"fixture.exe",L"--gpu-json",L"--gpu-729"};
 LPWSTR runGr[]={L"fixture.exe",L"--gpu-json",L"--gpu-gr-729"};
 LPWSTR runIrq[]={L"fixture.exe",L"--gpu-json",L"--gpu-irq-729"};
 LPWSTR run3d[]={L"fixture.exe",L"--gpu-json",L"--gpu-3d-729"};
 LPWSTR runClear[]={L"fixture.exe",L"--gpu-json",L"--gpu-clear-729"};
 LPWSTR runDraw[]={L"fixture.exe",L"--gpu-json",L"--gpu-draw-729"};
 LPWSTR runBackend[]={L"fixture.exe",L"--gpu-json",L"--gpu-backend-729"};
 LPWSTR conflictBackend[]={L"fixture.exe",L"--gpu-json",L"--gpu-backend-729",L"--gpu-draw-729"};
 LPWSTR conflictDraw[]={L"fixture.exe",L"--gpu-json",L"--gpu-draw-729",L"--gpu-clear-729"};
 LPWSTR conflictClear[]={L"fixture.exe",L"--gpu-json",L"--gpu-clear-729",L"--gpu-3d-729"};
 LPWSTR conflict3d[]={L"fixture.exe",L"--gpu-json",L"--gpu-3d-729",L"--gpu-irq-729"};
 LPWSTR conflictIrq[]={L"fixture.exe",L"--gpu-json",L"--gpu-irq-729",L"--gpu-gr-729"};
 LPWSTR conflictGr[]={L"fixture.exe",L"--gpu-json",L"--gpu-gr-729",L"--gpu-729"};
 LPWSTR conflict[]={L"fixture.exe",L"--gpu-json",L"--gpu-729",L"--gpu-1228"};
 const DWORD results[]={0,0,ERROR_GEN_FAILURE,0,ERROR_ACCESS_DENIED,
                         ERROR_REVISION_MISMATCH,ERROR_REVISION_MISMATCH,ERROR_INVALID_PARAMETER,
                         ERROR_GEN_FAILURE,ERROR_GEN_FAILURE,ERROR_GEN_FAILURE,0,
                         ERROR_GEN_FAILURE,ERROR_GEN_FAILURE,ERROR_GEN_FAILURE};
 const unsigned fixtures[]={0,1,1,2,3,4,5,2,6,7,8,9,10,11,12};
 for(unsigned i=0;i<41;++i){
  fixture=i==40?67:67+i;opened=0;
  arguments=i==40?conflictBackend:runBackend;argumentCount=i==40?4:3;
  wantedCode=TG_GPU_IOCTL_BACKEND_RUN;
  DWORD result=gpu_json();
  if(result!=(i==40?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==40?0u:1u))ExitProcess(260+i);
 }
 for(unsigned i=0;i<15;++i){
  DWORD result;fixture=fixtures[i];opened=0;
  arguments=i<2?query:i==7?conflict:run;argumentCount=i<2?2:i==7?4:3;
  wantedCode=i<2?TG_GPU_IOCTL_STATUS:TG_GPU_IOCTL_RUN;
  result=gpu_json();
  if(result!=results[i] || opened!=(i==7?0u:1u))ExitProcess(110+i);
 }
 for(unsigned i=0;i<15;++i){
  fixture=i==14?53:53+i;opened=0;
  arguments=i==14?conflictDraw:runDraw;argumentCount=i==14?4:3;
  wantedCode=TG_GPU_IOCTL_DRAW_RUN;
  DWORD result=gpu_json();
  if(result!=(i==14?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==14?0u:1u))ExitProcess(230+i);
 }
 for(unsigned i=0;i<12;++i){
  fixture=i==11?42:42+i;opened=0;
  arguments=i==11?conflictClear:runClear;argumentCount=i==11?4:3;
  wantedCode=TG_GPU_IOCTL_CLEAR_RUN;
  DWORD result=gpu_json();
  if(result!=(i==11?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==11?0u:1u))ExitProcess(210+i);
 }
 for(unsigned i=0;i<13;++i){
  fixture=i==12?30:30+i;opened=0;
  arguments=i==12?conflict3d:run3d;argumentCount=i==12?4:3;
  wantedCode=TG_GPU_IOCTL_3D_RUN;
  DWORD result=gpu_json();
  if(result!=(i==12?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==12?0u:1u))ExitProcess(180+i);
 }
 for(unsigned i=0;i<11;++i){
  fixture=i==10?20:20+i;opened=0;
  arguments=i==10?conflictIrq:runIrq;argumentCount=i==10?4:3;
  wantedCode=TG_GPU_IOCTL_IRQ_RUN;
  DWORD result=gpu_json();
  if(result!=(i==10?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==10?0u:1u))ExitProcess(160+i);
 }
 for(unsigned i=0;i<8;++i){
  fixture=i==7?13:13+i;opened=0;
  arguments=i==7?conflictGr:runGr;argumentCount=i==7?4:3;
  wantedCode=TG_GPU_IOCTL_GR_RUN;
  DWORD result=gpu_json();
  if(result!=(i==7?ERROR_INVALID_PARAMETER:i?ERROR_GEN_FAILURE:0) ||
     opened!=(i==7?0u:1u))ExitProcess(140+i);
 }
 if(!json_text(GetStdHandle(STD_OUTPUT_HANDLE),"{\"fixture_complete\":true}\r\n"))ExitProcess(120);
 ExitProcess(0);
}
