/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_T214_PMU_H
#define TG_GPU_T214_PMU_H
#include "t214_acr.h"
enum tg_gpu_pmu_result {
    TG_GPU_PMU_OK, TG_GPU_PMU_ARGUMENT, TG_GPU_PMU_SECURITY,
    TG_GPU_PMU_REGISTER, TG_GPU_PMU_HALT_IRQ, TG_GPU_PMU_PIO,
    TG_GPU_PMU_INIT_TIMEOUT, TG_GPU_PMU_INIT_FORMAT, TG_GPU_PMU_QUEUE,
    TG_GPU_PMU_REPLY_TIMEOUT, TG_GPU_PMU_WPR
};
struct tg_gpu_pmu_status {
    uint32_t stage, result, cpuctl, irqstat, msg_head, msg_tail;
    uint32_t init_header, hpq_base, hpq_size, hpq_index, msg_base, msg_size;
    uint32_t last_header, reply_code, messages, last_register, last_value, cmd_head;
};
/* Start only the authenticated GM20B 20490253 PMU RTOS and initialize its WPR
 * service. Poll fixed DMEM queues with MC CPU IRQ outputs disabled. No trace,
 * power governor, arbitrary commands, host pointers or additional DMA buffers.
 * The enclosing probe retains all ACR backing until full GPU reset/drain.
 */
enum tg_gpu_pmu_result tg_gpu_pmu_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, const struct tg_gpu_acr_hw_status *acr,
    struct tg_gpu_pmu_status *status);
#endif
