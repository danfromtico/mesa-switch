/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_EXECUTE_H
#define TEGRA_GPU_EXECUTE_H
#include "../platform/t214_power.h"
#include "../windows/control.h"
#include "channel.h"
#include <stddef.h>

#define TG_GPU_ARENA_BYTES 0x80000u
#define TG_GPU_GR_SURFACE_OFFSET 0x50000u
#define TG_GPU_GR_SURFACE_BYTES 0x10000u
#define TG_GPU_GR_SURFACE_VA 0x20000u
#define TG_GPU_GR_INSTANCE_OFFSET 0x49000u
#define TG_GPU_ACR_INSTANCE_OFFSET 0x4a000u
#define TG_GPU_ACR_SURFACE_OFFSET 0x60000u
#define TG_GPU_ACR_SURFACE_BYTES 0x20000u
#define TG_GPU_ACR_SURFACE_VA 0x40000u
#define TG_GPU_BOOT_USERD_BAR1_OFFSET 0x13000u
enum tg_gpu_execute_result {
    TG_GPU_EXEC_OK, TG_GPU_EXEC_ARGUMENT, TG_GPU_EXEC_BUSY,
    TG_GPU_EXEC_MMU_TIMEOUT, TG_GPU_EXEC_BAR1, TG_GPU_EXEC_RUNLIST,
    TG_GPU_EXEC_TIMEOUT, TG_GPU_EXEC_FAULT, TG_GPU_EXEC_FIFO_INIT,
    TG_GPU_EXEC_PBDMA_INIT, TG_GPU_EXEC_SCHEDULER, TG_GPU_EXEC_FRONTEND,
    TG_GPU_EXEC_GR_BOOT, TG_GPU_EXEC_ACR_BOOT, TG_GPU_EXEC_PMU_BOOT,
    TG_GPU_EXEC_GR_CONTEXT, TG_GPU_EXEC_IRQ, TG_GPU_EXEC_GR_ENGINE, TG_GPU_EXEC_PIXELS
};

/* A single diagnostic channel using caller-owned contiguous, uncached
 * memory below 4 GiB. Commands release two PBDMA semaphores; this proves
 * FIFO DMA execution, not GR firmware, shader execution or Vulkan drawing.
 * No user-provided command stream or physical address is accepted.
 */
struct tg_gpu_execution {
    bool owned;
    bool boot_ready;
    bool channel_bound;
    bool fifo_ready;
    bool pbdma_owned;
    bool gr_owned;
    bool acr_owned;
    uint32_t old_pbdma_enable;
    uint32_t ce_reset_mask;
    uint32_t stage;
    struct tg_gpu_channel channel;
};

/* Optional CPU-notification acceptance gate. arm runs after cold FIFO/GR
 * setup; wait requires a distinct ISR/DPC after each GPU report. The caller
 * masks/synchronizes the interrupt before execute_stop or power-down.
 */
struct tg_gpu_execution_notify {
    void *context;
    bool (*arm)(void *);
    bool (*wait)(void *,uint32_t expected_count);
};

bool tg_gpu_prepare_arena(void *cpu, uint64_t device_address, size_t bytes,
                          const uint32_t expected[2]);
bool tg_gpu_prepare_3d_arena(void *cpu,uint64_t device_address,size_t bytes,
                            const uint32_t expected[2]);
/* Called only after prepare_arena, before any GPU mapping becomes live. */
bool tg_gpu_prepare_gr_arena(void *cpu, uint64_t device_address, size_t bytes,
                            const struct tg_gpu_gr_payload *firmware);
bool tg_gpu_prepare_acr_arena(void *cpu, uint64_t device_address, size_t bytes);
/* Exclusive cold startup, shared by the diagnostic and future WDDM worker.
 * Copies/authenticates firmware, initializes FIFO/MMU/GR and saves the golden
 * image, but never schedules a runlist or publishes GP_PUT. Two distinct
 * expected values must be supplied as for execute; GPU report locations
 * retain their sentinels, never matching these expected values.
 * On success the GPU and both allocations stay owned; stop/drain/reset and
 * power retirement are still required before any backing can be freed.
 * Firmware source files may be released once this function returns. Success
 * means bootstrap readiness, not a submitted/completed application workload.
 */
enum tg_gpu_execute_result tg_gpu_boot_graphics(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,void *cpu,uint64_t device_address,
    const struct tg_gpu_gr_payload *,const struct tg_gpu_gr_work *,
    struct tg_gpu_execution *,struct tg_gpu_status *);
/* Idle cold engine only. Repoint the retained bootstrap BAR1 USERD alias to
 * a caller-owned, zeroed physical page; no GP_PUT is published. Both the new
 * page and bootstrap arena remain referenced even after a partial failure,
 * until full stop/drain/reset. The real context RAMIN is prepared separately.
 */
enum tg_gpu_execute_result tg_gpu_boot_map_userd(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,void *cpu,uint64_t arena_physical,
    uint64_t userd_physical,const struct tg_gpu_execution *,struct tg_gpu_status *);
enum tg_gpu_execute_result tg_gpu_execute(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, void *uncached_cpu,
    uint64_t device_address, const struct tg_gpu_gr_payload *firmware,
    struct tg_gpu_execution *state,
    struct tg_gpu_status *result);
/* Opt-in golden-image diagnostic; the original FIFO entry point is unchanged. */
enum tg_gpu_execute_result tg_gpu_execute_gr(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, void *uncached_cpu,
    uint64_t device_address, const struct tg_gpu_gr_payload *firmware,
    const struct tg_gpu_gr_work *work, struct tg_gpu_execution *state,
    struct tg_gpu_status *result);
enum tg_gpu_execute_result tg_gpu_execute_irq(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power,void *uncached_cpu,uint64_t device_address,
    const struct tg_gpu_gr_payload *firmware,const struct tg_gpu_execution_notify *,
    struct tg_gpu_execution *,struct tg_gpu_status *result);
/* Separate 3D-engine report diagnostic, using the extended channel-image
 * allocation. It neither accepts application commands nor runs shaders.
 */
enum tg_gpu_execute_result tg_gpu_execute_3d(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,void *cpu,uint64_t device_address,
    const struct tg_gpu_gr_payload *,const struct tg_gpu_gr_work *,
    struct tg_gpu_execution *,struct tg_gpu_status *);
enum tg_gpu_execute_result tg_gpu_execute_clear(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,void *cpu,uint64_t device_address,
    const struct tg_gpu_gr_payload *,const struct tg_gpu_gr_work *,
    struct tg_gpu_execution *,struct tg_gpu_status *);
enum tg_gpu_execute_result tg_gpu_execute_draw(const struct tg_gpu_power_io *,
    const struct tg_gpu_power *,void *cpu,uint64_t device_address,
    const struct tg_gpu_gr_payload *,const struct tg_gpu_gr_work *,
    struct tg_gpu_execution *,struct tg_gpu_status *);
/* Stop channel fetch before the caller drains MC, stops the PLL and powers
 * down. Clear execution ownership/free the arena only AFTER power_down
 * confirms no ownership. The probe does not change any Tegra SMMU domain.
 */
void tg_gpu_execute_stop(const struct tg_gpu_power_io *io, struct tg_gpu_execution *state);
void tg_gpu_execute_snapshot(const struct tg_gpu_power_io *io,
    const struct tg_gpu_execution *state, struct tg_gpu_status *result);
#endif
