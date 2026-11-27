/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_T214_DC_H
#define TG_GPU_T214_DC_H

#include <stdbool.h>
#include <stdint.h>

/* Tegra210 display A word indices. The direct Window A blocks at 0xb80
 * and 0xbc0 avoid changes to the indexed window selector.
 * Register layout: Linux v6.12 drivers/gpu/drm/tegra/{dc.c,dc.h}.
 * The mode constants describe Hekate bdk/display/di{.c,.inl}.
 */
#define T214_DC_COMMAND            0x032
#define T214_DC_INT_STATUS         0x037
#define T214_DC_INT_MASK           0x038
#define T214_DC_INT_ENABLE         0x039
#define T214_DC_STATE_ACCESS       0x040
#define T214_DC_STATE_CONTROL      0x041
#define T214_DC_REG_ACT_CONTROL    0x043
#define T214_DC_OUTPUT_OPTIONS     0x402
#define T214_DC_SYNC_WIDTH         0x407
#define T214_DC_BACK_PORCH         0x408
#define T214_DC_ACTIVE_SIZE        0x409
#define T214_DC_FRONT_PORCH        0x40a
#define T214_DC_WIN_A_OPTIONS      0xb80
#define T214_DC_WIN_A_BYTE_SWAP    0xb81
#define T214_DC_WIN_A_BUFFER       0xb82
#define T214_DC_WIN_A_COLOR        0xb83
#define T214_DC_WIN_A_POSITION     0xb84
#define T214_DC_WIN_A_SIZE         0xb85
#define T214_DC_WIN_A_PRESCALED    0xb86
#define T214_DC_WIN_A_H_INITIAL_DDA 0xb87
#define T214_DC_WIN_A_V_INITIAL_DDA 0xb88
#define T214_DC_WIN_A_DDA          0xb89
#define T214_DC_WIN_A_STRIDE       0xb8a
#define T214_DC_WIN_A_ADDR         0xbc0
#define T214_DC_WIN_A_H_OFFSET     0xbc6
#define T214_DC_WIN_A_V_OFFSET     0xbc8
#define T214_DC_WIN_A_UNDERFLOW    0xbca
#define T214_DC_WIN_A_SURFACE_KIND 0xbcb
#define T214_DC_WIN_A_ADDR_HI      0xbcd

#define T214_DC_MODE_MASK          (3u << 5)
#define T214_DC_MODE_CONTINUOUS    (1u << 5)
#define T214_DC_VBLANK_INT         (1u << 2)
#define T214_DC_READ_ACTIVE        (1u << 0)
#define T214_DC_GENERAL_ACT_REQ    (1u << 0)
#define T214_DC_WIN_A_ACT_REQ      (1u << 1)
#define T214_DC_WIN_A_HCOUNTER     (1u << 2)
#define T214_DC_DSI_ENABLE         (1u << 29)
#define T214_DC_WIN_ENABLE         (1u << 30)
/* Only used to reject/clear an inherited transform, never to rotate scanout. */
#define T214_DC_WIN_TRANSFORM_BITS 0x15u
#define T214_DC_COLOR_BGRA8888     12u
#define T214_DC_PACK(h, v)         ((h) | ((v) << 16))

#define T214_DC_PANEL_WIDTH        720u
#define T214_DC_PANEL_HEIGHT       1280u
#define T214_DC_PANEL_PITCH        (T214_DC_PANEL_WIDTH * 4u)
#define T214_DC_PANEL_ACTIVE       T214_DC_PACK(T214_DC_PANEL_WIDTH, T214_DC_PANEL_HEIGHT)
#define T214_DC_PANEL_SYNC         T214_DC_PACK(72u, 1u)
#define T214_DC_PANEL_BACK         T214_DC_PACK(72u, 9u)
#define T214_DC_PANEL_FRONT        T214_DC_PACK(136u, 10u)
#define T214_DC_PANEL_PRESCALED    T214_DC_PACK(T214_DC_PANEL_PITCH, T214_DC_PANEL_HEIGHT)
#define T214_DC_DDA_IDENTITY       T214_DC_PACK(0x1000u, 0x1000u)

struct tg_t214_dc_scanout {
    uint64_t address;
    bool visible;
};

struct tg_t214_dc_io {
    void *context;
    uint32_t (*read32)(void *context, uint32_t word);
    void (*write32)(void *context, uint32_t word, uint32_t value);
};

struct tg_t214_dc {
    struct tg_t214_dc_io io;
    bool owned;
    bool vblank;
    bool boot_hcounter;
};

enum tg_t214_dc_result {
    TG_T214_DC_OK,
    TG_T214_DC_INVALID,
    TG_T214_DC_BUSY,
    TG_T214_DC_MODE_MISMATCH
};

/* Read the selected register bank without changing the controller. */
bool tg_t214_dc_has_boot_mode(const struct tg_t214_dc_io *io);

/* Zero-initialize state. The caller owns the writable Display A resource
 * and serializes all access, including its interrupt handler. These calls
 * do not sleep. Keep every queued or active scanout allocation pinned.
 */
enum tg_t214_dc_result tg_t214_dc_acquire(struct tg_t214_dc *dc,
    const struct tg_t214_dc_io *io);
void tg_t214_dc_set_vblank(struct tg_t214_dc *dc, bool enabled);
bool tg_t214_dc_ack_vblank(struct tg_t214_dc *dc);
uint64_t tg_t214_dc_active_address(struct tg_t214_dc *dc);
/* Read the active bank only after its pending activation has completed. */
bool tg_t214_dc_latched_scanout(const struct tg_t214_dc *dc, uint64_t *address, bool *visible);
/* Confirm the active address and complete mutable fetch layout together. */
bool tg_t214_dc_latched_surface(const struct tg_t214_dc *dc, struct tg_t214_dc_scanout *scanout);
/* Layout/visibility changes latch at VBlank even for an immediate request.
 * Only an address change with the same fetch layout may use HBlank. */
enum tg_t214_dc_result tg_t214_dc_queue_surface(struct tg_t214_dc *dc,
    const struct tg_t214_dc_scanout *scanout, bool immediate);
/* This fixed-mode path scans a contiguous, page-aligned native surface
 * entirely below 4 GiB. The caller owns its backing and cache visibility.
 */
enum tg_t214_dc_result tg_t214_dc_queue_flip(struct tg_t214_dc *dc, uint64_t address);
/* Change visibility and address in the same native Window A activation. */
enum tg_t214_dc_result tg_t214_dc_queue_scanout(struct tg_t214_dc *dc,
    uint64_t address, bool visible);
/* HBlank is permitted for an address-only change. Visibility changes still
 * latch at VBlank. The caller must confirm the active bank before completing
 * an immediate request. */
enum tg_t214_dc_result tg_t214_dc_queue_immediate(struct tg_t214_dc *dc,
    uint64_t address,bool visible);
/* Queue and confirm the original POST address first. Retry BUSY until
 * the restore activation completes before unmapping or freeing scanout.
 */
enum tg_t214_dc_result tg_t214_dc_release(struct tg_t214_dc *dc);

#endif
