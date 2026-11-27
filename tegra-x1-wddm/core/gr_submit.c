/* SPDX-License-Identifier: MIT */
#include "gr_submit.h"
#include "report.h"
#include "gm20b_mmu.h"
#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/clb06f.h"
#include "nouveau/headers/nvidia/classes/clb197.h"

_Static_assert(TG_GPU_GR_ENGINE_CLASS==MAXWELL_B,"GM20B graphics class changed");
_Static_assert(TG_GPU_GR_ENGINE_NEXT_WORDS==2+TG_GPU_REPORT_WORDS,"GR report length");
_Static_assert(TG_GPU_GR_ENGINE_FIRST_WORDS==2+TG_GPU_GR_ENGINE_NEXT_WORDS,"GR bind length");
bool tg_gpu_build_gr_engine_packet(uint32_t *output,size_t capacity,
    uint64_t report_va,uint32_t value,bool bind)
{
    size_t words=bind?TG_GPU_GR_ENGINE_FIRST_WORDS:TG_GPU_GR_ENGINE_NEXT_WORDS;
    if(!output || capacity<words || (report_va&3) || report_va>=TG_GPU_VA_LIMIT)
        return false;
    unsigned n=0;
    if(bind) {
        /* NVK nvk_push_draw_state_init: class ID plus engine ID zero. */
        output[n++]=NVDEF(NVB06F,DMA_INCR,OPCODE,VALUE)|
            NVVAL(NVB06F,DMA_INCR,COUNT,1)|
            NVVAL(NVB06F,DMA_INCR,ADDRESS,NVB197_SET_OBJECT>>2);
        output[n++]=NVVAL(NVB197,SET_OBJECT,CLASS_ID,TG_GPU_GR_ENGINE_CLASS)|
            NVVAL(NVB197,SET_OBJECT,ENGINE_ID,0);
    }
    output[n++]=NVDEF(NVB06F,DMA_IMMD,OPCODE,VALUE)|
        NVVAL(NVB06F,DMA_IMMD,ADDRESS,NVB197_NO_OPERATION>>2);
    output[n++]=NVDEF(NVB06F,DMA_IMMD,OPCODE,VALUE)|
        NVVAL(NVB06F,DMA_IMMD,ADDRESS,NVB197_WAIT_FOR_IDLE>>2);
    return tg_gpu_build_report(output+n,capacity-n,report_va,value);
}
