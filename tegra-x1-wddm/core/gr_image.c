/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2020 NVIDIA CORPORATION. All rights reserved.
 * Copyright 2026 ruje0504-2
 * Image fields/whitelist follow Switchroot nvgpu 1ae0167d:
 * gr_gk20a_load_golden_ctx_image, gr_gk20a_init_access_map,
 * gr_gm20b_get_access_map and hw_ctxsw_prog_gm20b.h. See ../NOTICE.
 */
#include "gr_image.h"
#include "gm20b_mmu.h"
#include "../platform/t214_gr_context_regs.h"

static uint32_t read_le32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void write_le32(uint8_t *p,uint32_t value)
{ for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(value>>(8*i)); }
static bool span(uint64_t address,size_t bytes)
{
    return address && !(address&4095) && address<TG_GPU_VA_LIMIT &&
        bytes && bytes<=TG_GPU_VA_LIMIT-address;
}
static bool separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
enum tg_gpu_gr_image_result tg_gpu_gr_image_build(void *output,size_t capacity,
    const void *golden,size_t bytes,const struct tg_gpu_gr_image_config *config)
{
    if(!output || !golden || !config || capacity<bytes ||
       bytes<TG_GPU_GR_IMAGE_HEADER_BYTES || bytes>TG_GPU_GR_IMAGE_MAX_BYTES ||
       (bytes&3) || !separate(output,bytes,golden,bytes)) return TG_GPU_GR_IMAGE_ARGUMENT;
    const uint8_t *source=golden;
    if(read_le32(source+ctxsw_prog_main_image_magic_value_o())!=
        ctxsw_prog_main_image_magic_value_v_value_v()) return TG_GPU_GR_IMAGE_FORMAT;
    /* Preserve all 40 GPU-VA bits, including addresses above 4 GiB. These
     * are virtual addresses, not Tegra's physical/SMMU selector encodings.
     */
    struct tg_gpu_gr_image_config c=*config;
    if(!span(c.access_map_va,TG_GPU_GR_ACCESS_MAP_BYTES) ||
       ((c.patch_va || c.patch_bytes || c.patch_count) &&
        (!span(c.patch_va,c.patch_bytes) || (uint64_t)c.patch_count*8>c.patch_bytes)))
        return TG_GPU_GR_IMAGE_ADDRESS;
    if(c.patch_va && c.patch_va<c.access_map_va+TG_GPU_GR_ACCESS_MAP_BYTES &&
       c.access_map_va<c.patch_va+c.patch_bytes) return TG_GPU_GR_IMAGE_ADDRESS;
    uint8_t *p=output;
    for(size_t i=0;i<bytes;++i) p[i]=source[i];
#define FIELD(name,value) write_le32(p+ctxsw_prog_main_image_##name##_o(),value)
    FIELD(context_id,c.context_id);
    FIELD(num_save_ops,0);FIELD(num_restore_ops,0);
    FIELD(patch_count,c.patch_count);
    FIELD(patch_adr_lo,(uint32_t)c.patch_va);FIELD(patch_adr_hi,(uint32_t)(c.patch_va>>32));
    FIELD(priv_access_map_config,ctxsw_prog_main_image_priv_access_map_config_mode_use_map_f());
    FIELD(priv_access_map_addr_lo,(uint32_t)c.access_map_va);
    FIELD(priv_access_map_addr_hi,(uint32_t)(c.access_map_va>>32));
    FIELD(misc_options,read_le32(p+ctxsw_prog_main_image_misc_options_o())&
        ~ctxsw_prog_main_image_misc_options_verif_features_m());
    FIELD(ctl,read_le32(p+ctxsw_prog_main_image_ctl_o())&~ctxsw_prog_main_image_ctl_cde_enabled_f());
    FIELD(zcull,ctxsw_prog_main_image_zcull_mode_no_ctxsw_v());FIELD(zcull_ptr,0);
    FIELD(pm,read_le32(p+ctxsw_prog_main_image_pm_o())&
        ~(ctxsw_prog_main_image_pm_mode_m()|ctxsw_prog_main_image_pm_smpc_mode_m()|
          ctxsw_prog_main_image_pm_pc_sampling_m()));
    FIELD(pm_ptr,0);
    /* No trace or compute CTA-preemption backing is owned by this image. */
    FIELD(context_timestamp_buffer_control,0);
    FIELD(context_timestamp_buffer_ptr_hi,0);FIELD(context_timestamp_buffer_ptr,0);
    FIELD(preemption_options,0);
#undef FIELD
    return TG_GPU_GR_IMAGE_OK;
}
bool tg_gpu_gr_access_map_build(void *output,size_t capacity)
{
    static const uint32_t whitelist[]={
        0x404468,0x418300,0x418800,0x418e00,0x418e40,0x418e44,
        0x418e48,0x418e4c,0x418e50,0x418e58,0x418e5c,0x418e60,
        0x418e64,0x418e68,0x418e6c,0x418e70,0x418e74,0x418e78,
        0x418e7c,0x418e80,0x418e84,0x418e88,0x418e8c,0x418e90,
        0x418e94,0x419864,0x419a04,0x419a08,0x419e10,0x419f78
    };
    if(!output || capacity<TG_GPU_GR_ACCESS_MAP_BYTES) return false;
    uint8_t *p=output;
    for(size_t i=0;i<TG_GPU_GR_ACCESS_MAP_BYTES;++i) p[i]=0;
    for(unsigned i=0;i<sizeof(whitelist)/sizeof(whitelist[0]);++i) {
        uint32_t bit=whitelist[i]>>2;
        p[bit>>3]|=(uint8_t)(1u<<(bit&7));
    }
    return true;
}
bool tg_gpu_gr_patch_build(void *output,size_t capacity,
    const struct tg_gpu_gr_patch *records,uint32_t count)
{
    if(!output || !records || !count || (uint64_t)count*8>capacity ||
       !separate(output,(size_t)count*8,records,(size_t)count*sizeof(*records))) return false;
    for(uint32_t i=0;i<count;++i)
        if((records[i].address&3) || records[i].address<0x400000 ||
           records[i].address>=0x600000) return false;
    for(uint32_t i=0;i<count;++i) {
        write_le32((uint8_t *)output+(size_t)i*8,records[i].address);
        write_le32((uint8_t *)output+(size_t)i*8+4,records[i].value);
    }
    return true;
}
