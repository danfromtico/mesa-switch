/* SPDX-License-Identifier: MIT */
#include "t214_dc.h"
#include "t214_resources.h"
#include <stddef.h>

static uint32_t read_reg(const struct tg_t214_dc_io *io, uint32_t word)
{
    return io->read32(io->context, word);
}

static void write_reg(const struct tg_t214_dc_io *io, uint32_t word, uint32_t value)
{
    io->write32(io->context, word, value);
}

static bool activation_pending(const struct tg_t214_dc_io *io)
{
    return (read_reg(io, T214_DC_STATE_CONTROL) &
        (T214_DC_GENERAL_ACT_REQ | T214_DC_WIN_A_ACT_REQ)) != 0;
}

static void activate(const struct tg_t214_dc_io *io, uint32_t group)
{
    /* Copy assembly to the armed bank, then request the hardware latch. */
    write_reg(io, T214_DC_STATE_CONTROL, group << 8);
    (void)read_reg(io, T214_DC_STATE_CONTROL);
    write_reg(io, T214_DC_STATE_CONTROL, group);
    (void)read_reg(io, T214_DC_STATE_CONTROL);
}

bool tg_t214_dc_has_boot_mode(const struct tg_t214_dc_io *io)
{
    return !activation_pending(io) &&
        (read_reg(io, T214_DC_COMMAND) & T214_DC_MODE_MASK) == T214_DC_MODE_CONTINUOUS &&
        (read_reg(io, T214_DC_OUTPUT_OPTIONS) & T214_DC_DSI_ENABLE) != 0 &&
        read_reg(io, T214_DC_ACTIVE_SIZE) == T214_DC_PANEL_ACTIVE &&
        read_reg(io, T214_DC_SYNC_WIDTH) == T214_DC_PANEL_SYNC &&
        read_reg(io, T214_DC_BACK_PORCH) == T214_DC_PANEL_BACK &&
        read_reg(io, T214_DC_FRONT_PORCH) == T214_DC_PANEL_FRONT &&
        read_reg(io, T214_DC_WIN_A_OPTIONS) == T214_DC_WIN_ENABLE &&
        read_reg(io, T214_DC_WIN_A_BYTE_SWAP) == 0 &&
        read_reg(io, T214_DC_WIN_A_BUFFER) == 0 &&
        read_reg(io, T214_DC_WIN_A_COLOR) == T214_DC_COLOR_BGRA8888 &&
        read_reg(io, T214_DC_WIN_A_POSITION) == 0 &&
        read_reg(io, T214_DC_WIN_A_SIZE) == T214_DC_PANEL_ACTIVE &&
        read_reg(io, T214_DC_WIN_A_PRESCALED) == T214_DC_PANEL_PRESCALED &&
        read_reg(io, T214_DC_WIN_A_H_INITIAL_DDA) == 0 &&
        read_reg(io, T214_DC_WIN_A_V_INITIAL_DDA) == 0 &&
        read_reg(io, T214_DC_WIN_A_DDA) == T214_DC_DDA_IDENTITY &&
        (read_reg(io, T214_DC_WIN_A_STRIDE) & 0xffffu) == T214_DC_PANEL_PITCH &&
        read_reg(io, T214_DC_WIN_A_ADDR) == T214_GPU_POST_FB_BASE &&
        read_reg(io, T214_DC_WIN_A_ADDR_HI) == 0 &&
        read_reg(io, T214_DC_WIN_A_H_OFFSET) == 0 &&
        read_reg(io, T214_DC_WIN_A_V_OFFSET) == 0 &&
        read_reg(io, T214_DC_WIN_A_SURFACE_KIND) == 0 &&
        !activation_pending(io);
}

static uint64_t active_address(const struct tg_t214_dc_io *io, bool *visible)
{
    uint32_t access = read_reg(io, T214_DC_STATE_ACCESS);
    write_reg(io, T214_DC_STATE_ACCESS, access | T214_DC_READ_ACTIVE);
    uint32_t lo = read_reg(io, T214_DC_WIN_A_ADDR);
    uint32_t hi = read_reg(io, T214_DC_WIN_A_ADDR_HI);
    if (visible)
        *visible = (read_reg(io, T214_DC_WIN_A_OPTIONS) & T214_DC_WIN_ENABLE) != 0;
    write_reg(io, T214_DC_STATE_ACCESS, access);
    (void)read_reg(io, T214_DC_STATE_ACCESS);
    return ((uint64_t)hi << 32) | lo;
}

enum tg_t214_dc_result tg_t214_dc_acquire(struct tg_t214_dc *dc,
    const struct tg_t214_dc_io *io)
{
    if (!dc || !io || !io->read32 || !io->write32)
        return TG_T214_DC_INVALID;
    if (dc->owned || read_reg(io, T214_DC_STATE_ACCESS) != 0)
        return TG_T214_DC_BUSY;
    uint32_t mask = read_reg(io, T214_DC_INT_MASK);
    uint32_t enable = read_reg(io, T214_DC_INT_ENABLE);
    if ((mask & enable) || ((mask | enable) & T214_DC_VBLANK_INT))
        return TG_T214_DC_BUSY;
    if (!tg_t214_dc_has_boot_mode(io) || active_address(io, NULL) != T214_GPU_POST_FB_BASE)
        return TG_T214_DC_MODE_MISMATCH;

    uint32_t act = read_reg(io, T214_DC_REG_ACT_CONTROL);
    dc->io = *io;
    dc->boot_hcounter = (act & T214_DC_WIN_A_HCOUNTER) != 0;
    dc->vblank = false;
    dc->owned = true;
    if (dc->boot_hcounter) {
        write_reg(io, T214_DC_REG_ACT_CONTROL, act & ~T214_DC_WIN_A_HCOUNTER);
        activate(io, T214_DC_GENERAL_ACT_REQ);
    }
    return TG_T214_DC_OK;
}

void tg_t214_dc_set_vblank(struct tg_t214_dc *dc, bool enabled)
{
    if (!dc->owned || dc->vblank == enabled)
        return;
    const struct tg_t214_dc_io *io = &dc->io;
    uint32_t mask = read_reg(io, T214_DC_INT_MASK);
    uint32_t enable = read_reg(io, T214_DC_INT_ENABLE);
    if (enabled) {
        write_reg(io, T214_DC_INT_STATUS, T214_DC_VBLANK_INT);
        write_reg(io, T214_DC_INT_ENABLE, enable | T214_DC_VBLANK_INT);
        write_reg(io, T214_DC_INT_MASK, mask | T214_DC_VBLANK_INT);
    } else {
        write_reg(io, T214_DC_INT_MASK, mask & ~T214_DC_VBLANK_INT);
        write_reg(io, T214_DC_INT_ENABLE, enable & ~T214_DC_VBLANK_INT);
        write_reg(io, T214_DC_INT_STATUS, T214_DC_VBLANK_INT);
    }
    (void)read_reg(io, T214_DC_INT_STATUS);
    dc->vblank = enabled;
}

bool tg_t214_dc_ack_vblank(struct tg_t214_dc *dc)
{
    const struct tg_t214_dc_io *io = &dc->io;
    if (!dc->owned || !dc->vblank || !(read_reg(io, T214_DC_INT_STATUS) & T214_DC_VBLANK_INT))
        return false;
    write_reg(io, T214_DC_INT_STATUS, T214_DC_VBLANK_INT);
    (void)read_reg(io, T214_DC_INT_STATUS);
    return true;
}

uint64_t tg_t214_dc_active_address(struct tg_t214_dc *dc)
{
    return dc->owned ? active_address(&dc->io, NULL) : 0;
}

bool tg_t214_dc_latched_scanout(const struct tg_t214_dc *dc, uint64_t *address, bool *visible)
{
    struct tg_t214_dc_scanout scanout;
    if (!tg_t214_dc_latched_surface(dc, &scanout))
        return false;
    *address = scanout.address;
    if (visible) *visible = scanout.visible;
    return true;
}

bool tg_t214_dc_latched_surface(const struct tg_t214_dc *dc, struct tg_t214_dc_scanout *scanout)
{
    if (!dc || !dc->owned || !scanout ||
        (read_reg(&dc->io, T214_DC_STATE_CONTROL) & T214_DC_WIN_A_ACT_REQ))
        return false;
    const struct tg_t214_dc_io *io = &dc->io;
    const uint32_t access = read_reg(io, T214_DC_STATE_ACCESS);
    write_reg(io, T214_DC_STATE_ACCESS, access | T214_DC_READ_ACTIVE);
    const uint64_t address = (uint64_t)read_reg(io, T214_DC_WIN_A_ADDR) |
        (uint64_t)read_reg(io, T214_DC_WIN_A_ADDR_HI) << 32;
    const uint32_t options = read_reg(io, T214_DC_WIN_A_OPTIONS);
    const uint32_t pitch = read_reg(io, T214_DC_WIN_A_STRIDE) & 0xffffu;
    const uint32_t h_offset = read_reg(io, T214_DC_WIN_A_H_OFFSET);
    const uint32_t v_offset = read_reg(io, T214_DC_WIN_A_V_OFFSET);
    write_reg(io, T214_DC_STATE_ACCESS, access);
    (void)read_reg(io, T214_DC_STATE_ACCESS);
    if ((options & T214_DC_WIN_TRANSFORM_BITS) || pitch != T214_DC_PANEL_PITCH ||
        h_offset || v_offset)
        return false;
    *scanout = (struct tg_t214_dc_scanout){address, (options & T214_DC_WIN_ENABLE) != 0};
    return true;
}

enum tg_t214_dc_result tg_t214_dc_queue_surface(struct tg_t214_dc *dc,
    const struct tg_t214_dc_scanout *scanout, bool immediate)
{
    if (!dc || !dc->owned || !scanout ||
        (scanout->address & 0xfffu) || scanout->address < 0x80000000u ||
        scanout->address > (UINT64_C(1) << 32) - T214_GPU_POST_FB_SIZE)
        return TG_T214_DC_INVALID;
    const struct tg_t214_dc_io *io = &dc->io;
    if (activation_pending(io))
        return TG_T214_DC_BUSY;
    uint32_t options = read_reg(io, T214_DC_WIN_A_OPTIONS);
    const bool same_layout = !(options & T214_DC_WIN_TRANSFORM_BITS) &&
        (read_reg(io, T214_DC_WIN_A_STRIDE) & 0xffffu) == T214_DC_PANEL_PITCH &&
        read_reg(io, T214_DC_WIN_A_H_OFFSET) == 0 &&
        read_reg(io, T214_DC_WIN_A_V_OFFSET) == 0;
    const bool hcounter = immediate && same_layout &&
        scanout->visible == ((options & T214_DC_WIN_ENABLE) != 0);
    const uint32_t act = read_reg(io, T214_DC_REG_ACT_CONTROL);
    const uint32_t next = (act & ~T214_DC_WIN_A_HCOUNTER) | (hcounter ? T214_DC_WIN_A_HCOUNTER : 0);
    uint32_t group = T214_DC_WIN_A_ACT_REQ;
    if (next != act) {
        write_reg(io, T214_DC_REG_ACT_CONTROL, next);
        group |= T214_DC_GENERAL_ACT_REQ;
    }
    write_reg(io, T214_DC_WIN_A_OPTIONS,
        (options & ~(T214_DC_WIN_ENABLE | T214_DC_WIN_TRANSFORM_BITS)) |
        (scanout->visible ? T214_DC_WIN_ENABLE : 0));
    if (!same_layout) {
        write_reg(io, T214_DC_WIN_A_STRIDE,
            (read_reg(io, T214_DC_WIN_A_STRIDE) & 0xffff0000u) | T214_DC_PANEL_PITCH);
        write_reg(io, T214_DC_WIN_A_H_OFFSET, 0);
        write_reg(io, T214_DC_WIN_A_V_OFFSET, 0);
    }
    write_reg(io, T214_DC_WIN_A_ADDR, (uint32_t)scanout->address);
    activate(io, group);
    return TG_T214_DC_OK;
}

enum tg_t214_dc_result tg_t214_dc_queue_flip(struct tg_t214_dc *dc, uint64_t address)
{
    if (!dc || !dc->owned) return TG_T214_DC_INVALID;
    const struct tg_t214_dc_scanout scanout = {address,
        (read_reg(&dc->io, T214_DC_WIN_A_OPTIONS) & T214_DC_WIN_ENABLE) != 0};
    return tg_t214_dc_queue_surface(dc, &scanout, false);
}

enum tg_t214_dc_result tg_t214_dc_queue_scanout(struct tg_t214_dc *dc,
    uint64_t address, bool visible)
{
    const struct tg_t214_dc_scanout scanout = {address, visible};
    return tg_t214_dc_queue_surface(dc, &scanout, false);
}

enum tg_t214_dc_result tg_t214_dc_queue_immediate(struct tg_t214_dc *dc,
    uint64_t address,bool visible)
{
    const struct tg_t214_dc_scanout scanout = {address, visible};
    return tg_t214_dc_queue_surface(dc, &scanout, true);
}

enum tg_t214_dc_result tg_t214_dc_release(struct tg_t214_dc *dc)
{
    if (!dc->owned)
        return TG_T214_DC_OK;
    const struct tg_t214_dc_io *io = &dc->io;
    struct tg_t214_dc_scanout scanout;
    if (activation_pending(io) || !tg_t214_dc_latched_surface(dc, &scanout) ||
        scanout.address != T214_GPU_POST_FB_BASE || !scanout.visible)
        return TG_T214_DC_BUSY;

    uint32_t act = read_reg(io, T214_DC_REG_ACT_CONTROL);
    uint32_t boot_act = (act & ~T214_DC_WIN_A_HCOUNTER) |
        (dc->boot_hcounter ? T214_DC_WIN_A_HCOUNTER : 0);
    if (act != boot_act) {
        write_reg(io, T214_DC_REG_ACT_CONTROL, boot_act);
        activate(io, T214_DC_GENERAL_ACT_REQ);
        if (activation_pending(io))
            return TG_T214_DC_BUSY;
    }
    tg_t214_dc_set_vblank(dc, false);
    dc->owned = false;
    return TG_T214_DC_OK;
}
