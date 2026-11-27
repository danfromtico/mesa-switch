/* SPDX-License-Identifier: MIT
 * Copyright 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2026 ruje0504-2
 * Pitch target/clear state follows pinned Mesa NVK nvk_cmd_draw.c and
 * nvk_cmd_clear.c (0ff52079). Maxwell definitions are unchanged NVIDIA class
 * headers from that tree. See ../NOTICE.
 */
#include "gr_clear.h"
#include "gr_submit.h"
#include "report.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb197.h"

static void method(uint32_t *p,unsigned *n,uint32_t address,uint32_t value)
{
    p[(*n)++]=NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE)|
        NVVAL(NVB06F,DMA_INCR,COUNT,1)|NVVAL(NVB06F,DMA_INCR,ADDRESS,address>>2);
    p[(*n)++]=value;
}
static void immediate(uint32_t *p,unsigned *n,uint32_t address,uint32_t value)
{
    p[(*n)++]=NVDEF(NVB06F,DMA_IMMD,OPCODE,VALUE)|
        NVVAL(NVB06F,DMA_IMMD,DATA,value)|NVVAL(NVB06F,DMA_IMMD,ADDRESS,address>>2);
}
bool tg_gpu_build_clear_packet(uint32_t *p,size_t capacity,uint64_t image_va,
    uint64_t report_va,uint32_t value,bool first,uint32_t *words)
{
    if(!p || !words || capacity<TG_GPU_CLEAR_MAX_WORDS || !image_va ||
       (image_va&127) || image_va>TG_GPU_VA_LIMIT-TG_GPU_CLEAR_BYTES ||
       (report_va&3) || report_va>=TG_GPU_VA_LIMIT ||
       (report_va>=image_va && report_va<image_va+TG_GPU_CLEAR_BYTES)) return false;
    unsigned n=0;
    if(first) {
        method(p,&n,NVB197_SET_OBJECT,TG_GPU_GR_ENGINE_CLASS);
        /* Pitch must be 128B-aligned; WIDTH is pitch in bytes in this layout.
         * Single target/layer, no compression, MSAA, depth or conditional draw.
         */
        method(p,&n,NVB197_SET_COLOR_TARGET_A(0),(uint32_t)(image_va>>32));
        method(p,&n,NVB197_SET_COLOR_TARGET_B(0),(uint32_t)image_va);
        method(p,&n,NVB197_SET_COLOR_TARGET_WIDTH(0),TG_GPU_CLEAR_PITCH);
        method(p,&n,NVB197_SET_COLOR_TARGET_HEIGHT(0),TG_GPU_CLEAR_HEIGHT);
        method(p,&n,NVB197_SET_COLOR_TARGET_FORMAT(0),TG_GPU_CLEAR_FORMAT);
        method(p,&n,NVB197_SET_COLOR_TARGET_MEMORY(0),
            NVDEF(NVB197,SET_COLOR_TARGET_MEMORY,LAYOUT,PITCH));
        method(p,&n,NVB197_SET_COLOR_TARGET_THIRD_DIMENSION(0),1);
        method(p,&n,NVB197_SET_COLOR_TARGET_ARRAY_PITCH(0),0);
        method(p,&n,NVB197_SET_COLOR_TARGET_LAYER(0),0);
        immediate(p,&n,NVB197_SET_COLOR_COMPRESSION(0),0);
        immediate(p,&n,NVB197_SET_CT_SELECT,1);
        immediate(p,&n,NVB197_SET_ZT_SELECT,0);
        immediate(p,&n,NVB197_SET_RT_LAYER,0);
        immediate(p,&n,NVB197_SET_ANTI_ALIAS,0);
        immediate(p,&n,NVB197_SET_ANTI_ALIAS_ENABLE,0);
        /* Do not add SET_ANTI_ALIAS_RASTER (0xfb8): GM20B's physical class
         * rejected it with class-error 4. Pinned NVK does not emit it here.
         */
        immediate(p,&n,NVB197_SET_WINDOW_CLIP_ENABLE,0);
        immediate(p,&n,NVB197_SET_SRGB_WRITE,0);
        immediate(p,&n,NVB197_SET_RENDER_ENABLE_C,1);
        immediate(p,&n,NVB197_SET_RENDER_ENABLE_OVERRIDE,1);
        method(p,&n,NVB197_SET_SURFACE_CLIP_HORIZONTAL,TG_GPU_CLEAR_WIDTH<<16);
        method(p,&n,NVB197_SET_SURFACE_CLIP_VERTICAL,TG_GPU_CLEAR_HEIGHT<<16);
        immediate(p,&n,NVB197_SET_CLEAR_SURFACE_CONTROL,
            NVDEF(NVB197,SET_CLEAR_SURFACE_CONTROL,USE_CLEAR_RECT,TRUE));
    }
    uint32_t low=first?0:4,high=first?16:12;
    method(p,&n,NVB197_SET_CLEAR_RECT_HORIZONTAL,low|(high<<16));
    method(p,&n,NVB197_SET_CLEAR_RECT_VERTICAL,low|(high<<16));
    for(unsigned c=0;c<4;++c) {
        bool one=c==3 || (first?c==2:c==0);
        method(p,&n,NVB197_SET_COLOR_CLEAR_VALUE(c),one?0x3f800000u:0);
    }
    immediate(p,&n,NVB197_CLEAR_SURFACE,0x3c); /* RGBA, target/layer zero */
    immediate(p,&n,NVB197_WAIT_FOR_IDLE,0);
    if(!tg_gpu_build_report(p+n,capacity-n,report_va,value)) return false;
    n+=TG_GPU_REPORT_WORDS;*words=n;
    return n<=TG_GPU_CLEAR_MAX_WORDS;
}
bool tg_gpu_check_clear_pixels(const volatile uint32_t *p,size_t bytes,
    struct tg_gpu_clear_status *s)
{
    if(!p || !s || bytes<TG_GPU_CLEAR_BYTES) return false;
    *s=(struct tg_gpu_clear_status){.checked=1,.width=TG_GPU_CLEAR_WIDTH,
        .height=TG_GPU_CLEAR_HEIGHT,.pitch=TG_GPU_CLEAR_PITCH,.format=TG_GPU_CLEAR_FORMAT,
        .image_hash=2166136261u};
    for(unsigned i=0;i<TG_GPU_CLEAR_BYTES/4;++i) {
        unsigned x=i%(TG_GPU_CLEAR_PITCH/4),y=i/(TG_GPU_CLEAR_PITCH/4);
        uint32_t value=p[i];
        if(x<TG_GPU_CLEAR_WIDTH && y<TG_GPU_CLEAR_HEIGHT) {
            bool central=x>=4 && x<12 && y>=4 && y<12;
            if(value!=(central?TG_GPU_CLEAR_FOREGROUND:TG_GPU_CLEAR_BACKGROUND)) ++s->errors;
            s->image_hash=(s->image_hash^value)*16777619u;++s->pixels;
        } else if(value!=TG_GPU_CLEAR_SENTINEL) ++s->guard_errors;
        if(!i)s->background=value;
        if(x==4&&y==4)s->foreground=value;
    }
    return !s->errors&&!s->guard_errors;
}
