/* SPDX-License-Identifier: MIT */
#include "../platform/t214_dc.h"
#include "../platform/t214_resources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DC_CHECK assert
#include "dc_model.h"

static void portrait(const struct display_model *m)
{
    assert(!(m->active[T214_DC_WIN_A_OPTIONS] & 0x15));
    assert(m->active[T214_DC_WIN_A_STRIDE] == 2880);
    assert(!m->active[T214_DC_WIN_A_H_OFFSET] && !m->active[T214_DC_WIN_A_V_OFFSET]);
    for (unsigned y=0; y<1280; ++y) for (unsigned x=0; x<720; ++x) {
        const uint64_t offset=(uint64_t)y*m->active[T214_DC_WIN_A_STRIDE]+4*x;
        assert(offset==((uint64_t)y*720+x)*4 && offset+4<=T214_GPU_POST_FB_SIZE);
    }
}
int main(void)
{
    static struct display_model m;
    initialize(&m);
    struct tg_t214_dc dc={0};
    const struct tg_t214_dc_io io={&m,read_reg,write_reg};
    assert(tg_t214_dc_acquire(&dc,&io)==TG_T214_DC_OK);
    latch(&m,true);
    struct tg_t214_dc_scanout active;
    assert(tg_t214_dc_latched_surface(&dc,&active));portrait(&m);
    for(unsigned i=0;i<3;++i) {
        struct tg_t214_dc_scanout next={0xc1000000+i*0x400000,true};
        assert(tg_t214_dc_queue_surface(&dc,&next,false)==TG_T214_DC_OK);
        assert(!tg_t214_dc_latched_surface(&dc,&active));
        latch(&m,false);assert(!tg_t214_dc_latched_surface(&dc,&active));
        latch(&m,true);assert(tg_t214_dc_latched_surface(&dc,&active) && active.address==next.address);
        portrait(&m);
        next.address+=0x400000;
        assert(tg_t214_dc_queue_surface(&dc,&next,true)==TG_T214_DC_OK);
        latch(&m,false);assert(tg_t214_dc_latched_surface(&dc,&active) && active.address==next.address);
        latch(&m,true);
        m.active[T214_DC_WIN_A_OPTIONS]|=0x10;
        assert(!tg_t214_dc_latched_surface(&dc,&active));
        m.active[T214_DC_WIN_A_OPTIONS]&=~0x10u;
        m.active[T214_DC_WIN_A_STRIDE]^=64;
        assert(!tg_t214_dc_latched_surface(&dc,&active));
        m.active[T214_DC_WIN_A_STRIDE]^=64;
        m.active[T214_DC_WIN_A_H_OFFSET]=4;
        assert(!tg_t214_dc_latched_surface(&dc,&active));
        m.active[T214_DC_WIN_A_H_OFFSET]=0;
        assert(m.assembly[T214_DC_STATE_ACCESS]==0);
    }
    unsigned writes=m.writes;
    struct tg_t214_dc_scanout bad={0xfff00000,true};
    assert(tg_t214_dc_queue_surface(&dc,&bad,false)==TG_T214_DC_INVALID && m.writes==writes);
    bad.address=0xc1000004;
    assert(tg_t214_dc_queue_surface(&dc,&bad,false)==TG_T214_DC_INVALID && m.writes==writes);
    assert(tg_t214_dc_queue_scanout(&dc,T214_GPU_POST_FB_BASE,true)==TG_T214_DC_OK);
    assert(tg_t214_dc_release(&dc)==TG_T214_DC_BUSY);
    latch(&m,true);portrait(&m);
    assert(tg_t214_dc_release(&dc)==TG_T214_DC_BUSY);
    latch(&m,true);assert(tg_t214_dc_release(&dc)==TG_T214_DC_OK && !dc.owned);
    puts("T214 portrait-only fetch, foreign-layout rejection, full-frame addressing and latch: PASS (model only)");
    return 0;
}
