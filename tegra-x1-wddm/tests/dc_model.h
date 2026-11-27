/* SPDX-License-Identifier: MIT
 * Explicit assembly/armed/active register banks for display controls.
 * DC_CHECK is supplied by the host or Windows test harness.
 */
#ifndef TG_TEST_DC_MODEL_H
#define TG_TEST_DC_MODEL_H
#include "../platform/t214_dc.h"
#include "../platform/t214_resources.h"

struct display_model {
    uint32_t assembly[T214_GPU_DC_SIZE / 4];
    uint32_t armed[T214_GPU_DC_SIZE / 4];
    uint32_t active[T214_GPU_DC_SIZE / 4];
    uint32_t update;
    unsigned writes;
};

static uint32_t read_reg(void *context, uint32_t word)
{
    const struct display_model *m = context;
    DC_CHECK(word < T214_GPU_DC_SIZE / 4);
    if ((m->assembly[T214_DC_STATE_ACCESS] & T214_DC_READ_ACTIVE) &&
        word >= 0xb80 && word <= 0xbcd)
        return m->active[word];
    return m->assembly[word];
}

static void write_reg(void *context, uint32_t word, uint32_t value)
{
    struct display_model *m = context;
    ++m->writes;
    switch (word) {
    case T214_DC_STATE_CONTROL:
        if (value & 0x300u) {
            DC_CHECK(!(value & ~0x300u));
            m->update = value >> 8;
            memcpy(m->armed, m->assembly, sizeof(m->armed));
        } else {
            DC_CHECK(value && value == m->update);
            m->assembly[word] |= value;
            m->update = 0;
        }
        return;
    case T214_DC_INT_STATUS:
        DC_CHECK(value == T214_DC_VBLANK_INT);
        m->assembly[word] &= ~value;
        return;
    case T214_DC_STATE_ACCESS: case T214_DC_REG_ACT_CONTROL:
    case T214_DC_INT_MASK: case T214_DC_INT_ENABLE:
    case T214_DC_WIN_A_OPTIONS: case T214_DC_WIN_A_STRIDE:
    case T214_DC_WIN_A_H_OFFSET: case T214_DC_WIN_A_V_OFFSET:
    case T214_DC_WIN_A_ADDR:
        m->assembly[word] = value;
        return;
    default:
        /* Rotation must not change the inherited DSI timing or pixel format. */
        DC_CHECK(!"unexpected display register write");
    }
}

static void latch(struct display_model *m, bool vblank)
{
    if ((vblank || (m->assembly[T214_DC_REG_ACT_CONTROL] & T214_DC_WIN_A_HCOUNTER)) &&
        (m->assembly[T214_DC_STATE_CONTROL] & T214_DC_WIN_A_ACT_REQ)) {
        memcpy(&m->active[0xb80], &m->armed[0xb80], (0xbce - 0xb80) * sizeof(uint32_t));
        m->assembly[T214_DC_STATE_CONTROL] &= ~T214_DC_WIN_A_ACT_REQ;
    }
    if (vblank) m->assembly[T214_DC_STATE_CONTROL] = 0;
}

static void initialize(struct display_model *m)
{
    memset(m, 0, sizeof(*m));
    uint32_t *r = m->assembly;
    r[T214_DC_COMMAND] = T214_DC_MODE_CONTINUOUS;
    r[T214_DC_OUTPUT_OPTIONS] = T214_DC_DSI_ENABLE;
    r[T214_DC_ACTIVE_SIZE] = T214_DC_PANEL_ACTIVE;
    r[T214_DC_SYNC_WIDTH] = T214_DC_PANEL_SYNC;
    r[T214_DC_BACK_PORCH] = T214_DC_PANEL_BACK;
    r[T214_DC_FRONT_PORCH] = T214_DC_PANEL_FRONT;
    r[T214_DC_WIN_A_OPTIONS] = T214_DC_WIN_ENABLE;
    r[T214_DC_WIN_A_COLOR] = T214_DC_COLOR_BGRA8888;
    r[T214_DC_WIN_A_SIZE] = T214_DC_PANEL_ACTIVE;
    r[T214_DC_WIN_A_PRESCALED] = T214_DC_PANEL_PRESCALED;
    r[T214_DC_WIN_A_DDA] = T214_DC_DDA_IDENTITY;
    r[T214_DC_WIN_A_STRIDE] = 720 * 4;
    r[T214_DC_WIN_A_ADDR] = T214_GPU_POST_FB_BASE;
    r[T214_DC_REG_ACT_CONTROL] = 0x454;
    memcpy(m->active, r, sizeof(m->active));
}

#endif
