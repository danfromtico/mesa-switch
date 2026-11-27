// SPDX-License-Identifier: BSD-2-Clause-Patent
// --gpu-json queries live status; add --gpu-729 or --gpu-1228 for one FIFO run.
// --gpu-gr-729 additionally initializes and reads back a golden GR context.
// --gpu-3d-729 loads a channel image and reads back graphics-engine reports.
// --gpu-clear-729 additionally verifies an off-screen ROP color-clear pattern.
// --gpu-draw-729 draws the foreground with fixed NAK vertex/fragment shaders.
// --gpu-irq-729 requires two distinct hardware ISR/DPC notifications as well.
// JSON goes to inherited stdout (SSH/pipe/redirection), without a GUI or UAC.
// Query success exits 0 even for an earlier failed test; inspect fifo_pass.
// A run exits 0 only when its requested FIFO/GR check passes, 31 on a test
// failure, or a Win32 error for I/O/arguments. Neither check is Vulkan drawing.
#define UNICODE
#define _UNICODE
#define WINVER 0x0a00
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <shellapi.h>
#include <windows/control.h>
/* This tool links without the CRT. Larger status aggregates may lower to a
 * memset call; volatile stores keep this implementation self-contained. */
void *memset(void *destination,int value,size_t bytes) {
 volatile unsigned char *out=destination;
 for(size_t i=0;i<bytes;++i)out[i]=(unsigned char)value;
 return destination;
}
static UINT_PTR image_cookie = 0x00002b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
 .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64), .SecurityCookie = (ULONG_PTR)&image_cookie
};
static HFONT font,controlFont;
static DWORD cpu,ram,requested,hand,error,soc,board,duty,runs,failures,throttled;
static DWORD lastRuns,stale;
static BOOL present,live;
typedef struct { DWORD version, brightness, duty, displayState; } BACKLIGHT_STATUS;
static BACKLIGHT_STATUS backlight;
static BOOL backlightAvailable;
static DWORD backlightError;
static DWORD backlightSetError;
static HWND darker,lighter,gpuChoice,gpuRun;
static struct tg_gpu_status gpu, gpuWorkerResult;
static BOOL gpuAvailable;
static DWORD gpuError,gpuWorkerError,gpuTestError;
static volatile LONG gpuBusy;
static DWORD gpuRequested=TG_GPU_CONTROL_DEFAULT_KHZ;
static int argumentCount;
static LPWSTR *arguments;
#define WM_GPU_DONE (WM_APP+1)
#define WM_GPU_RUN (WM_APP+2)
static BOOL gpu_io(DWORD code, DWORD target, struct tg_gpu_status *out, DWORD *error) {
 DWORD bytes=0;
 BOOL run=code==TG_GPU_IOCTL_RUN || code==TG_GPU_IOCTL_GR_RUN || code==TG_GPU_IOCTL_IRQ_RUN ||
          code==TG_GPU_IOCTL_3D_RUN || code==TG_GPU_IOCTL_CLEAR_RUN || code==TG_GPU_IOCTL_DRAW_RUN ||
          code==TG_GPU_IOCTL_BACKEND_RUN;
 struct tg_gpu_request in={TG_GPU_CONTROL_VERSION,target};
 HANDLE h=CreateFileW(L"\\\\.\\TegraGpuProbe",run?GENERIC_WRITE:GENERIC_READ,
                     FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
 if(h==INVALID_HANDLE_VALUE){*error=GetLastError();return FALSE;}
 BOOL ok=DeviceIoControl(h,code,run?&in:NULL,
                        run?sizeof(in):0,out,sizeof(*out),&bytes,NULL);
 *error=ok?0:GetLastError();CloseHandle(h);
 if(ok && (bytes!=sizeof(*out) || out->version!=TG_GPU_CONTROL_VERSION)){*error=ERROR_REVISION_MISMATCH;return FALSE;}
 return ok;
}
static DWORD WINAPI gpu_worker(void *context) {
 HWND w=(HWND)context;
 gpu_io(TG_GPU_IOCTL_RUN,gpuRequested,&gpuWorkerResult,&gpuWorkerError);
 PostMessageW(w,WM_GPU_DONE,0,0);return 0;
}
static void start_gpu(HWND w) {
 if(InterlockedCompareExchange(&gpuBusy,1,0))return;
 gpuTestError=0;
 gpuRequested=SendMessageW(gpuChoice,CB_GETCURSEL,0,0)==1?TG_GPU_CONTROL_OC_KHZ:TG_GPU_CONTROL_DEFAULT_KHZ;
 EnableWindow(gpuRun,FALSE);EnableWindow(gpuChoice,FALSE);
 HANDLE t=CreateThread(NULL,0,gpu_worker,w,0,NULL);
 if(t)CloseHandle(t);else{gpuWorkerError=GetLastError();PostMessageW(w,WM_GPU_DONE,0,0);}
 InvalidateRect(w,NULL,TRUE);
}
static BOOL has_switch(LPCWSTR wanted) {
 for(int i=1;i<argumentCount;++i)if(!lstrcmpW(arguments[i],wanted))return TRUE;
 return FALSE;
}
#include "gpu-pass.h"
static BOOL json_text(HANDLE out,const char *text) {
 DWORD length=0,written;
 while(text[length])++length;
 while(length){
  if(!WriteFile(out,text,length,&written,NULL) || !written)return FALSE;
  text+=written;length-=written;
 }
 return TRUE;
}
static BOOL json_number(HANDLE out,const char *name,DWORD value,BOOL hex) {
 char text[128]; /* All field names below are fixed short literals. */
 wsprintfA(text,hex?",\"%s\":\"0x%08lX\"":",\"%s\":%lu",name,value);
 return json_text(out,text);
}
static DWORD gpu_json(void) {
 struct tg_gpu_status s={0};
 DWORD ioError=0;
 BOOL run729=has_switch(L"--gpu-729"),run1228=has_switch(L"--gpu-1228");
 BOOL runGr=has_switch(L"--gpu-gr-729");
 BOOL runIrq=has_switch(L"--gpu-irq-729");
 BOOL run3d=has_switch(L"--gpu-3d-729");
 BOOL runClear=has_switch(L"--gpu-clear-729");
 BOOL runDraw=has_switch(L"--gpu-draw-729");
 BOOL runBackend=has_switch(L"--gpu-backend-729");
 BOOL run=run729||run1228||runGr||runIrq||run3d||runClear||runDraw||runBackend;
 HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE);
 if(!out || out==INVALID_HANDLE_VALUE)return ERROR_INVALID_HANDLE;
 for(int i=1;i<argumentCount;++i)
  if(lstrcmpW(arguments[i],L"--gpu-json") && lstrcmpW(arguments[i],L"--gpu-729") &&
     lstrcmpW(arguments[i],L"--gpu-1228") && lstrcmpW(arguments[i],L"--gpu-gr-729") &&
     lstrcmpW(arguments[i],L"--gpu-irq-729") && lstrcmpW(arguments[i],L"--gpu-3d-729") &&
     lstrcmpW(arguments[i],L"--gpu-clear-729") && lstrcmpW(arguments[i],L"--gpu-draw-729") &&
     lstrcmpW(arguments[i],L"--gpu-backend-729"))ioError=ERROR_INVALID_PARAMETER;
 if(run729+run1228+runGr+runIrq+run3d+runClear+runDraw+runBackend>1)ioError=ERROR_INVALID_PARAMETER;
 /* A status query never powers up the GPU. RUN requires an already elevated
  * caller: no UAC window, hidden GUI, retry loop or file is created over SSH.
  */
 if(!ioError)gpu_io(runBackend?TG_GPU_IOCTL_BACKEND_RUN:runDraw?TG_GPU_IOCTL_DRAW_RUN:runClear?TG_GPU_IOCTL_CLEAR_RUN:run3d?TG_GPU_IOCTL_3D_RUN:runIrq?TG_GPU_IOCTL_IRQ_RUN:runGr?TG_GPU_IOCTL_GR_RUN:run?TG_GPU_IOCTL_RUN:TG_GPU_IOCTL_STATUS,
                   run1228?TG_GPU_CONTROL_OC_KHZ:TG_GPU_CONTROL_DEFAULT_KHZ,&s,&ioError);
 if(!json_text(out,runBackend?"{\"operation\":\"windows-backend\"":runDraw?"{\"operation\":\"gr-draw\"":runClear?"{\"operation\":\"gr-clear\"":run3d?"{\"operation\":\"gr-engine\"":runIrq?"{\"operation\":\"irq\"":runGr?"{\"operation\":\"gr-context\"":run?"{\"operation\":\"run\"":"{\"operation\":\"status\"") ||
    !json_number(out,"tool_abi",TG_GPU_CONTROL_VERSION,FALSE) ||
    !json_number(out,"win32_error",ioError,FALSE) ||
    !json_text(out,!ioError&&gpu_pass(&s)?",\"fifo_pass\":true":",\"fifo_pass\":false") ||
    !json_text(out,!ioError&&gpu_gr_pass(&s)?",\"gr_context_pass\":true":",\"gr_context_pass\":false") ||
    !json_text(out,!ioError&&gpu_irq_pass(&s)?",\"irq_pass\":true":",\"irq_pass\":false") ||
    !json_text(out,!ioError&&gpu_3d_pass(&s)?",\"gr_engine_pass\":true":",\"gr_engine_pass\":false") ||
    !json_text(out,!ioError&&gpu_pixel_pass(&s)?",\"pixel_pass\":true":",\"pixel_pass\":false") ||
    !json_text(out,!ioError&&gpu_draw_pass(&s)?",\"shader_pass\":true":",\"shader_pass\":false") ||
    !json_text(out,!ioError&&gpu_backend_pass(&s)?",\"backend_pass\":true":",\"backend_pass\":false"))
  return ERROR_WRITE_FAULT;
 if(!ioError){
#define NUMBER(field) if(!json_number(out,#field,s.field,FALSE))return ERROR_WRITE_FAULT
#define HEX(field) if(!json_number(out,#field,s.field,TRUE))return ERROR_WRITE_FAULT
  NUMBER(version);NUMBER(tested);NUMBER(target_khz);NUMBER(measured_khz);NUMBER(rail_uv);
  HEX(boot0);NUMBER(stage);NUMBER(power_result);NUMBER(clock_result);NUMBER(execution_result);
  HEX(restore_result);NUMBER(resources_owned);
  HEX(expected[0]);HEX(expected[1]);HEX(observed[0]);HEX(observed[1]);
  HEX(fifo_interrupt);HEX(mmu_fault);HEX(pbdma_interrupt);HEX(channel_status);
  HEX(mc_enable_before);HEX(channel_instance_before);HEX(mc_enable_after);HEX(channel_instance_after);
  HEX(pbdma_enable_before);HEX(pbdma_enable_after);HEX(pbdma_runlist_map);HEX(pbdma_status);
  NUMBER(userd_gp_get);NUMBER(userd_gp_put);HEX(scheduler_before);HEX(scheduler_after);HEX(scheduler_error);
  HEX(runlist_base);HEX(runlist_status);HEX(pbdma_gp_base);HEX(pbdma_gp_fetch);
  HEX(pbdma_gp_get);HEX(pbdma_gp_put);HEX(pbdma_interrupt_1);
  NUMBER(frontend.stage);NUMBER(frontend.result);NUMBER(frontend.ltc_count);NUMBER(frontend.fb_flushes);
  HEX(frontend.bus_interrupt);HEX(frontend.pri_status0);HEX(frontend.pri_status1);
  HEX(frontend.pri_error_address);HEX(frontend.pri_error_code);
  NUMBER(frontend.l2_flushes);NUMBER(frontend.l2_invalidations);
  HEX(frontend.gr_enable);HEX(frontend.gr_fifo_access);HEX(frontend.gr_gate_ctrl[0]);HEX(frontend.gr_gate_ctrl[1]);
  HEX(bind_error);HEX(chsw_error);HEX(engine_status[0]);HEX(engine_status[1]);
  NUMBER(bar1_phase);HEX(bar1_expected);HEX(bar1_read);HEX(bar1_cpu);
  NUMBER(gr.stage);NUMBER(gr.result);HEX(gr.priv_security);HEX(gr.last_register);HEX(gr.last_value);
  HEX(gr.current_context);NUMBER(gr.context_bytes);NUMBER(gr.zcull_bytes);NUMBER(gr.pm_bytes);
  HEX(gr.fecs_host_interrupt);HEX(gr.fecs_ctxsw_status);HEX(gr.fecs_error_mailbox);HEX(gr.new_context);
  HEX(gr.fecs_hwcfg);HEX(gr.gpccs_hwcfg);HEX(gr.fecs_mailbox);HEX(gr.gr_interrupt);HEX(firmware_status);
  NUMBER(acr_firmware_result);NUMBER(acr.stage);NUMBER(acr.result);HEX(acr.security);
  HEX(acr.hwcfg);NUMBER(acr.imem_size);NUMBER(acr.dmem_size);HEX(acr.mc_enable);
  HEX(acr.mailbox0);HEX(acr.mailbox1);HEX(acr.cpuctl);HEX(acr.sctl);
  HEX(acr.last_register);HEX(acr.last_value);NUMBER(acr.reset_confirmed);HEX(acr.scp_status);
  NUMBER(pmu.stage);NUMBER(pmu.result);HEX(pmu.cpuctl);HEX(pmu.irqstat);
  HEX(pmu.msg_head);HEX(pmu.msg_tail);HEX(pmu.init_header);HEX(pmu.hpq_base);
  NUMBER(pmu.hpq_size);NUMBER(pmu.hpq_index);HEX(pmu.msg_base);NUMBER(pmu.msg_size);
  HEX(pmu.last_header);HEX(pmu.reply_code);NUMBER(pmu.messages);
  HEX(pmu.last_register);HEX(pmu.last_value);HEX(pmu.cmd_head);
  HEX(pbdma_context.intr_enable);HEX(pbdma_context.intr_stall);HEX(pbdma_context.fifo_pending);
  HEX(pbdma_context.target);HEX(pbdma_context.signature);HEX(pbdma_context.method0);HEX(pbdma_context.data0);
  HEX(pbdma_context.pb_header);HEX(pbdma_context.header_shadow);HEX(pbdma_context.subdevice);
  NUMBER(graphics.stage);NUMBER(graphics.result);NUMBER(graphics.context_bytes);
  NUMBER(graphics.gpcs);NUMBER(graphics.tpcs);NUMBER(graphics.zculls);
  HEX(graphics.last_register);HEX(graphics.last_value);HEX(graphics.bind_mailbox);HEX(graphics.save_mailbox);
  HEX(graphics.image_magic);NUMBER(graphics.image_changed_words);HEX(graphics.image_hash);
  NUMBER(interrupt.result);NUMBER(interrupt.restore_result);HEX(interrupt.mc_pending);
  HEX(interrupt.fifo_pending);NUMBER(interrupt.interrupts);
  NUMBER(interrupt.stage);HEX(interrupt.last_register);HEX(interrupt.last_value);
  NUMBER(irq_dpcs);NUMBER(irq_gsi);NUMBER(irq_expected);
  NUMBER(gr_channel.image_result);NUMBER(gr_channel.image_ready);HEX(gr_channel.class_id);
  HEX(gr_channel.current_context);NUMBER(gr_channel.save_ops);NUMBER(gr_channel.restore_ops);
  NUMBER(clear.checked);NUMBER(clear.width);NUMBER(clear.height);NUMBER(clear.pitch);NUMBER(clear.format);
  NUMBER(clear.pixels);NUMBER(clear.errors);NUMBER(clear.guard_errors);
  HEX(clear.background);HEX(clear.foreground);HEX(clear.image_hash);
  HEX(method_error.status);HEX(method_error.trapped_address);HEX(method_error.data_lo);HEX(method_error.data_hi);
  HEX(method_error.exception);HEX(method_error.fe_esr);HEX(method_error.mme_esr);HEX(method_error.scc_esr);
  NUMBER(draw.requested);NUMBER(draw.checked);NUMBER(draw.sm);NUMBER(draw.vs_bytes);NUMBER(draw.fs_bytes);
  NUMBER(draw.vs_gprs);NUMBER(draw.fs_gprs);HEX(draw.code_hash);
  NUMBER(backend.requested);NUMBER(backend.stage);HEX(backend.result);NUMBER(backend.worker_started);
  NUMBER(backend.submissions);NUMBER(backend.notifications);NUMBER(backend.recovery_requests);
  NUMBER(backend.userd_gp_get);NUMBER(backend.userd_gp_put);NUMBER(backend.user_priv);NUMBER(backend.kernel_priv);
  NUMBER(backend.allocations_remaining);NUMBER(backend.context_generation);NUMBER(backend.completion_fence);
  NUMBER(backend.fault_id);HEX(backend.fault_instance);HEX(backend.fault_address_lo);HEX(backend.fault_address_hi);HEX(backend.fault_info);
  HEX(backend.fence_observed);HEX(backend.fence_expected);NUMBER(backend.channel_stage);
  HEX(backend.fence_cpu);
  NUMBER(backend.preemptions);NUMBER(backend.preemption_notifications);NUMBER(backend.preemption_completed);
  HEX(backend.cancelled_report);
  NUMBER(backend.resets);NUMBER(backend.restarts);NUMBER(backend.reset_isolated);NUMBER(backend.lost_context_rejected);
  NUMBER(backend.snapshot_uploads);NUMBER(backend.snapshot_bytes);NUMBER(backend.snapshot_rejections);NUMBER(backend.source_mutations);
  NUMBER(backend.paging_fence);NUMBER(backend.paging_bytes);NUMBER(backend.paging_errors);
  {
   char text[128];
   wsprintfA(text,",\"acr.wpr_base\":\"0x%08lX%08lX\",\"acr.wpr_size\":\"0x%08lX%08lX\"",
             (DWORD)(s.acr.wpr_base>>32),(DWORD)s.acr.wpr_base,
             (DWORD)(s.acr.wpr_size>>32),(DWORD)s.acr.wpr_size);
   if(!json_text(out,text))return ERROR_WRITE_FAULT;
  }
#undef NUMBER
#undef HEX
 }
 if(!json_text(out,"}\r\n"))return ERROR_WRITE_FAULT;
 return ioError?ioError:run && !(runBackend?gpu_backend_pass(&s):runDraw?gpu_draw_pass(&s):runClear?gpu_pixel_pass(&s):run3d?gpu_3d_pass(&s):runIrq?gpu_irq_pass(&s):runGr?gpu_gr_pass(&s):gpu_pass(&s))?ERROR_GEN_FAILURE:ERROR_SUCCESS;
}
static BOOL backlight_io(DWORD code,void *in,DWORD inSize,void *out,DWORD outSize) {
 DWORD bytes=0;
 HANDLE h=CreateFileW(L"\\\\.\\SwitchBacklight",code==0x00226040?GENERIC_READ:GENERIC_WRITE,
                     FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
 if(h==INVALID_HANDLE_VALUE){backlightError=GetLastError();return FALSE;}
 BOOL ok=DeviceIoControl(h,code,in,inSize,out,outSize,&bytes,NULL);
 backlightError=ok?0:GetLastError();CloseHandle(h);
 return ok && (outSize==0 || bytes==outSize);
}
static DWORD value(HKEY k,LPCWSTR name) {
 DWORD t,v=0,n=sizeof(v);
 return RegQueryValueExW(k,name,NULL,&t,(BYTE*)&v,&n)==ERROR_SUCCESS && t==REG_DWORD && n==sizeof(v)?v:0;
}
static void read_status(void) {
 HKEY k;present=FALSE;
 gpuAvailable=gpu_io(TG_GPU_IOCTL_STATUS,0,&gpu,&gpuError);
 EnableWindow(gpuRun,gpuAvailable);EnableWindow(gpuChoice,gpuAvailable);
 backlightAvailable=backlight_io(0x00226040,NULL,0,&backlight,sizeof(backlight)) && backlight.version==2;
 EnableWindow(darker,backlightAvailable);EnableWindow(lighter,backlightAvailable);
 if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Services\\switchfan\\Parameters",0,KEY_QUERY_VALUE,&k)!=ERROR_SUCCESS)return;
 present=TRUE;
 cpu=value(k,L"CpuActualKhz");ram=value(k,L"RamActualKhz");requested=value(k,L"CpuRequestedKhz");
 hand=value(k,L"HandoffStatus");error=value(k,L"CpuLastError");soc=value(k,L"TempSoC");
 board=value(k,L"TempBoard");duty=value(k,L"Duty");runs=value(k,L"Runs");
 failures=value(k,L"SensorFailures");throttled=value(k,L"CpuThrottled");RegCloseKey(k);
 if(runs!=lastRuns){live=TRUE;stale=0;lastRuns=runs;}else ++stale;
}
static void line(HDC dc,int y,LPCWSTR text) { TextOutW(dc,24,y,text,lstrlenW(text)); }
static void gpu_line(HDC dc,int y,LPCWSTR text) { TextOutW(dc,660,y,text,lstrlenW(text)); }
static void layout_controls(HWND w) {
 RECT r;GetClientRect(w,&r);
 int width=r.right>0?r.right:1,height=r.bottom>0?r.bottom:1;
 HWND controls[]={darker,lighter,gpuChoice,gpuRun};
 const int rects[][4]={{24,580,180,56},{222,580,180,56},{660,110,420,160},{660,178,420,56}};
 int sizeX=MulDiv(24,width,1200),sizeY=MulDiv(24,height,680),size=sizeX<sizeY?sizeX:sizeY;
 if(size<12)size=12;
 HFONT next=CreateFontW(size,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
 for(unsigned i=0;i<4;++i)if(controls[i]){
  MoveWindow(controls[i],MulDiv(rects[i][0],width,1200),MulDiv(rects[i][1],height,680),MulDiv(rects[i][2],width,1200),MulDiv(rects[i][3],height,680),TRUE);
  if(next)SendMessageW(controls[i],WM_SETFONT,(WPARAM)next,TRUE);
 }
 if(next){if(controlFont)DeleteObject(controlFont);controlFont=next;}
}
static LRESULT CALLBACK proc(HWND w,UINT msg,WPARAM a,LPARAM b) {
 if(msg==WM_CREATE){
  darker=CreateWindowExW(0,L"BUTTON",L"Darker",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,24,580,180,56,w,(HMENU)10,GetModuleHandleW(NULL),NULL);
  lighter=CreateWindowExW(0,L"BUTTON",L"Brighter",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,222,580,180,56,w,(HMENU)11,GetModuleHandleW(NULL),NULL);
  SendMessageW(darker,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(lighter,WM_SETFONT,(WPARAM)font,TRUE);
  gpuChoice=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,660,110,330,160,w,(HMENU)20,GetModuleHandleW(NULL),NULL);
  SendMessageW(gpuChoice,CB_ADDSTRING,0,(LPARAM)L"729.6 MHz (default)");
  SendMessageW(gpuChoice,CB_ADDSTRING,0,(LPARAM)L"1228.8 MHz (overclock)");
  SendMessageW(gpuChoice,CB_SETCURSEL,has_switch(L"--gpu-1228")?1:0,0);
  gpuRun=CreateWindowExW(0,L"BUTTON",L"Apply and test GPU",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,660,178,330,56,w,(HMENU)21,GetModuleHandleW(NULL),NULL);
  SendMessageW(gpuChoice,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(gpuRun,WM_SETFONT,(WPARAM)font,TRUE);
  layout_controls(w);read_status();
  if(!has_switch(L"--gpu-729") && !has_switch(L"--gpu-1228"))SendMessageW(gpuChoice,CB_SETCURSEL,gpu.target_khz==TG_GPU_CONTROL_OC_KHZ?1:0,0);
  SetTimer(w,1,1000,NULL);
  if(has_switch(L"--gpu-729") || has_switch(L"--gpu-1228"))PostMessageW(w,WM_GPU_RUN,0,0);
  return 0;
 }
 if(msg==WM_COMMAND && (LOWORD(a)==10 || LOWORD(a)==11) && backlightAvailable){
  DWORD level=backlight.brightness;
  if(LOWORD(a)==10)level=level>34?level-26:8;else level=level<229?level+26:255;
  if(backlight_io(0x0022A044,&level,sizeof(level),NULL,0)){backlightSetError=0;if(!gpuBusy)read_status();}
  else backlightSetError=backlightError;
  InvalidateRect(w,NULL,TRUE);return 0;
 }
 if((msg==WM_COMMAND && LOWORD(a)==21) || msg==WM_GPU_RUN){start_gpu(w);return 0;}
 if(msg==WM_GPU_DONE){
  InterlockedExchange(&gpuBusy,0);gpuTestError=gpuWorkerError;
  if(!gpuTestError){gpu=gpuWorkerResult;gpuAvailable=TRUE;}
  if(gpuTestError==ERROR_ACCESS_DENIED){
   WCHAR exe[MAX_PATH];
   if(GetModuleFileNameW(NULL,exe,MAX_PATH)){
    HINSTANCE opened=ShellExecuteW(w,L"runas",exe,gpuRequested==TG_GPU_CONTROL_OC_KHZ?L"--gpu-1228":L"--gpu-729",NULL,SW_SHOWNORMAL);
    if((INT_PTR)opened>32){DestroyWindow(w);return 0;}
   }
  }
  EnableWindow(gpuRun,gpuAvailable);EnableWindow(gpuChoice,gpuAvailable);
  InvalidateRect(w,NULL,TRUE);return 0;
 }
 if(msg==WM_TIMER){if(!gpuBusy)read_status();InvalidateRect(w,NULL,TRUE);return 0;}
 if(msg==WM_CLOSE && gpuBusy)return 0;
 if(msg==WM_SIZE){layout_controls(w);InvalidateRect(w,NULL,TRUE);return 0;}
 if(msg==WM_PAINT){
  PAINTSTRUCT p;RECT r;WCHAR text[192];HDC dc=BeginPaint(w,&p);GetClientRect(w,&r);
  FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
  SetMapMode(dc,MM_ANISOTROPIC);SetWindowExtEx(dc,1200,680,NULL);SetViewportExtEx(dc,r.right,r.bottom,NULL);
  SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
  BOOL ready=present && live && stale<6 && failures==0 && hand==1 && error==0 && cpu==1996800 && ram==2100000;
  SetTextColor(dc,ready?RGB(0,125,50):RGB(160,85,0));
  line(dc,26,ready?L"CPU / RAM targets reached":throttled?L"Thermal throttling":L"Current performance");
  SetTextColor(dc,RGB(20,30,45));
  wsprintfW(text,L"CPU     %lu.%lu MHz",cpu/1000,(cpu%1000)/100);line(dc,86,text);
  wsprintfW(text,L"RAM     %lu MHz",ram/1000);line(dc,130,text);
  wsprintfW(text,L"Temperature   SoC %lu C / board %lu C",soc,board);line(dc,174,text);
  wsprintfW(text,L"Fan PWM     %lu%%",duty*100/236);line(dc,218,text);
  line(dc,262,L"Renderer: CPU software");
  wsprintfW(text,L"Samples %lu   CPU target %lu MHz",runs,requested/1000);line(dc,322,text);
  wsprintfW(text,L"Boot status %lu   CPU error 0x%08lX",hand,error);line(dc,348,text);
  if(!present || !live || stale>=6)line(dc,374,L"Waiting for fresh fan readings");
  else if(gpu.tested && !gpuBusy && !gpuError){
   wsprintfW(text,L"Init %lu/%lu LTC %lu FB %lu L2 %lu/%lu",gpu.frontend.stage,gpu.frontend.result,gpu.frontend.ltc_count,gpu.frontend.fb_flushes,gpu.frontend.l2_flushes,gpu.frontend.l2_invalidations);line(dc,374,text);
  }
  if(gpu.tested && !gpuBusy && !gpuTestError && !gpuError){
   wsprintfW(text,L"Sched %08lX -> %08lX  err %08lX",gpu.scheduler_before,gpu.scheduler_after,gpu.scheduler_error);line(dc,400,text);
   wsprintfW(text,L"Bind %08lX  Chsw %08lX",gpu.bind_error,gpu.chsw_error);line(dc,426,text);
   wsprintfW(text,L"List %08lX / %08lX  PB %08lX",gpu.runlist_base,gpu.runlist_status,gpu.pbdma_status);line(dc,452,text);
   if(tg_gpu_pbdma_owns_slot(gpu.pbdma_status))
    wsprintfW(text,L"HW base %08lX fetch %lu G%lu P%lu",gpu.pbdma_gp_base,gpu.pbdma_gp_fetch,gpu.pbdma_gp_get,gpu.pbdma_gp_put);
   else wsprintfW(text,L"PBDMA has no valid channel/TSG 0");
   line(dc,478,text);
   wsprintfW(text,L"USERD G%lu P%lu  PB1 %08lX",gpu.userd_gp_get,gpu.userd_gp_put,gpu.pbdma_interrupt_1);line(dc,504,text);
   if(gpu.gr.stage)wsprintfW(text,L"GR %lu/%lu  mailbox %08lX",gpu.gr.stage,gpu.gr.result,gpu.gr.fecs_mailbox);
   else wsprintfW(text,L"Engine %08lX / %08lX",gpu.engine_status[0],gpu.engine_status[1]);
   line(dc,530,text);
  }else line(dc,478,L"Runtime readings; not a stability test.");
  if(backlightAvailable)wsprintfW(text,L"Brightness   %lu%%",(backlight.brightness*100+127)/255);
  else wsprintfW(text,L"Brightness unavailable (0x%08lX)",backlightError);
  line(dc,556,text);
  if(backlightSetError){wsprintfW(text,L"Brightness change failed: 0x%08lX",backlightSetError);line(dc,644,text);}
  wsprintfW(text,L"GPU - experimental (ABI %lu)",(DWORD)TG_GPU_CONTROL_VERSION);gpu_line(dc,26,text);
  gpu_line(dc,68,L"Choose a GPU clock profile");
  if(gpuBusy)wsprintfW(text,L"Running clock and FIFO readback...");
  else if(gpuTestError || gpuError)wsprintfW(text,L"GPU unavailable/error: 0x%08lX",gpuTestError?gpuTestError:gpuError);
  else if(!gpu.tested)wsprintfW(text,L"GPU has not been tested yet");
  else wsprintfW(text,L"Last measurement: %lu.%lu MHz",gpu.measured_khz/1000,(gpu.measured_khz%1000)/100);
  TextOutW(dc,660,264,text,lstrlenW(text));
  if(gpu.tested && !gpuBusy && !gpuTestError && !gpuError){
   wsprintfW(text,L"GPU rail: %lu mV  BOOT0: %08lX",gpu.rail_uv/1000,gpu.boot0);TextOutW(dc,660,308,text,lstrlenW(text));
   BOOL pass=gpu_pass(&gpu);
   SetTextColor(dc,pass?RGB(0,125,50):RGB(160,85,0));
   LPCWSTR result=pass?L"GPU FIFO READBACK PASS":L"GPU check did not pass";
   TextOutW(dc,660,352,result,lstrlenW(result));SetTextColor(dc,RGB(20,30,45));
   wsprintfW(text,L"Stage %lu  P:%lu C:%lu E:%lu",gpu.stage,gpu.power_result,gpu.clock_result,gpu.execution_result);TextOutW(dc,660,396,text,lstrlenW(text));
   if(gpu.firmware_status){
    wsprintfW(text,L"Firmware files: 0x%08lX",gpu.firmware_status);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"GPU bootstrap not started");
   }else if(gpu.stage==310){
    wsprintfW(text,L"PMU %lu/%lu  reply %08lX",gpu.pmu.stage,gpu.pmu.result,gpu.pmu.reply_code);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"Msg head %lX tail %lX",gpu.pmu.msg_head,gpu.pmu.msg_tail);
   }else if(gpu.stage==309){
    wsprintfW(text,L"ACR %lu/%lu  pack %lu",gpu.acr.stage,gpu.acr.result,gpu.acr_firmware_result);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"Reg %08lX = %08lX",gpu.acr.last_register,gpu.acr.last_value);
   }else if(gpu.stage==308){
    wsprintfW(text,L"GR bootstrap %lu/%lu sec %lX",gpu.gr.stage,gpu.gr.result,gpu.gr.priv_security);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"Reg %08lX = %08lX",gpu.gr.last_register,gpu.gr.last_value);
   }else if(gpu.stage==307){
    wsprintfW(text,L"GR enable %08lX access %08lX",gpu.frontend.gr_enable,gpu.frontend.gr_fifo_access);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"GR clocks %08lX / %08lX",gpu.frontend.gr_gate_ctrl[0],gpu.frontend.gr_gate_ctrl[1]);
   }else if(gpu.stage==302 && gpu.bar1_phase){
    wsprintfW(text,L"BAR1 step %lu expect %08lX",gpu.bar1_phase,gpu.bar1_expected);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"GPU %08lX CPU %08lX",gpu.bar1_read,gpu.bar1_cpu);
   }else if(gpu.frontend.result){
    wsprintfW(text,L"BUS %08lX PRI %08lX/%08lX",gpu.frontend.bus_interrupt,gpu.frontend.pri_status0,gpu.frontend.pri_status1);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"Addr %08lX Code %08lX",gpu.frontend.pri_error_address,gpu.frontend.pri_error_code);
   }else if(gpu.stage>=301 && gpu.stage<304){
    wsprintfW(text,L"MC: %08lX -> %08lX",gpu.mc_enable_before,gpu.mc_enable_after);TextOutW(dc,660,440,text,lstrlenW(text));
    if(gpu.mc_enable_before&0x100)wsprintfW(text,L"INST: %08lX -> %08lX",gpu.channel_instance_before,gpu.channel_instance_after);
    else wsprintfW(text,L"INST: reset -> %08lX",gpu.channel_instance_after);
   }else{
    wsprintfW(text,L"Read 1: %08lX / %08lX",gpu.observed[0],gpu.expected[0]);TextOutW(dc,660,440,text,lstrlenW(text));
    wsprintfW(text,L"Read 2: %08lX / %08lX",gpu.observed[1],gpu.expected[1]);
   }
   TextOutW(dc,660,484,text,lstrlenW(text));
   wsprintfW(text,L"Restore %08lX  owned %lu",gpu.restore_result,gpu.resources_owned);TextOutW(dc,660,528,text,lstrlenW(text));
  }
  if(gpu.tested && gpu.execution_result && !gpuBusy && !gpuTestError && !gpuError){
   wsprintfW(text,L"FIFO %08lX  MMU %08lX",gpu.fifo_interrupt,gpu.mmu_fault);gpu_line(dc,580,text);
   wsprintfW(text,L"PBDMA %08lX  CH %08lX",gpu.pbdma_interrupt,gpu.channel_status);gpu_line(dc,624,text);
  }else{
   gpu_line(dc,580,L"GPU powers down after this test.");
   gpu_line(dc,624,L"FIFO pass is not Vulkan rendering.");
  }
  EndPaint(w,&p);return 0;
 }
 if(msg==WM_DESTROY){KillTimer(w,1);PostQuitMessage(0);return 0;}
 return DefWindowProcW(w,msg,a,b);
}
void start(void) {
 arguments=CommandLineToArgvW(GetCommandLineW(),&argumentCount);
 if(!arguments)ExitProcess(ERROR_NOT_ENOUGH_MEMORY);
 if(has_switch(L"--gpu-json")){DWORD code=gpu_json();LocalFree(arguments);ExitProcess(code);}
 HINSTANCE h=GetModuleHandleW(NULL);WNDCLASSW c={0};MSG m;
 c.lpfnWndProc=proc;c.hInstance=h;c.hCursor=LoadCursorW(NULL,IDC_ARROW);c.lpszClassName=L"LitePerformanceCheck";
 RegisterClassW(&c);
 font=CreateFontW(24,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
 HWND w=CreateWindowExW(0,c.lpszClassName,L"Switch Lite - Performance and brightness",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN),NULL,NULL,h,NULL);
 UpdateWindow(w);while(GetMessageW(&m,NULL,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}
 if(controlFont)DeleteObject(controlFont);DeleteObject(font);LocalFree(arguments);ExitProcess(0);
}
