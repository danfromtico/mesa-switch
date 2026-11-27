/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef LITE_GPU_PASS_H
#define LITE_GPU_PASS_H
#include <stdbool.h>
#include <windows/control.h>

/* Acceptance of a completed diagnostic report, not proof of shader/Vulkan
 * execution. The caller has already checked the device I/O ABI and size. */
static inline bool gpu_pass(const struct tg_gpu_status *s) {
 return s->tested && !s->power_result && !s->clock_result && !s->execution_result &&
        !s->frontend.result && !s->gr.result && s->gr.stage==15 &&
        s->gr.context_bytes && s->gr.zcull_bytes && s->gr.pm_bytes &&
        !s->acr_firmware_result && !s->acr.result && !s->pmu.result &&
        (!s->gr.priv_security || (s->acr.stage==9 && s->pmu.stage==6 && !s->pmu.reply_code)) &&
        !s->firmware_status && !s->restore_result && !s->resources_owned &&
        s->expected[0]==s->observed[0] && s->expected[1]==s->observed[1];
}
static inline bool gpu_gr_pass(const struct tg_gpu_status *s) {
 return gpu_pass(s) && !s->graphics.result && s->graphics.stage==9 &&
        s->graphics.context_bytes==s->gr.context_bytes &&
        s->graphics.bind_mailbox!=0xffffffff && (s->graphics.bind_mailbox&0x30)==0x10 &&
        s->graphics.save_mailbox!=0xffffffff && (s->graphics.save_mailbox&3)==1 &&
        s->graphics.image_magic==0x600dc0de && s->graphics.image_changed_words;
}
static inline bool gpu_irq_pass(const struct tg_gpu_status *s) {
 return gpu_pass(s) && !s->interrupt.result && !s->interrupt.restore_result &&
        s->irq_gsi==T214_GPU_NONSTALL_GSI && s->irq_expected==2 &&
        s->interrupt.interrupts==s->irq_expected && s->irq_dpcs==s->irq_expected;
}
static inline bool gpu_3d_pass(const struct tg_gpu_status *s) {
 return gpu_gr_pass(s) && s->gr_channel.image_ready==1 && !s->gr_channel.image_result &&
        s->gr_channel.class_id==TG_GPU_GR_ENGINE_CLASS &&
        !s->gr.gr_interrupt && !s->gr.fecs_error_mailbox && !s->gr.fecs_host_interrupt;
}
static inline bool gpu_pixel_pass(const struct tg_gpu_status *s) {
 return gpu_3d_pass(s) && s->clear.checked==1 && s->clear.pixels==TG_GPU_CLEAR_WIDTH*TG_GPU_CLEAR_HEIGHT &&
        s->clear.width==TG_GPU_CLEAR_WIDTH && s->clear.height==TG_GPU_CLEAR_HEIGHT &&
        s->clear.pitch==TG_GPU_CLEAR_PITCH && s->clear.format==TG_GPU_CLEAR_FORMAT &&
        !s->clear.errors && !s->clear.guard_errors &&
        s->clear.background==TG_GPU_CLEAR_BACKGROUND && s->clear.foreground==TG_GPU_CLEAR_FOREGROUND;
}
static inline bool gpu_draw_pass(const struct tg_gpu_status *s) {
 return gpu_pixel_pass(s) && s->draw.requested==1 && s->draw.checked==1 && s->draw.sm==53 &&
        s->draw.vs_bytes==TG_GPU_DRAW_VS_CODE_BYTES && s->draw.fs_bytes==TG_GPU_DRAW_FS_CODE_BYTES &&
        s->draw.vs_gprs==TG_GPU_DRAW_VS_GPRS && s->draw.fs_gprs==TG_GPU_DRAW_FS_GPRS &&
        s->draw.code_hash==TG_GPU_DRAW_CODE_HASH;
}
static inline bool gpu_backend_pass(const struct tg_gpu_status *s) {
 return gpu_draw_pass(s) && s->backend.requested==1 &&
        s->backend.stage==TG_GPU_BACKEND_COMPLETE_STAGE && !s->backend.result &&
        s->backend.worker_started==1 && s->backend.submissions==6 &&
        s->backend.notifications==4 && !s->backend.recovery_requests &&
        s->backend.userd_gp_get==2 && s->backend.userd_gp_put==2 &&
        !s->backend.user_priv && s->backend.kernel_priv==1 &&
        !s->backend.allocations_remaining && s->backend.context_generation==2 &&
        s->backend.completion_fence==9 && s->backend.fence_observed==6 &&
        s->backend.fence_expected==6 && s->backend.fence_cpu==6 &&
        s->backend.channel_stage==7 && s->gr_channel.restore_ops>=2 &&
        s->backend.preemptions==2 && s->backend.preemption_notifications==2 &&
        s->backend.preemption_completed==7 && s->backend.cancelled_report==TG_GPU_CLEAR_SENTINEL &&
        s->backend.resets==1 && s->backend.restarts==1 && s->backend.reset_isolated==1 &&
        s->backend.lost_context_rejected==1 &&
        s->backend.snapshot_uploads==4 && s->backend.snapshot_bytes>0 &&
        s->backend.snapshot_rejections==2 && s->backend.source_mutations==6 &&
        s->backend.paging_fence==1 && s->backend.paging_bytes==4000 && !s->backend.paging_errors &&
        !(s->gr_channel.current_context&0x80000000u) &&
        !tg_gpu_fifo_has_error(s->fifo_interrupt) && !s->mmu_fault &&
        !s->pbdma_interrupt && !s->pbdma_interrupt_1;
}
#endif
