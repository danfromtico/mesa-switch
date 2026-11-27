/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_PANEL_SNAPSHOT_H
#define TG_GPU_PANEL_SNAPSHOT_H
#include <stdint.h>

/* One privileged, read-only D3DKMT_ESCAPE_DRIVERPRIVATE request. No caller
 * address, offset, register selector or GPU command is accepted. Samples are
 * observations of the active backing, not a GPU fence or physical-screen pass.
 */
#define TG_GPU_PANEL_SNAPSHOT_MAGIC 0x53504754u
#define TG_GPU_PANEL_SNAPSHOT_VERSION 1u
#define TG_GPU_PANEL_SNAPSHOT_STABLE 1u
#define TG_GPU_PANEL_SNAPSHOT_LAYOUT 2u
#define TG_GPU_PANEL_SNAPSHOT_PIXELS 4u

struct tg_gpu_panel_snapshot {
    uint32_t magic, version, size, flags;
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t path_rotation, source_visible, target_active;
    uint32_t state_control_before, state_control_after, interrupt_status, underflow;
    uint32_t options, pitch, h_offset, v_offset, surface_kind;
    uint32_t output_size, prescaled_size, dda, panel_active;
    uint64_t address, pending_address, displayed_address;
    uint64_t pending_present, displayed_present;
    uint32_t rotation, width, height, reserved;
    struct { uint32_t x, y, bgra; } pixels[4];
};
#endif
