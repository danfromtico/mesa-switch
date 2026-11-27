/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../gpu-pass.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
 struct tg_gpu_status good={0};
 good.tested=1;good.expected[0]=good.observed[0]=0x12345678;
 good.expected[1]=good.observed[1]=0x87654321;
 good.gr.stage=15;good.gr.context_bytes=79872;good.gr.zcull_bytes=66048;good.gr.pm_bytes=2304;
 good.gr.priv_security=1;good.acr.stage=9;good.pmu.stage=6;
 good.graphics.stage=9;good.graphics.context_bytes=79872;
 good.graphics.bind_mailbox=0x10;good.graphics.save_mailbox=1;
 good.graphics.image_magic=0x600dc0de;good.graphics.image_changed_words=19328;
 good.gr_channel.image_ready=1;good.gr_channel.class_id=TG_GPU_GR_ENGINE_CLASS;
 assert(gpu_pass(&good)&&gpu_gr_pass(&good)&&gpu_3d_pass(&good));
 assert(!gpu_irq_pass(&good));
 struct tg_gpu_status bad;
 for(unsigned test=0;test<15;++test){
  bad=good;
  if(test==0)bad.gr_channel.image_ready=0;
  if(test==1)bad.gr_channel.image_ready=2;
  if(test==2)bad.gr_channel.class_id=0xb06f; /* PBDMA class is not graphics. */
  if(test==3)bad.gr_channel.image_result=TG_GPU_GR_CTX_VISIBILITY;
  if(test==4)bad.gr.gr_interrupt=1;
  if(test==5)bad.gr.fecs_error_mailbox=1;
  if(test==6)bad.observed[0]=0xdeadbeef;
  if(test==7)bad.observed[1]=0xdeadbeef;
  if(test==8)bad.resources_owned=1;
  if(test==9)bad.restore_result=1;
  if(test==10)bad.graphics.image_changed_words=0;
  if(test==11)bad.graphics.stage=8;
  if(test==12)bad.graphics.image_magic=0xa5a5a5a5;
  if(test==13)bad.pmu.reply_code=1;
  if(test==14)bad.gr.fecs_host_interrupt=0x10000;
  assert(!gpu_3d_pass(&bad));
  if(test<6 || test==14)assert(gpu_gr_pass(&bad)); /* Golden/common readback alone is insufficient. */
 }
 bad=good;bad.irq_expected=2;bad.irq_gsi=T214_GPU_NONSTALL_GSI;
 bad.interrupt.interrupts=bad.irq_dpcs=2;assert(gpu_irq_pass(&bad));
 assert(!gpu_pixel_pass(&good));
 good.clear=(struct tg_gpu_clear_status){.checked=1,.width=16,.height=16,.pitch=128,
     .format=0xd5,.pixels=256,.background=0xffff0000,.foreground=0xff0000ff};
 assert(gpu_pixel_pass(&good));
 assert(!gpu_draw_pass(&good)); /* The accepted clear is not a shader pass. */
 struct tg_gpu_status drawn=good;
 drawn.draw=(struct tg_gpu_draw_status){1,1,53,TG_GPU_DRAW_VS_CODE_BYTES,TG_GPU_DRAW_FS_CODE_BYTES,
     TG_GPU_DRAW_VS_GPRS,TG_GPU_DRAW_FS_GPRS,TG_GPU_DRAW_CODE_HASH};
 assert(gpu_draw_pass(&drawn));
 assert(!gpu_backend_pass(&drawn)); /* A legacy shader pass has no backend proof. */
 struct tg_gpu_status backend=drawn;
 backend.backend=(struct tg_gpu_backend_status){.requested=1,.stage=TG_GPU_BACKEND_COMPLETE_STAGE,
     .worker_started=1,.submissions=6,.notifications=4,.userd_gp_get=2,.userd_gp_put=2,
     .kernel_priv=1,.context_generation=2,.completion_fence=9,
     .fence_observed=6,.fence_expected=6,.channel_stage=7,.fence_cpu=6,
     .preemptions=2,.preemption_notifications=2,.preemption_completed=7,.cancelled_report=TG_GPU_CLEAR_SENTINEL,
     .resets=1,.restarts=1,.reset_isolated=1,.lost_context_rejected=1,
     .snapshot_uploads=4,.snapshot_bytes=1024,.snapshot_rejections=2,.source_mutations=6,
      .paging_fence=1,.paging_bytes=4000};
 backend.gr_channel.restore_ops=2;
 assert(gpu_backend_pass(&backend));
 for(unsigned test=0;test<39;++test){
  bad=backend;
  if(test==0)bad.backend.requested=0;if(test==1)bad.backend.stage=7;
  if(test==2)bad.backend.result=0xc0000001;if(test==3)bad.backend.worker_started=0;
  if(test==4)bad.backend.submissions=1;if(test==5)bad.backend.notifications=1;
  if(test==6)bad.backend.recovery_requests=1;if(test==7)bad.backend.userd_gp_get=1;
  if(test==8)bad.backend.userd_gp_put=1;if(test==9)bad.backend.user_priv=1;
  if(test==10)bad.backend.kernel_priv=0;if(test==11)bad.backend.allocations_remaining=1;
  if(test==12)bad.backend.context_generation=1;if(test==13)bad.backend.completion_fence=1;
  if(test==14)bad.gr_channel.restore_ops=1;if(test==15)bad.gr_channel.current_context=0x80000000;
  if(test==16)bad.fifo_interrupt=0x100;if(test==17)bad.mmu_fault=1;
  if(test==18)bad.pbdma_interrupt=1;if(test==19)bad.pbdma_interrupt_1=1;
  if(test==20)bad.backend.fence_observed=1;if(test==21)bad.backend.fence_expected=1;
  if(test==22)bad.backend.channel_stage=4;
  if(test==23)bad.backend.fence_cpu=1;
  if(test==24)bad.backend.preemptions=0;if(test==25)bad.backend.preemption_notifications=0;
  if(test==26)bad.backend.preemption_completed=2;if(test==27)bad.backend.cancelled_report=0;
  if(test==28)bad.backend.resets=0;if(test==29)bad.backend.restarts=0;
  if(test==30)bad.backend.reset_isolated=0;if(test==31)bad.backend.lost_context_rejected=0;
  if(test==32)bad.backend.snapshot_uploads=0;if(test==33)bad.backend.snapshot_bytes=0;
  if(test==34)bad.backend.snapshot_rejections=0;if(test==35)bad.backend.source_mutations=0;
  if(test==36)bad.backend.paging_fence=0;if(test==37)bad.backend.paging_bytes=3996;
  if(test==38)bad.backend.paging_errors=1;
  assert(!gpu_backend_pass(&bad));
 }
 for(unsigned test=0;test<12;++test){
  bad=drawn;
  if(test==0)bad.draw.requested=0;if(test==1)bad.draw.checked=0;
  if(test==2)bad.draw.sm=50;if(test==3)bad.draw.vs_bytes=0;if(test==4)bad.draw.fs_bytes=0;
  if(test==5)bad.draw.vs_gprs=0;if(test==6)bad.draw.fs_gprs=0;if(test==7)bad.draw.code_hash^=1;
  if(test==8)bad.clear.guard_errors=1;if(test==9)bad.clear.errors=1;
  if(test==10)bad.resources_owned=1;if(test==11)bad.gr.gr_interrupt=1;
  assert(!gpu_draw_pass(&bad));
 }
 for(unsigned test=0;test<8;++test){
  bad=good;
  if(test==0)bad.clear.errors=1;if(test==1)bad.clear.guard_errors=1;
  if(test==2)bad.clear.background=0xdeadbeef;if(test==3)bad.clear.foreground=0xdeadbeef;
  if(test==4)bad.clear.checked=0;if(test==5)bad.clear.height=15;
  if(test==6)bad.clear.format=0;if(test==7)bad.clear.pixels=255;
  assert(gpu_3d_pass(&bad)&&!gpu_pixel_pass(&bad));
 }
 puts("GPU diagnostic acceptance gates: PASS (status fixtures only)");
 return 0;
}
