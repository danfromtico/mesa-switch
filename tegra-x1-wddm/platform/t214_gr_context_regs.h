/* SPDX-License-Identifier: MIT
 * Copyright (c) 2014-2020 NVIDIA CORPORATION. All rights reserved.
 * Selected unchanged GM20B definitions from Switchroot nvgpu 1ae0167d
 * include/nvgpu/hw/gm20b/hw_{gr,ctxsw_prog,top,pri_ringmaster,pri_ringstation_sys,pri_ringstation_gpc}_gm20b.h.
 * Type spelling/whitespace adjusted for this build. See ../NOTICE.
 */
#ifndef TG_GPU_T214_GR_CONTEXT_REGS_H
#define TG_GPU_T214_GR_CONTEXT_REGS_H
#include <stdint.h>
static inline uint32_t ctxsw_prog_main_image_context_id_o(void) { return 0x000000f0U; }
static inline uint32_t ctxsw_prog_main_image_num_save_ops_o(void) { return 0x000000f4U; }
static inline uint32_t ctxsw_prog_main_image_num_restore_ops_o(void) { return 0x000000f8U; }
static inline uint32_t ctxsw_prog_main_image_patch_count_o(void) { return 0x00000010U; }
static inline uint32_t ctxsw_prog_main_image_patch_adr_lo_o(void) { return 0x00000014U; }
static inline uint32_t ctxsw_prog_main_image_patch_adr_hi_o(void) { return 0x00000018U; }
static inline uint32_t ctxsw_prog_main_image_priv_access_map_config_o(void) { return 0x000000a0U; }
static inline uint32_t ctxsw_prog_main_image_priv_access_map_addr_lo_o(void) { return 0x000000a4U; }
static inline uint32_t ctxsw_prog_main_image_priv_access_map_addr_hi_o(void) { return 0x000000a8U; }
static inline uint32_t ctxsw_prog_main_image_misc_options_o(void) { return 0x0000003cU; }
static inline uint32_t ctxsw_prog_main_image_ctl_o(void) { return 0x0000000cU; }
static inline uint32_t ctxsw_prog_main_image_pm_o(void) { return 0x00000028U; }
static inline uint32_t ctxsw_prog_main_image_pm_ptr_o(void) { return 0x0000002cU; }
static inline uint32_t ctxsw_prog_main_image_context_timestamp_buffer_control_o(void) { return 0x000000acU; }
static inline uint32_t ctxsw_prog_main_image_context_timestamp_buffer_ptr_hi_o(void) { return 0x000000b0U; }
static inline uint32_t ctxsw_prog_main_image_context_timestamp_buffer_ptr_o(void) { return 0x000000b4U; }
static inline uint32_t ctxsw_prog_main_image_preemption_options_o(void) { return 0x00000060U; }
static inline uint32_t ctxsw_prog_main_image_priv_access_map_config_mode_use_map_f(void) { return 0x2U; }
static inline uint32_t ctxsw_prog_main_image_misc_options_verif_features_m(void) { return 0x1U << 3U; }
static inline uint32_t ctxsw_prog_main_image_ctl_cde_enabled_f(void) { return 0x400U; }
static inline uint32_t ctxsw_prog_main_image_pm_mode_m(void) { return 0x7U << 0U; }
static inline uint32_t ctxsw_prog_main_image_pm_smpc_mode_m(void) { return 0x7U << 3U; }
static inline uint32_t ctxsw_prog_main_image_pm_pc_sampling_m(void) { return 0x1U << 6U; }
static inline uint32_t ctxsw_prog_main_image_magic_value_o(void) { return 0x000000fcU; }
static inline uint32_t ctxsw_prog_main_image_magic_value_v_value_v(void) { return 0x600dc0deU; }
static inline uint32_t ctxsw_prog_main_image_zcull_mode_no_ctxsw_v(void) { return 0x00000001U; }
static inline uint32_t ctxsw_prog_main_image_zcull_o(void) { return 0x0000001cU; }
static inline uint32_t ctxsw_prog_main_image_zcull_ptr_o(void) { return 0x00000020U; }
static inline uint32_t gr_be0_crop_debug3_r(void) { return 0x00410108U; }
static inline uint32_t gr_bes_crop_debug3_comp_vdc_4to2_disable_m(void) { return 0x1U << 31U; }
static inline uint32_t gr_bes_crop_debug3_r(void) { return 0x00408908U; }
static inline uint32_t gr_bes_crop_settings_num_active_ltcs_f(uint32_t v) { return (v & 0xfU) << 0U; }
static inline uint32_t gr_bes_crop_settings_r(void) { return 0x00408958U; }
static inline uint32_t gr_bes_zrop_settings_num_active_ltcs_f(uint32_t v) { return (v & 0xfU) << 0U; }
static inline uint32_t gr_bes_zrop_settings_r(void) { return 0x00408850U; }
static inline uint32_t gr_crstr_gpc_map0_r(void) { return 0x00418b08U; }
static inline uint32_t gr_crstr_map_table_cfg_num_entries_f(uint32_t v) { return (v & 0xffU) << 8U; }
static inline uint32_t gr_crstr_map_table_cfg_r(void) { return 0x00418bb8U; }
static inline uint32_t gr_crstr_map_table_cfg_row_offset_f(uint32_t v) { return (v & 0xffU) << 0U; }
static inline uint32_t gr_cwd_fs_num_gpcs_f(uint32_t v) { return (v & 0xffU) << 0U; }
static inline uint32_t gr_cwd_fs_num_tpcs_f(uint32_t v) { return (v & 0xffU) << 8U; }
static inline uint32_t gr_cwd_fs_r(void) { return 0x00405b00U; }
static inline uint32_t gr_cwd_gpc_tpc_id_r(uint32_t i) { return 0x00405b60U + i*4U; }
static inline uint32_t gr_cwd_gpc_tpc_id_tpc0_f(uint32_t v) { return (v & 0xfU) << 0U; }
static inline uint32_t gr_cwd_sm_id__size_1_v(void) { return 0x00000006U; }
static inline uint32_t gr_cwd_sm_id_r(uint32_t i) { return 0x00405ba0U + i*4U; }
static inline uint32_t gr_ds_debug_r(void) { return 0x00405800U; }
static inline uint32_t gr_ds_debug_timeslice_mode_enable_f(void) { return 0x8000000U; }
static inline uint32_t gr_ds_num_tpc_per_gpc_r(uint32_t i) { return 0x00405870U + i*4U; }
static inline uint32_t gr_ds_tga_constraintlogic_alpha_cbsize_f(uint32_t v) { return (v & 0xffffU) << 0U; }
static inline uint32_t gr_ds_tga_constraintlogic_beta_cbsize_f(uint32_t v) { return (v & 0xffffU) << 16U; }
static inline uint32_t gr_ds_tga_constraintlogic_r(void) { return 0x00405830U; }
static inline uint32_t gr_fe_go_idle_timeout_r(void) { return 0x00404154U; }
static inline uint32_t gr_fe_pwr_mode_r(void) { return 0x00404170U; }
static inline uint32_t gr_fe_tpc_fs_r(void) { return 0x004041c4U; }
static inline uint32_t gr_fecs_ctxsw_mailbox_clear_r(uint32_t i) { return 0x00409840U + i*4U; }
static inline uint32_t gr_fecs_ctxsw_mailbox_r(uint32_t i) { return 0x00409800U + i*4U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_be_context_reset_disabled_f(void) { return 0x400U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_be_engine_reset_disabled_f(void) { return 0x40U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_be_halt_disabled_f(void) { return 0x0U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_gpc_context_reset_disabled_f(void) { return 0x200U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_gpc_engine_reset_disabled_f(void) { return 0x20U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_gpc_halt_disabled_f(void) { return 0x0U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_r(void) { return 0x00409614U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_sys_context_reset_disabled_f(void) { return 0x100U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_sys_engine_reset_disabled_f(void) { return 0x10U; }
static inline uint32_t gr_fecs_ctxsw_reset_ctl_sys_halt_disabled_f(void) { return 0x0U; }
static inline uint32_t gr_fecs_current_ctx_r(void) { return 0x00409b00U; }
static inline uint32_t gr_fecs_method_data_r(void) { return 0x00409500U; }
static inline uint32_t gr_fecs_method_push_r(void) { return 0x00409504U; }
static inline uint32_t gr_gpc0_fs_gpc_num_available_tpcs_v(uint32_t r) { return (r >> 0U) & 0x1fU; }
static inline uint32_t gr_gpc0_fs_gpc_num_available_zculls_v(uint32_t r) { return (r >> 16U) & 0x1fU; }
static inline uint32_t gr_gpc0_fs_gpc_r(void) { return 0x00502608U; }
static inline uint32_t gr_gpc0_gpm_pd_pes_tpc_id_mask_r(uint32_t i) { return 0x00500c30U + i*4U; }
static inline uint32_t gr_gpc0_gpm_pd_sm_id_r(uint32_t i) { return 0x00500c10U + i*4U; }
static inline uint32_t gr_gpc0_ppc0_cbm_alpha_cb_offset_r(void) { return 0x005030f8U; }
static inline uint32_t gr_gpc0_ppc0_cbm_alpha_cb_size_r(void) { return 0x005030e4U; }
static inline uint32_t gr_gpc0_ppc0_cbm_beta_cb_offset_r(void) { return 0x005030f4U; }
static inline uint32_t gr_gpc0_ppc0_cbm_beta_cb_size_r(void) { return 0x005030c0U; }
static inline uint32_t gr_gpc0_ppc0_pes_vsc_strem_master_pe_true_f(void) { return 0x1U; }
static inline uint32_t gr_gpc0_ppc0_pes_vsc_strem_r(void) { return 0x00503018U; }
static inline uint32_t gr_gpc0_tpc0_pe_cfg_smid_r(void) { return 0x00504088U; }
static inline uint32_t gr_gpc0_tpc0_sm_cfg_r(void) { return 0x00504698U; }
static inline uint32_t gr_gpc0_zcull_fs_num_active_banks_f(uint32_t v) { return (v & 0xfU) << 16U; }
static inline uint32_t gr_gpc0_zcull_fs_num_sms_f(uint32_t v) { return (v & 0x1ffU) << 0U; }
static inline uint32_t gr_gpc0_zcull_fs_r(void) { return 0x00500910U; }
static inline uint32_t gr_gpc0_zcull_ram_addr_r(void) { return 0x00500914U; }
static inline uint32_t gr_gpc0_zcull_ram_addr_row_offset_f(uint32_t v) { return (v & 0xfU) << 8U; }
static inline uint32_t gr_gpc0_zcull_ram_addr_tiles_per_hypertile_row_per_gpc_f(uint32_t v) { return (v & 0xfU) << 0U; }
static inline uint32_t gr_gpc0_zcull_sm_num_rcp_conservative__max_v(void) { return 0x00800000U; }
static inline uint32_t gr_gpc0_zcull_sm_num_rcp_conservative_f(uint32_t v) { return (v & 0xffffffU) << 0U; }
static inline uint32_t gr_gpc0_zcull_sm_num_rcp_r(void) { return 0x00500918U; }
static inline uint32_t gr_gpcs_gcc_pagepool_base_r(void) { return 0x00419004U; }
static inline uint32_t gr_gpcs_gcc_pagepool_r(void) { return 0x00419008U; }
static inline uint32_t gr_gpcs_gpm_pd_cfg_r(void) { return 0x00418c6cU; }
static inline uint32_t gr_gpcs_gpm_pd_cfg_timeslice_mode_enable_f(void) { return 0x1U; }
static inline uint32_t gr_gpcs_ppcs_wwdx_sm_num_rcp_conservative_f(uint32_t v) { return (v & 0xffffffU) << 0U; }
static inline uint32_t gr_gpcs_ppcs_wwdx_sm_num_rcp_r(void) { return 0x0041bfd4U; }
static inline uint32_t gr_gpcs_setup_attrib_cb_base_r(void) { return 0x00418810U; }
static inline uint32_t gr_gpcs_swdx_bundle_cb_base_r(void) { return 0x00418e24U; }
static inline uint32_t gr_gpcs_swdx_bundle_cb_size_r(void) { return 0x00418e28U; }
static inline uint32_t gr_gpcs_swdx_rm_pagepool_r(void) { return 0x00418e30U; }
static inline uint32_t gr_gpcs_swdx_tc_beta_cb_size_div3_f(uint32_t v) { return (v & 0xffffU) << 16U; }
static inline uint32_t gr_gpcs_swdx_tc_beta_cb_size_r(uint32_t i) { return 0x00418ea0U + i*4U; }
static inline uint32_t gr_gpcs_swdx_tc_beta_cb_size_v_f(uint32_t v) { return (v & 0xffffU) << 0U; }
static inline uint32_t gr_gpcs_tpcs_mpc_vtg_cb_global_base_addr_r(void) { return 0x00419c2cU; }
static inline uint32_t gr_gpcs_tpcs_mpc_vtg_debug_r(void) { return 0x00419c00U; }
static inline uint32_t gr_gpcs_tpcs_mpc_vtg_debug_timeslice_mode_enabled_f(void) { return 0x8U; }
static inline uint32_t gr_gpcs_tpcs_pe_pin_cb_global_base_addr_r(void) { return 0x00419848U; }
static inline uint32_t gr_gpcs_tpcs_pe_vaf_fast_mode_switch_true_f(void) { return 0x10U; }
static inline uint32_t gr_gpcs_tpcs_pe_vaf_r(void) { return 0x0041980cU; }
static inline uint32_t gr_gpcs_tpcs_pes_vsc_vpc_fast_mode_switch_true_f(void) { return 0x4U; }
static inline uint32_t gr_gpcs_tpcs_pes_vsc_vpc_r(void) { return 0x0041be08U; }
static inline uint32_t gr_gpcs_tpcs_tex_m_dbg2_lg_rd_coalesce_en_m(void) { return 0x1U << 2U; }
static inline uint32_t gr_gpcs_tpcs_tex_m_dbg2_r(void) { return 0x00419a3cU; }
static inline uint32_t gr_gpcs_tpcs_tex_m_dbg2_su_rd_coalesce_en_m(void) { return 0x1U << 4U; }
static inline uint32_t gr_gpcs_zcull_sm_in_gpc_number_map0_r(void) { return 0x00418980U; }
static inline uint32_t gr_gpcs_zcull_sm_in_gpc_number_map0_tile_1_f(uint32_t v) { return (v & 0x7U) << 4U; }
static inline uint32_t gr_pd_ab_dist_cfg0_r(void) { return 0x004064c0U; }
static inline uint32_t gr_pd_ab_dist_cfg0_timeslice_enable_en_f(void) { return 0x80000000U; }
static inline uint32_t gr_pd_ab_dist_cfg1_max_batches_init_f(void) { return 0xffffU; }
static inline uint32_t gr_pd_ab_dist_cfg1_max_output_f(uint32_t v) { return (v & 0xffffU) << 16U; }
static inline uint32_t gr_pd_ab_dist_cfg1_r(void) { return 0x004064c4U; }
static inline uint32_t gr_pd_ab_dist_cfg2_r(void) { return 0x004064c8U; }
static inline uint32_t gr_pd_ab_dist_cfg2_state_limit_f(uint32_t v) { return (v & 0xfffU) << 16U; }
static inline uint32_t gr_pd_ab_dist_cfg2_token_limit_f(uint32_t v) { return (v & 0xfffU) << 0U; }
static inline uint32_t gr_pd_dist_skip_table__size_1_v(void) { return 0x00000008U; }
static inline uint32_t gr_pd_dist_skip_table_r(uint32_t i) { return 0x004064d0U + i*4U; }
static inline uint32_t gr_pd_num_tpc_per_gpc__size_1_v(void) { return 0x00000004U; }
static inline uint32_t gr_pd_num_tpc_per_gpc_r(uint32_t i) { return 0x00406028U + i*4U; }
static inline uint32_t gr_pd_pagepool_r(void) { return 0x004064ccU; }
static inline uint32_t gr_pipe_bundle_address_r(void) { return 0x00400200U; }
static inline uint32_t gr_pipe_bundle_address_value_v(uint32_t r) { return (r >> 0U) & 0xffffU; }
static inline uint32_t gr_pipe_bundle_config_r(void) { return 0x00400208U; }
static inline uint32_t gr_pipe_bundle_data_r(void) { return 0x00400204U; }
static inline uint32_t gr_ppcs_wwdx_map_gpc_map0_r(void) { return 0x0041bf00U; }
static inline uint32_t gr_ppcs_wwdx_map_table_cfg2_r(void) { return 0x0041bfe4U; }
static inline uint32_t gr_ppcs_wwdx_map_table_cfg_normalized_num_entries_f(uint32_t v) { return (v & 0x1fU) << 16U; }
static inline uint32_t gr_ppcs_wwdx_map_table_cfg_normalized_shift_value_f(uint32_t v) { return (v & 0x7U) << 21U; }
static inline uint32_t gr_ppcs_wwdx_map_table_cfg_num_entries_f(uint32_t v) { return (v & 0xffU) << 8U; }
static inline uint32_t gr_ppcs_wwdx_map_table_cfg_r(void) { return 0x0041bfd0U; }
static inline uint32_t gr_ppcs_wwdx_map_table_cfg_row_offset_f(uint32_t v) { return (v & 0xffU) << 0U; }
static inline uint32_t gr_pri_mme_shadow_raw_data_r(void) { return 0x0040448cU; }
static inline uint32_t gr_pri_mme_shadow_raw_index_r(void) { return 0x00404488U; }
static inline uint32_t gr_rstr2d_gpc_map0_r(void) { return 0x0040780cU; }
static inline uint32_t gr_rstr2d_map_table_cfg_num_entries_f(uint32_t v) { return (v & 0xffU) << 8U; }
static inline uint32_t gr_rstr2d_map_table_cfg_r(void) { return 0x004078bcU; }
static inline uint32_t gr_rstr2d_map_table_cfg_row_offset_f(uint32_t v) { return (v & 0xffU) << 0U; }
static inline uint32_t gr_scc_bundle_cb_base_r(void) { return 0x00408004U; }
static inline uint32_t gr_scc_bundle_cb_size_r(void) { return 0x00408008U; }
static inline uint32_t gr_scc_init_r(void) { return 0x0040802cU; }
static inline uint32_t gr_scc_pagepool_base_r(void) { return 0x0040800cU; }
static inline uint32_t gr_scc_pagepool_r(void) { return 0x00408010U; }
static inline uint32_t gr_scc_pagepool_total_pages_hwmax_v(void) { return 0x00000000U; }
static inline uint32_t gr_status_r(void) { return 0x00400700U; }
static inline uint32_t pri_ringmaster_enum_gpc_r(void) { return 0x00120078U; }
static inline uint32_t pri_ringstation_gpc_master_config_r(uint32_t i) { return 0x00128300U + i*4U; }
static inline uint32_t pri_ringstation_sys_master_config_r(uint32_t i) { return 0x00122300U + i*4U; }
static inline uint32_t top_num_gpcs_r(void) { return 0x00022430U; }
static inline uint32_t top_tpc_per_gpc_r(void) { return 0x00022434U; }
#endif
