/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2018 NVIDIA CORPORATION. All rights reserved.
 * GM20B cold graphics-context initialization adapted from Switchroot nvgpu
 * 1ae0167d gk20a/gr_gk20a.c, gm20b/gr_gm20b.c and
 * common/priv_ring/priv_ring_gm20b.c. See ../NOTICE.
 */
#include "t214_gr_context.h"
#include "t214_gr_context_regs.h"
#include "../core/gm20b_mmu.h"
#include "../core/gr_draw.h"

#define GPU 0x57000000u
static uint32_t rd(const struct tg_gpu_power_io *io, uint32_t r)
{ return io->read32(io->context,GPU+r); }
static void wr(const struct tg_gpu_power_io *io, uint32_t r, uint32_t v)
{ io->write32(io->context,GPU+r,v); (void)rd(io,r); }
/* Method/bundle ports advance on access. Control-register posted-write
 * readbacks must not be applied to these vendor write-only streams. */
static void stream(const struct tg_gpu_power_io *io, uint32_t r, uint32_t v)
{ io->write32(io->context,GPU+r,v); }
static uint32_t le32(const uint8_t *p)
{ return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static bool valid_work(const struct tg_gpu_gr_work *w,size_t bytes)
{
    return w && w->cpu && !((uintptr_t)w->cpu&4095) && !(w->physical&4095) &&
        w->bytes>=bytes && w->physical>=0x80000000 &&
        w->physical<=UINT64_C(0x100000000)-bytes;
}
static bool prepare_work(void *arena, uint64_t physical, size_t bytes,
    const struct tg_gpu_gr_work *w,size_t work_bytes)
{
    uint64_t *pt;
    uint32_t *image;
    if(!arena || ((uintptr_t)arena&4095) || (physical&4095) || bytes<0x80000 ||
       physical<0x80000000 || physical>UINT64_C(0x100000000)-0x80000 ||
       !valid_work(w,work_bytes) || (physical<w->physical+work_bytes &&
                         w->physical<physical+0x80000)) return false;
    pt=(uint64_t *)((uint8_t *)arena+0x20000);
    /* Validate first; publication cannot leave a partly initialized mapping. */
    for(unsigned i=0;i<work_bytes/4096;++i)
        if(pt[(TG_GPU_GR_WORK_VA>>12)+i]) return false;
    for(unsigned i=0;i<work_bytes/4;++i) ((uint32_t *)w->cpu)[i]=0;
    image=(uint32_t *)((uint8_t *)w->cpu+TG_GPU_GR_GOLD_OFFSET);
    for(unsigned i=0;i<TG_GPU_GR_CTX_CAPACITY/4;++i) image[i]=TG_GPU_GR_GOLD_SENTINEL;
    for(unsigned i=0;i<work_bytes/4096;++i) {
        unsigned offset=i*4096;
        bool read_only=work_bytes==TG_GPU_GR_CHANNEL_WORK_BYTES &&
            ((offset>=TG_GPU_GR_ACCESS_MAP_OFFSET &&
              offset<TG_GPU_GR_ACCESS_MAP_OFFSET+TG_GPU_GR_ACCESS_MAP_BYTES) ||
             (offset>=TG_GPU_GR_PATCH_OFFSET && offset<TG_GPU_GR_PATCH_OFFSET+TG_GPU_GR_PATCH_BYTES) ||
             (offset>=TG_GPU_DRAW_OFFSET && offset<TG_GPU_DRAW_OFFSET+TG_GPU_DRAW_BYTES));
        if(!tg_gpu_pte_4k(w->physical+(uint64_t)i*4096,read_only,true,
                         &pt[(TG_GPU_GR_WORK_VA>>12)+i])) return false;
    }
    return true;
}
bool tg_gpu_gr_context_prepare(void *arena,uint64_t physical,size_t bytes,
    const struct tg_gpu_gr_work *w)
{ return prepare_work(arena,physical,bytes,w,TG_GPU_GR_WORK_BYTES); }
bool tg_gpu_gr_context_prepare_channel(void *arena,uint64_t physical,size_t bytes,
    const struct tg_gpu_gr_work *w)
{ return prepare_work(arena,physical,bytes,w,TG_GPU_GR_CHANNEL_WORK_BYTES); }
static bool wait_mask(const struct tg_gpu_power_io *io, struct tg_gpu_gr_context_status *s,
    uint32_t r, uint32_t mask, uint32_t value)
{
    s->last_register=r;
    for(unsigned i=0;i<10000;++i) {
        s->last_value=rd(io,r);
        if(s->last_value==UINT32_MAX) {s->result=TG_GPU_GR_CTX_REGISTER;return false;}
        if((s->last_value&mask)==value) return true;
        io->delay_us(io->context,10);
    }
    s->result=TG_GPU_GR_CTX_TIMEOUT;
    return false;
}
static bool idle(const struct tg_gpu_power_io *io, struct tg_gpu_gr_context_status *s)
{
    /* All waits are before scheduling; GR engine zero is the GM20B TOP node. */
    for(unsigned i=0;i<10000;++i) {
        uint32_t status=rd(io,gr_status_r()),engine=rd(io,0x2640),busy=rd(io,0x40060c);
        s->last_register=0x2640;s->last_value=engine;
        if(status==UINT32_MAX || engine==UINT32_MAX || busy==UINT32_MAX) {
            s->result=TG_GPU_GR_CTX_REGISTER;return false;
        }
        if(!(engine&0xe000) || (!(busy&1) && !(engine&0x8000))) return true;
        io->delay_us(io->context,10);
    }
    s->result=TG_GPU_GR_CTX_TIMEOUT;return false;
}
static bool fecs(const struct tg_gpu_power_io *io, struct tg_gpu_gr_context_status *s,
    uint32_t method, uint32_t data, uint32_t ok, uint32_t fail, uint32_t *reply)
{
    wr(io,gr_fecs_ctxsw_mailbox_clear_r(0),ok|fail);
    wr(io,gr_fecs_method_data_r(),data);
    wr(io,gr_fecs_method_push_r(),method);
    s->last_register=gr_fecs_ctxsw_mailbox_r(0);
    for(unsigned i=0;i<10000;++i) {
        *reply=s->last_value=rd(io,s->last_register);
        if(*reply==UINT32_MAX) {s->result=TG_GPU_GR_CTX_REGISTER;return false;}
        if(*reply&fail) {s->result=TG_GPU_GR_CTX_FECS_REJECTED;return false;}
        if(*reply&ok) return true;
        io->delay_us(io->context,10);
    }
    s->result=TG_GPU_GR_CTX_TIMEOUT;return false;
}
static bool streams_valid(const struct tg_gpu_gr_netlist *n)
{
    const unsigned ids[]={TG_GPU_GR_CONTEXT,TG_GPU_GR_BUNDLE,TG_GPU_GR_METHOD};
    for(unsigned i=0;i<3;++i) {
        struct tg_gpu_gr_blob b=n->region[ids[i]];
        unsigned stride=i?8:12;
        if(!b.data || !b.size || b.size%stride) return false;
        for(size_t j=0;j<b.size;j+=stride) {
            uint32_t r=le32(b.data+j);
            if(!i && ((r&3) || r<0x400000 || r>=0x600000)) return false;
            if(i==2 && (r&0x80000000)) return false;
        }
    }
    return true;
}
static void context_stream(const struct tg_gpu_power_io *io, const struct tg_gpu_gr_netlist *n)
{
    struct tg_gpu_gr_blob b=n->region[TG_GPU_GR_CONTEXT];
    /* Real NETB context streams include bundle/method ports as well as plain
     * registers; retain the vendor's write-only behavior for the entire pack. */
    for(size_t j=0;j<b.size;j+=12) stream(io,le32(b.data+j),le32(b.data+j+8));
}
static void method_stream(const struct tg_gpu_power_io *io, const struct tg_gpu_gr_netlist *n)
{
    struct tg_gpu_gr_blob b=n->region[TG_GPU_GR_METHOD];
    uint32_t last=0;
    for(size_t j=0;j<b.size;j+=8) {
        uint32_t value=le32(b.data+j+4);
        if(!j || value!=last) {stream(io,gr_pri_mme_shadow_raw_data_r(),value);last=value;}
        stream(io,gr_pri_mme_shadow_raw_index_r(),0x80000000|le32(b.data+j));
    }
}
bool tg_gpu_gr_context_global_patches(const struct tg_gpu_gr_global_addresses *a,
    struct tg_gpu_gr_patch *output,size_t capacity)
{
    if(!a || !output || capacity<TG_GPU_GR_GLOBAL_PATCH_COUNT ||
       !a->bundle || (a->bundle&255) || a->bundle>TG_GPU_VA_LIMIT-0x1800 ||
       !a->pagepool || (a->pagepool&255) || a->pagepool>TG_GPU_VA_LIMIT-0x8000 ||
       !a->attribute || (a->attribute&4095) || a->attribute>TG_GPU_VA_LIMIT-0x48000)
        return false;
    uint32_t page=(uint32_t)(a->pagepool>>8),bundle=(uint32_t)(a->bundle>>8);
    uint32_t attrib=(uint32_t)(a->attribute>>12);unsigned n=0;
#define PATCH(reg,value) output[n++]=(struct tg_gpu_gr_patch){reg,value}
    PATCH(gr_scc_pagepool_base_r(),page);
    PATCH(gr_scc_pagepool_r(),gr_scc_pagepool_total_pages_hwmax_v()|0x80000000);
    PATCH(gr_gpcs_gcc_pagepool_base_r(),page);
    PATCH(gr_gpcs_gcc_pagepool_r(),gr_scc_pagepool_total_pages_hwmax_v());
    PATCH(gr_pd_pagepool_r(),gr_scc_pagepool_total_pages_hwmax_v()|0x80000000);
    PATCH(gr_gpcs_swdx_rm_pagepool_r(),gr_scc_pagepool_total_pages_hwmax_v()|0x80000000);
    PATCH(gr_scc_bundle_cb_base_r(),bundle);PATCH(gr_scc_bundle_cb_size_r(),0x80000018);
    PATCH(gr_gpcs_swdx_bundle_cb_base_r(),bundle);PATCH(gr_gpcs_swdx_bundle_cb_size_r(),0x80000018);
    PATCH(gr_pd_ab_dist_cfg2_r(),gr_pd_ab_dist_cfg2_token_limit_f(0x1c0)|
          gr_pd_ab_dist_cfg2_state_limit_f(0xc0));
    PATCH(gr_gpcs_setup_attrib_cb_base_r(),attrib|0x80000000);
    PATCH(gr_gpcs_tpcs_pe_pin_cb_global_base_addr_r(),attrib|0x10000000);
    PATCH(gr_gpcs_tpcs_mpc_vtg_cb_global_base_addr_r(),attrib|0x10000000);
    PATCH(gr_ds_tga_constraintlogic_r(),gr_ds_tga_constraintlogic_beta_cbsize_f(0x400)|
          gr_ds_tga_constraintlogic_alpha_cbsize_f(0x800));
    PATCH(gr_pd_ab_dist_cfg1_r(),gr_pd_ab_dist_cfg1_max_output_f(0x800*32/128)|
          gr_pd_ab_dist_cfg1_max_batches_init_f());
    /* Two TPCs in one PES. Maximum backing includes 50% growth for each CB. */
    PATCH(gr_gpc0_ppc0_cbm_beta_cb_size_r(),0x800);
    PATCH(gr_gpc0_ppc0_cbm_beta_cb_offset_r(),0);
    PATCH(gr_gpc0_ppc0_cbm_alpha_cb_size_r(),0x1000);
    PATCH(gr_gpc0_ppc0_cbm_alpha_cb_offset_r(),0xc00);
    PATCH(gr_gpcs_swdx_tc_beta_cb_size_r(0),gr_gpcs_swdx_tc_beta_cb_size_v_f(0x800)|
          gr_gpcs_swdx_tc_beta_cb_size_div3_f(0x800/3));
#undef PATCH
    return n==TG_GPU_GR_GLOBAL_PATCH_COUNT;
}
static bool globals(const struct tg_gpu_power_io *io)
{
    const struct tg_gpu_gr_global_addresses a={TG_GPU_GR_WORK_VA+TG_GPU_GR_BUNDLE_OFFSET,
        TG_GPU_GR_WORK_VA+TG_GPU_GR_PAGEPOOL_OFFSET,TG_GPU_GR_WORK_VA+TG_GPU_GR_ATTRIB_OFFSET};
    struct tg_gpu_gr_patch p[TG_GPU_GR_GLOBAL_PATCH_COUNT];
    if(!tg_gpu_gr_context_global_patches(&a,p,TG_GPU_GR_GLOBAL_PATCH_COUNT)) return false;
    for(unsigned i=0;i<TG_GPU_GR_GLOBAL_PATCH_COUNT;++i) wr(io,p[i].address,p[i].value);
    return true;
}
static void floorsweep(const struct tg_gpu_power_io *io, uint32_t ltcs)
{
    for(unsigned tpc=0;tpc<2;++tpc) {
        wr(io,gr_gpc0_tpc0_sm_cfg_r()+tpc*0x800,tpc);
        wr(io,gr_gpc0_gpm_pd_sm_id_r(tpc),tpc);
        wr(io,gr_gpc0_tpc0_pe_cfg_smid_r()+tpc*0x800,tpc);
    }
    for(unsigned i=0;i<gr_pd_num_tpc_per_gpc__size_1_v();++i) {
        wr(io,gr_pd_num_tpc_per_gpc_r(i),i?0:2);wr(io,gr_ds_num_tpc_per_gpc_r(i),i?0:2);
    }
    /* Every tile belongs to GPC0. One-GPC normalization has zero moduli. */
    wr(io,gr_crstr_map_table_cfg_r(),gr_crstr_map_table_cfg_row_offset_f(1)|
       gr_crstr_map_table_cfg_num_entries_f(2));
    wr(io,gr_ppcs_wwdx_map_table_cfg_r(),gr_ppcs_wwdx_map_table_cfg_row_offset_f(1)|
       gr_ppcs_wwdx_map_table_cfg_normalized_num_entries_f(16)|
       gr_ppcs_wwdx_map_table_cfg_normalized_shift_value_f(3)|
       gr_ppcs_wwdx_map_table_cfg_num_entries_f(2));
    wr(io,gr_ppcs_wwdx_map_table_cfg2_r(),0);
    wr(io,gr_rstr2d_map_table_cfg_r(),gr_rstr2d_map_table_cfg_row_offset_f(1)|
       gr_rstr2d_map_table_cfg_num_entries_f(2));
    for(unsigned i=0;i<6;++i) {
        wr(io,gr_crstr_gpc_map0_r()+i*4,0);wr(io,gr_ppcs_wwdx_map_gpc_map0_r()+i*4,0);
        wr(io,gr_rstr2d_gpc_map0_r()+i*4,0);
    }
    for(unsigned i=0;i<gr_pd_dist_skip_table__size_1_v();++i) wr(io,gr_pd_dist_skip_table_r(i),0);
    wr(io,gr_cwd_fs_r(),gr_cwd_fs_num_gpcs_f(1)|gr_cwd_fs_num_tpcs_f(2));
    wr(io,gr_bes_zrop_settings_r(),gr_bes_zrop_settings_num_active_ltcs_f(ltcs));
    wr(io,gr_bes_crop_settings_r(),gr_bes_crop_settings_num_active_ltcs_f(ltcs));
    wr(io,gr_bes_crop_debug3_r(),rd(io,gr_be0_crop_debug3_r())|gr_bes_crop_debug3_comp_vdc_4to2_disable_m());
    wr(io,gr_fe_tpc_fs_r(),3);
    wr(io,gr_cwd_gpc_tpc_id_r(0),gr_cwd_gpc_tpc_id_tpc0_f(1)<<8);
    for(unsigned i=0;i<gr_cwd_sm_id__size_1_v();++i) wr(io,gr_cwd_sm_id_r(i),i?0:0x100);
}
static void zcull(const struct tg_gpu_power_io *io, uint32_t banks)
{
    wr(io,gr_gpc0_ppc0_pes_vsc_strem_r(),rd(io,gr_gpc0_ppc0_pes_vsc_strem_r())|
       gr_gpc0_ppc0_pes_vsc_strem_master_pe_true_f());
    wr(io,gr_gpcs_zcull_sm_in_gpc_number_map0_r(),gr_gpcs_zcull_sm_in_gpc_number_map0_tile_1_f(1));
    wr(io,gr_gpc0_zcull_ram_addr_r(),gr_gpc0_zcull_ram_addr_row_offset_f(1)|
       gr_gpc0_zcull_ram_addr_tiles_per_hypertile_row_per_gpc_f(2));
    wr(io,gr_gpc0_zcull_fs_r(),gr_gpc0_zcull_fs_num_active_banks_f(banks)|
       gr_gpc0_zcull_fs_num_sms_f(2));
    wr(io,gr_gpc0_zcull_sm_num_rcp_r(),gr_gpc0_zcull_sm_num_rcp_conservative_f(
       (gr_gpc0_zcull_sm_num_rcp_conservative__max_v()+1)/2));
    wr(io,gr_gpcs_ppcs_wwdx_sm_num_rcp_r(),gr_gpcs_ppcs_wwdx_sm_num_rcp_conservative_f(
       (gr_gpc0_zcull_sm_num_rcp_conservative__max_v()+1)/2));
}
/* Switchroot nvgpu 1ae0167d gm20b_priv_set_timeout_settings (called from
 * gk20a_init_gr_setup_hw right after gr_gk20a_zcull_init_hw). Upstream notes
 * hardware reset defaults of 0x400; live readback on physical hardware is
 * separate and not assumed here. Apply only the two 0x800 writes.
 */
static void priv_ring_timeouts(const struct tg_gpu_power_io *io)
{
    wr(io,pri_ringstation_sys_master_config_r(0x15),0x800);
    wr(io,pri_ringstation_gpc_master_config_r(0xa),0x800);
}
/* Switchroot nvgpu 1ae0167d gm20a_gr_disable_rd_coalesce (called from
 * gk20a_init_gr_setup_hw strictly before sw_ctx_load replay). The supplied
 * L4T 32.7.6 NETB region 5 subsequently writes 0x419a3c = 0x181e, so do not
 * clear after replay or during golden capture.
 */
static bool disable_rd_coalesce(const struct tg_gpu_power_io *io,
    struct tg_gpu_gr_context_status *s)
{
    const uint32_t mask=gr_gpcs_tpcs_tex_m_dbg2_lg_rd_coalesce_en_m()|
        gr_gpcs_tpcs_tex_m_dbg2_su_rd_coalesce_en_m();
    const uint32_t value=rd(io,gr_gpcs_tpcs_tex_m_dbg2_r());
    s->last_register=gr_gpcs_tpcs_tex_m_dbg2_r();s->last_value=value;
    if(value==UINT32_MAX || (value>>20)==0xbad) {s->result=TG_GPU_GR_CTX_REGISTER;return false;}
    wr(io,gr_gpcs_tpcs_tex_m_dbg2_r(),value&~mask);
    return true;
}
static void timeslice(const struct tg_gpu_power_io *io)
{
    /* Match the vendor's enabled timeslice mode, retaining other NETB bits. */
    const uint32_t r[]={gr_gpcs_gpm_pd_cfg_r(),gr_gpcs_tpcs_pe_vaf_r(),gr_gpcs_tpcs_pes_vsc_vpc_r(),
        gr_pd_ab_dist_cfg0_r(),gr_ds_debug_r(),gr_gpcs_tpcs_mpc_vtg_debug_r()};
    const uint32_t v[]={gr_gpcs_gpm_pd_cfg_timeslice_mode_enable_f(),gr_gpcs_tpcs_pe_vaf_fast_mode_switch_true_f(),
        gr_gpcs_tpcs_pes_vsc_vpc_fast_mode_switch_true_f(),gr_pd_ab_dist_cfg0_timeslice_enable_en_f(),
        gr_ds_debug_timeslice_mode_enable_f(),gr_gpcs_tpcs_mpc_vtg_debug_timeslice_mode_enabled_f()};
    for(unsigned i=0;i<6;++i) wr(io,r[i],rd(io,r[i])|v[i]);
}
static bool bundles(const struct tg_gpu_power_io *io, const struct tg_gpu_gr_netlist *n,
    struct tg_gpu_gr_context_status *s)
{
    struct tg_gpu_gr_blob b=n->region[TG_GPU_GR_BUNDLE];uint32_t last=0;
    wr(io,gr_pipe_bundle_config_r(),0x80000000);
    for(size_t j=0;j<b.size;j+=8) {
        uint32_t address=le32(b.data+j),value=le32(b.data+j+4);
        if(!j || last!=value) {stream(io,gr_pipe_bundle_data_r(),value);last=value;}
        stream(io,gr_pipe_bundle_address_r(),address);
        if(gr_pipe_bundle_address_value_v(address)==0xe100 && !idle(io,s)) return false;
        if(!wait_mask(io,s,gr_status_r(),4,0)) return false;
    }
    wr(io,gr_pipe_bundle_config_r(),0);
    return idle(io,s);
}
/* gr_pri_gpcs_tpcs_sm_disp_ctrl, the last entry of nvgpu's GM20B user access
 * map (gr_gm20b_get_access_map); nvgpu itself never writes it and defines no
 * accessor. Bit 3 disables memory loads in fragment-shader helper
 * invocations: Linux nouveau 1cb9e2ef66d5, gk110_grctx_generate_r419f78,
 * which also serves GM107. Upstream NVK clears it from user mode through
 * NVK_MME_SET_PRIV_REG; the Windows ICD omits that macro and the command
 * validator rejects it. The supplied NETB context stream sets bit 3 in
 * 0x10beb, so the KMD must establish the enabled policy. NVK reads
 * D3D constant buffers with global loads on this GPU. A helper lane that
 * skips them computes wrong derivatives and can iterate a data-dependent
 * loop forever, which retires no fence and raises no fault.
 *
 * Clear it once, after the netlist streams and before the golden save, so
 * every cloned context inherits it exactly as nouveau's golden image does.
 * A read that is not register data is reported without writing it back. */
#define GR_GPCS_TPCS_SM_DISP_CTRL 0x00419f78u
#define GR_SM_DISP_CTRL_NO_HELPER_LOADS 0x00000008u
static bool helper_loads(const struct tg_gpu_power_io *io, struct tg_gpu_gr_context_status *s)
{
    const uint32_t value=rd(io,GR_GPCS_TPCS_SM_DISP_CTRL);
    s->last_register=GR_GPCS_TPCS_SM_DISP_CTRL;s->last_value=value;
    /* 0xbad..... is the PRI ring's failed-access pattern on this family. */
    if(value==UINT32_MAX || (value>>20)==0xbad) {s->result=TG_GPU_GR_CTX_REGISTER;return false;}
    if(value&GR_SM_DISP_CTRL_NO_HELPER_LOADS) {
        const uint32_t enabled=value&~GR_SM_DISP_CTRL_NO_HELPER_LOADS;
        io->write32(io->context,GPU+GR_GPCS_TPCS_SM_DISP_CTRL,enabled);
        s->last_value=rd(io,GR_GPCS_TPCS_SM_DISP_CTRL);
        if(s->last_value!=enabled) {s->result=TG_GPU_GR_CTX_REGISTER;return false;}
    }
    return true;
}
/* Match NVK's MAXWELL_B shader-object context policy (nvk_cmd_draw.c).
 * Independently compiled stages can write attributes the next stage does
 * not consume, including a null fragment stage. NVK disables this warp
 * exception; its private-register MME is unavailable on Windows. The NETB
 * context stream enables bit 14 in 0x00d3eff2, so apply the same policy to
 * the golden context after those streams. All other exception bits remain.
 */
#define GR_GPCS_TPCS_SM_WARP_ESR_REPORT_MASK 0x00419e44u
#define GR_SM_WARP_OUT_OF_RANGE_ADDR (1u<<14)
static bool shader_output_exceptions(const struct tg_gpu_power_io *io,
    struct tg_gpu_gr_context_status *s)
{
    const uint32_t value=rd(io,GR_GPCS_TPCS_SM_WARP_ESR_REPORT_MASK);
    s->last_register=GR_GPCS_TPCS_SM_WARP_ESR_REPORT_MASK;s->last_value=value;
    if(value==UINT32_MAX || (value>>20)==0xbad) {s->result=TG_GPU_GR_CTX_REGISTER;return false;}
    if(value&GR_SM_WARP_OUT_OF_RANGE_ADDR) {
        const uint32_t enabled=value&~GR_SM_WARP_OUT_OF_RANGE_ADDR;
        io->write32(io->context,GPU+GR_GPCS_TPCS_SM_WARP_ESR_REPORT_MASK,enabled);
        s->last_value=rd(io,GR_GPCS_TPCS_SM_WARP_ESR_REPORT_MASK);
        if(s->last_value!=enabled) {s->result=TG_GPU_GR_CTX_REGISTER;return false;}
    }
    return true;
}
enum tg_gpu_gr_context_result tg_gpu_gr_context_init(
    const struct tg_gpu_power_io *io, const struct tg_gpu_power *power,
    struct tg_gpu_frontend_status *fe, const struct tg_gpu_gr_netlist *n,
    struct tg_gpu_gr_status *gr, uint32_t *inst, uint64_t physical,
    const struct tg_gpu_gr_work *w, struct tg_gpu_gr_context_status *s)
{
    uint32_t cfg,pes,ctxdata;uint32_t *ctx,*gold;
    if(!s) return TG_GPU_GR_CTX_ARGUMENT;
    *s=(struct tg_gpu_gr_context_status){0};
    if(!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
       !power->owned || !power->ready || !fe || fe->result || !fe->ltc_count ||
       !n || !gr || gr->result || gr->stage!=15 || !inst || !valid_work(w,TG_GPU_GR_WORK_BYTES) ||
       (physical&4095) || physical<0x80000000 || physical>0xfffff000 ||
       gr->context_bytes<256 || gr->context_bytes>TG_GPU_GR_CTX_CAPACITY || (gr->context_bytes&3))
        return s->result=TG_GPU_GR_CTX_ARGUMENT;
    if(!streams_valid(n)) return s->result=TG_GPU_GR_CTX_FORMAT;
    s->stage=1;s->context_bytes=gr->context_bytes;
    s->last_register=pri_ringmaster_enum_gpc_r();
    s->last_value=rd(io,s->last_register);s->gpcs=s->last_value&0x1f;
    if(s->gpcs!=1) return s->result=TG_GPU_GR_CTX_TOPOLOGY;
    s->last_register=gr_gpc0_fs_gpc_r();s->last_value=cfg=rd(io,s->last_register);
    s->tpcs=gr_gpc0_fs_gpc_num_available_tpcs_v(cfg);
    s->zculls=gr_gpc0_fs_gpc_num_available_zculls_v(cfg);
    /* GM20B's proj_scal_litter_num_zcull_banks_v() is four, independently
     * of its two TPCs. The physical Lite reports 0x00040002 here. Only this
     * full-bank layout uses the non-floorswept ZCULL setup below. */
    if(s->tpcs!=2 || s->zculls!=4)
        return s->result=TG_GPU_GR_CTX_TOPOLOGY;
    s->last_register=gr_gpc0_gpm_pd_pes_tpc_id_mask_r(0);
    s->last_value=pes=rd(io,s->last_register);
    if(pes!=3) return s->result=TG_GPU_GR_CTX_TOPOLOGY;
    s->last_register=top_num_gpcs_r();s->last_value=rd(io,s->last_register);
    if(s->last_value!=1) return s->result=TG_GPU_GR_CTX_TOPOLOGY;
    s->last_register=top_tpc_per_gpc_r();s->last_value=rd(io,s->last_register);
    if(s->last_value!=2) return s->result=TG_GPU_GR_CTX_TOPOLOGY;
    ctx=(uint32_t *)w->cpu;gold=(uint32_t *)((uint8_t *)w->cpu+TG_GPU_GR_GOLD_OFFSET);
    ctxdata=(uint32_t)(physical>>12)|0x80000000;
    inst[132]=TG_GPU_GR_WORK_VA|4;inst[133]=0;
    if(tg_gpu_frontend_sync_memory(io,power,fe)!=TG_GPU_FE_OK) {
        s->result=TG_GPU_GR_CTX_VISIBILITY;goto done;
    }
    s->stage=2;
    zcull(io,s->zculls);priv_ring_timeouts(io);
    if(!disable_rd_coalesce(io,s)) goto done;
    context_stream(io,n);
    if(!idle(io,s)) goto done;
    wr(io,gr_fe_go_idle_timeout_r(),0);timeslice(io);floorsweep(io,fe->ltc_count);
    wr(io,gr_fe_go_idle_timeout_r(),0x800);method_stream(io,n);
    if(!idle(io,s)) goto done;
    s->stage=3;
    wr(io,gr_fe_pwr_mode_r(),0x12);
    if(!wait_mask(io,s,gr_fe_pwr_mode_r(),0x10,0)) goto done;
    wr(io,gr_fecs_ctxsw_reset_ctl_r(),
       gr_fecs_ctxsw_reset_ctl_sys_halt_disabled_f()|gr_fecs_ctxsw_reset_ctl_gpc_halt_disabled_f()|
       gr_fecs_ctxsw_reset_ctl_be_halt_disabled_f()|gr_fecs_ctxsw_reset_ctl_sys_engine_reset_disabled_f()|
       gr_fecs_ctxsw_reset_ctl_gpc_engine_reset_disabled_f()|gr_fecs_ctxsw_reset_ctl_be_engine_reset_disabled_f());
    io->delay_us(io->context,10);
    wr(io,gr_fecs_ctxsw_reset_ctl_r(),
       gr_fecs_ctxsw_reset_ctl_sys_halt_disabled_f()|gr_fecs_ctxsw_reset_ctl_gpc_halt_disabled_f()|
       gr_fecs_ctxsw_reset_ctl_be_halt_disabled_f()|gr_fecs_ctxsw_reset_ctl_sys_engine_reset_disabled_f()|
       gr_fecs_ctxsw_reset_ctl_gpc_engine_reset_disabled_f()|gr_fecs_ctxsw_reset_ctl_be_engine_reset_disabled_f()|
       gr_fecs_ctxsw_reset_ctl_sys_context_reset_disabled_f()|
       gr_fecs_ctxsw_reset_ctl_gpc_context_reset_disabled_f()|gr_fecs_ctxsw_reset_ctl_be_context_reset_disabled_f());
    io->delay_us(io->context,10);wr(io,gr_fe_pwr_mode_r(),0x10);
    if(!wait_mask(io,s,gr_fe_pwr_mode_r(),0x10,0)) goto done;
    wr(io,gr_scc_init_r(),1);
    s->stage=4;
    if(!fecs(io,s,3,ctxdata,0x10,0x20,&s->bind_mailbox) || !idle(io,s)) goto done;
    s->stage=5;
    context_stream(io,n);
    if(!idle(io,s)) goto done;
    wr(io,gr_fe_go_idle_timeout_r(),0);
    if(!globals(io)) {s->result=TG_GPU_GR_CTX_ARGUMENT;goto done;}
    timeslice(io);floorsweep(io,fe->ltc_count);
    if(!idle(io,s) || !bundles(io,n,s)) goto done;
    wr(io,gr_fe_go_idle_timeout_r(),0x800);
    if(!idle(io,s)) goto done;
    method_stream(io,n);
    if(!idle(io,s)) goto done;
    if(!helper_loads(io,s)) goto done;
    if(!shader_output_exceptions(io,s)) goto done;
    s->stage=6;
    if(tg_gpu_frontend_sync_memory(io,power,fe)!=TG_GPU_FE_OK) {
        s->result=TG_GPU_GR_CTX_VISIBILITY;goto done;
    }
    for(unsigned i=0;i<256/4;++i) gold[i]=ctx[i];
    gold[ctxsw_prog_main_image_zcull_o()/4]=ctxsw_prog_main_image_zcull_mode_no_ctxsw_v();
    gold[ctxsw_prog_main_image_zcull_ptr_o()/4]=0;
    inst[132]=(TG_GPU_GR_WORK_VA+TG_GPU_GR_GOLD_OFFSET)|4;
    if(tg_gpu_frontend_sync_memory(io,power,fe)!=TG_GPU_FE_OK) {
        s->result=TG_GPU_GR_CTX_VISIBILITY;goto done;
    }
    s->stage=7;
    if(!fecs(io,s,9,ctxdata,1,2,&s->save_mailbox)) goto done;
    if(tg_gpu_frontend_sync_memory(io,power,fe)!=TG_GPU_FE_OK) {
        s->result=TG_GPU_GR_CTX_VISIBILITY;goto done;
    }
    s->stage=8;s->image_magic=gold[ctxsw_prog_main_image_magic_value_o()/4];
    s->image_hash=2166136261u;
    for(unsigned i=0;i<s->context_bytes/4;++i) {
        s->image_hash=(s->image_hash^gold[i])*16777619u;
        if(i>=256/4 && gold[i]!=TG_GPU_GR_GOLD_SENTINEL) ++s->image_changed_words;
    }
    if(s->image_magic!=ctxsw_prog_main_image_magic_value_v_value_v() || !s->image_changed_words) {
        s->result=TG_GPU_GR_CTX_IMAGE;goto done;
    }
    /* Restore the original context pointer before admitting the FIFO channel.
     * Clear FECS's manually bound context just as the vendor golden path does.
     */
    inst[132]=TG_GPU_GR_WORK_VA|4;wr(io,gr_fecs_current_ctx_r(),0);
    if(!wait_mask(io,s,gr_fecs_current_ctx_r(),0x80000000,0)) goto done;
    s->stage=9;
done:
    wr(io,gr_pipe_bundle_config_r(),0);wr(io,gr_fe_go_idle_timeout_r(),0x800);
    tg_gpu_gr_snapshot(io,gr);tg_gpu_frontend_snapshot(io,fe);
    if(!s->result && fe->result) s->result=TG_GPU_GR_CTX_PRI_FAULT;
    return (enum tg_gpu_gr_context_result)s->result;
}
enum tg_gpu_gr_context_result tg_gpu_gr_context_load_image(
    const struct tg_gpu_power_io *io,const struct tg_gpu_power *power,
    struct tg_gpu_frontend_status *fe,uint32_t *inst,const struct tg_gpu_gr_work *w,
    const struct tg_gpu_gr_context_status *golden,uint32_t context_id)
{
    if(!io || !io->read32 || !io->write32 || !io->delay_us || !power ||
       !power->owned || !power->ready || !fe || fe->result || !inst ||
       !valid_work(w,TG_GPU_GR_CHANNEL_WORK_BYTES) || !golden || golden->result ||
       golden->stage!=9 || !golden->image_changed_words ||
       golden->context_bytes<TG_GPU_GR_IMAGE_HEADER_BYTES ||
       golden->context_bytes>TG_GPU_GR_CTX_CAPACITY || (golden->context_bytes&3))
        return TG_GPU_GR_CTX_ARGUMENT;
    if(rd(io,gr_fecs_current_ctx_r())!=0) return TG_GPU_GR_CTX_ARGUMENT;
    const struct tg_gpu_gr_global_addresses a={TG_GPU_GR_WORK_VA+TG_GPU_GR_BUNDLE_OFFSET,
        TG_GPU_GR_WORK_VA+TG_GPU_GR_PAGEPOOL_OFFSET,TG_GPU_GR_WORK_VA+TG_GPU_GR_ATTRIB_OFFSET};
    struct tg_gpu_gr_patch p[TG_GPU_GR_GLOBAL_PATCH_COUNT];
    struct tg_gpu_gr_image_config config={TG_GPU_GR_WORK_VA+TG_GPU_GR_ACCESS_MAP_OFFSET,
        TG_GPU_GR_WORK_VA+TG_GPU_GR_PATCH_OFFSET,TG_GPU_GR_PATCH_BYTES,
        TG_GPU_GR_GLOBAL_PATCH_COUNT,context_id};
    if(!tg_gpu_gr_context_global_patches(&a,p,TG_GPU_GR_GLOBAL_PATCH_COUNT))
        return TG_GPU_GR_CTX_ARGUMENT;
    uint8_t *cpu=w->cpu;
    if(tg_gpu_gr_image_build(cpu,TG_GPU_GR_CTX_CAPACITY,cpu+TG_GPU_GR_GOLD_OFFSET,
          golden->context_bytes,&config)!=TG_GPU_GR_IMAGE_OK ||
       !tg_gpu_gr_access_map_build(cpu+TG_GPU_GR_ACCESS_MAP_OFFSET,TG_GPU_GR_ACCESS_MAP_BYTES) ||
       !tg_gpu_gr_patch_build(cpu+TG_GPU_GR_PATCH_OFFSET,TG_GPU_GR_PATCH_BYTES,
                             p,TG_GPU_GR_GLOBAL_PATCH_COUNT)) return TG_GPU_GR_CTX_IMAGE;
    inst[132]=TG_GPU_GR_WORK_VA|4;inst[133]=0;
    if(tg_gpu_frontend_sync_memory(io,power,fe)!=TG_GPU_FE_OK)
        return TG_GPU_GR_CTX_VISIBILITY;
    return TG_GPU_GR_CTX_OK;
}
