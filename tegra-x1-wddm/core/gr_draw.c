/* SPDX-License-Identifier: MIT
 * Copyright 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2026 ruje0504-2
 * Fixed no-descriptor graphics state follows pinned Mesa NVK nvk_shader.c and
 * nvk_cmd_draw.c (0ff52079). See ../NOTICE. Shader algorithms are unchanged NAK.
 */
#define TG_GPU_DRAW_INCLUDE_CODE
#include "gr_draw.h"
#include "report.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb197.h"

static uint32_t code_hash(const volatile uint32_t *code)
{
    uint32_t hash=2166136261u;
    for(unsigned stage=0;stage<2;++stage) {
        unsigned start=(stage*4096+0x30)/4;
        unsigned count=(80+(stage?TG_GPU_DRAW_FS_CODE_BYTES:TG_GPU_DRAW_VS_CODE_BYTES))/4;
        for(unsigned i=0;i<count;++i) hash=(hash^code[start+i])*16777619u;
    }
    return hash;
}
bool tg_gpu_prepare_draw_code(void *cpu,size_t bytes,struct tg_gpu_draw_status *s)
{
    if(!cpu || !s || bytes<TG_GPU_DRAW_BYTES || ((uintptr_t)cpu&4095)) return false;
    uint32_t *code=cpu;
    for(unsigned i=0;i<TG_GPU_DRAW_BYTES/4;++i)code[i]=0;
    for(unsigned i=0;i<sizeof(tg_gpu_draw_vs)/4;++i)code[0x30/4+i]=tg_gpu_draw_vs[i];
    for(unsigned i=0;i<sizeof(tg_gpu_draw_fs)/4;++i)code[(4096+0x30)/4+i]=tg_gpu_draw_fs[i];
    *s=(struct tg_gpu_draw_status){.requested=1,.sm=53,
        .vs_bytes=TG_GPU_DRAW_VS_CODE_BYTES,.fs_bytes=TG_GPU_DRAW_FS_CODE_BYTES,
        .vs_gprs=TG_GPU_DRAW_VS_GPRS,.fs_gprs=TG_GPU_DRAW_FS_GPRS,.code_hash=code_hash(code)};
    return s->code_hash==TG_GPU_DRAW_CODE_HASH;
}
bool tg_gpu_check_draw_code(const volatile uint32_t *code,size_t bytes)
{ return code && bytes>=TG_GPU_DRAW_BYTES && code_hash(code)==TG_GPU_DRAW_CODE_HASH; }

static void method(uint32_t *p,unsigned *n,uint32_t address,uint32_t value)
{
    p[(*n)++]=NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE)|
        NVVAL(NVB06F,DMA_INCR,COUNT,1)|NVVAL(NVB06F,DMA_INCR,ADDRESS,address>>2);
    p[(*n)++]=value;
}
#define M(a,v) method(p,&n,NVB197_##a,(v))
bool tg_gpu_build_draw_packet(uint32_t *p,size_t capacity,uint64_t code_va,
    uint64_t report_va,uint32_t value,uint32_t *words)
{
    if(!p || !words || capacity<TG_GPU_DRAW_MAX_WORDS || !code_va || (code_va&4095) ||
       code_va>TG_GPU_VA_LIMIT-TG_GPU_DRAW_BYTES || (report_va&3) || report_va>=TG_GPU_VA_LIMIT ||
       (report_va>=code_va && report_va<code_va+TG_GPU_DRAW_BYTES)) return false;
    unsigned n=0;
    M(WAIT_FOR_IDLE,0);
    M(INVALIDATE_SHADER_CACHES,0x1011);
    M(SET_PROGRAM_REGION_A,(uint32_t)(code_va>>32));
    M(SET_PROGRAM_REGION_B,(uint32_t)code_va);
    for(unsigned stage=0;stage<6;++stage) {
        bool active=stage==1||stage==5;
        M(SET_PIPELINE_SHADER(stage),(stage<<4)|(active?1u:0u));
        if(active) {
            M(SET_PIPELINE_PROGRAM(stage),stage==1?0x30:0x1030);
            M(SET_PIPELINE_REGISTER_COUNT(stage),stage==1?TG_GPU_DRAW_VS_GPRS:TG_GPU_DRAW_FS_GPRS);
            M(SET_PIPELINE_BINDING(stage),stage==1?0:4);
        }
    }
    M(SET_SHADER_LOCAL_MEMORY_WINDOW,0xff000000u);
    /* 0x1528 is a host software method, not a GM20B hardware state write.
     * NVK emits it only for Horizon. The Windows cold diagnostic retains
     * its golden error configuration; it never suppresses shader faults.
     */
    M(SET_RASTER_ENABLE,1);
    M(SET_STREAM_OUTPUT,0);
    M(SET_USER_CLIP_ENABLE,0);
    M(SET_USER_CLIP_OP,0);
    M(SET_VIEWPORT_SCALE_OFFSET,1);
    M(SET_VIEWPORT_PIXEL,0); /* Half-integer centers. */
    M(SET_WINDOW_ORIGIN,0); /* Upper left; no Y flip. */
    M(SET_WINDOW_OFFSET_X,0);M(SET_WINDOW_OFFSET_Y,0);
    M(SET_VIEWPORT_SCALE_X(0),0x41000000u); /* 8 */
    M(SET_VIEWPORT_SCALE_Y(0),0x41000000u);
    M(SET_VIEWPORT_SCALE_Z(0),0x3f800000u);
    M(SET_VIEWPORT_OFFSET_X(0),0x41000000u);
    M(SET_VIEWPORT_OFFSET_Y(0),0x41000000u);
    M(SET_VIEWPORT_OFFSET_Z(0),0);
    M(SET_VIEWPORT_CLIP_HORIZONTAL(0),16u<<16);
    M(SET_VIEWPORT_CLIP_VERTICAL(0),16u<<16);
    M(SET_VIEWPORT_CLIP_MIN_Z(0),0);
    M(SET_VIEWPORT_CLIP_MAX_Z(0),0x3f800000u);
    M(SET_VIEWPORT_COORDINATE_SWIZZLE(0),
        NVDEF(NVB197,SET_VIEWPORT_COORDINATE_SWIZZLE,X,POS_X)|
        NVDEF(NVB197,SET_VIEWPORT_COORDINATE_SWIZZLE,Y,POS_Y)|
        NVDEF(NVB197,SET_VIEWPORT_COORDINATE_SWIZZLE,Z,POS_Z)|
        NVDEF(NVB197,SET_VIEWPORT_COORDINATE_SWIZZLE,W,POS_W));
    M(SET_VIEWPORT_Z_CLIP,1);
    M(SET_VIEWPORT_CLIP_CONTROL,0x1819); /* XYZ geometry clip, [0,1] depth clamp. */
    for(unsigned i=0;i<16;++i)M(SET_SCISSOR_ENABLE(i),i==0?1:0);
    M(SET_SCISSOR_HORIZONTAL(0),4u|(12u<<16));
    M(SET_SCISSOR_VERTICAL(0),4u|(12u<<16));
    M(SET_FRONT_POLYGON_MODE,0x1b02);M(SET_BACK_POLYGON_MODE,0x1b02);
    M(OGL_SET_CULL,0);
    M(SET_DEPTH_TEST,0);M(SET_DEPTH_WRITE,0);M(SET_STENCIL_TEST,0);M(SET_ALPHA_TEST,0);
    M(SET_LOGIC_OP,0);M(SET_BLEND_STATE_PER_TARGET,1);M(SET_BLEND(0),0);
    M(SET_CT_MRT_ENABLE,1);M(SET_SINGLE_CT_WRITE_CONTROL,1);M(SET_CT_WRITE(0),0x1111);
    M(SET_ANTI_ALIAS_ENABLE,1);M(SET_ANTI_ALIAS,0);
    M(SET_PS_OUTPUT_SAMPLE_MASK_USAGE,0);M(SET_API_MANDATED_EARLY_Z,0);
    M(SET_POST_Z_PS_IMASK,0);M(SET_POST_PS_INITIAL_COVERAGE,1);
    M(SET_PIXEL_SHADER_INTERLOCK_CONTROL,0);M(SET_ZCULL,0);
    M(SET_SUBTILING_PERF_KNOB_A,0x087f6080);M(SET_SUBTILING_PERF_KNOB_B,0x20);
    for(unsigned a=0;a<32;++a)
        M(SET_VERTEX_ATTRIBUTE_A(a),
            NVDEF(NVB197,SET_VERTEX_ATTRIBUTE_A,SOURCE,INACTIVE)|
            NVDEF(NVB197,SET_VERTEX_ATTRIBUTE_A,COMPONENT_BIT_WIDTHS,R32_G32_B32_A32)|
            NVDEF(NVB197,SET_VERTEX_ATTRIBUTE_A,NUMERICAL_TYPE,NUM_FLOAT));
    M(SET_GLOBAL_BASE_VERTEX_INDEX,0);M(SET_VERTEX_ID_BASE,0);M(SET_GLOBAL_BASE_INSTANCE_INDEX,0);
    M(SET_DA_PRIMITIVE_RESTART,0);M(SET_DA_PRIMITIVE_RESTART_VERTEX_ARRAY,0);
    M(SET_PRIMITIVE_TOPOLOGY_CONTROL,1);
    M(SET_PRIMITIVE_TOPOLOGY,4);
    M(BEGIN,4);M(SET_VERTEX_ARRAY_START,0);M(DRAW_VERTEX_ARRAY,3);M(END,0);
    M(WAIT_FOR_IDLE,0);
    if(!tg_gpu_build_report(p+n,capacity-n,report_va,value))return false;
    n+=TG_GPU_REPORT_WORDS;*words=n;
    return n<=TG_GPU_DRAW_MAX_WORDS;
}
#undef M
