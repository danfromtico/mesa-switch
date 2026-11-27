/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_T214_INTERRUPT_H
#define TG_GPU_T214_INTERRUPT_H
#include "t214_power.h"
#include "t214_resources.h"

enum tg_gpu_irq_result {
    TG_GPU_IRQ_OK, TG_GPU_IRQ_ARGUMENT, TG_GPU_IRQ_REGISTER,
    TG_GPU_IRQ_FOREIGN, TG_GPU_IRQ_UNEXPECTED, TG_GPU_IRQ_STICKY,
    TG_GPU_IRQ_TIMEOUT
};
enum tg_gpu_irq_stage {
    TG_GPU_IRQ_IDLE, TG_GPU_IRQ_SNAPSHOT, TG_GPU_IRQ_DISABLE_MC,
    TG_GPU_IRQ_ROUTE_LIC, TG_GPU_IRQ_ENABLE_LIC, TG_GPU_IRQ_ENABLE_FIFO,
    TG_GPU_IRQ_MASK_MC, TG_GPU_IRQ_ENABLE_MC, TG_GPU_IRQ_ARMED,
    TG_GPU_IRQ_HANDLE, TG_GPU_IRQ_RESTORE
};
struct tg_gpu_irq_status {
    uint32_t result, restore_result, mc_pending, fifo_pending, interrupts;
    /* Freeze the first failure's phase/register/readback across cleanup. */
    uint32_t stage, last_register, last_value;
};
struct tg_gpu_irq_state {
    uint32_t mc_enable, mc_mask, fifo_enable, lic_enable, lic_class;
    bool saved, armed;
};
/* Exclusive, powered GPU only, after FIFO initialization. The caller must
 * hold its interrupt lock across arm/disarm/handle and stop CPU MMIO access
 * before powering down. No blocking calls, PMIC I/O or software fence writes.
 * The optional LIC resource must be translated/mapped before these calls.
 */
enum tg_gpu_irq_result tg_gpu_irq_arm(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,struct tg_gpu_irq_state *,struct tg_gpu_irq_status *);
enum tg_gpu_irq_result tg_gpu_irq_disarm(const struct tg_gpu_power_io *,
    struct tg_gpu_irq_state *,struct tg_gpu_irq_status *);
/* Only after the caller has proved full GPU reset/power retirement. Restore
 * any retained LIC ownership without accessing the powered-off GPU at all.
 * A prior restoration error remains diagnostic evidence even after recovery.
 */
enum tg_gpu_irq_result tg_gpu_irq_retire(const struct tg_gpu_power_io *,
    struct tg_gpu_irq_state *);
/* True only for this GPU's PFIFO nonstall source, including a masked fault.
 * An acknowledged event wakes a worker; it never proves a completed DMA.
 */
bool tg_gpu_irq_handle(const struct tg_gpu_power_io *,struct tg_gpu_irq_state *,
                       struct tg_gpu_irq_status *);
#endif
