/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_PMIC_H
#define TEGRA_GPU_PMIC_H
#include <stdint.h>
/* Kernel-only bridge to SwitchFan's already-open MAX77812 SPB target.
 * No second driver programs I2C5 registers or opens the CPU's connection.
 */
#define TG_GPU_IOCTL_PMIC 0x00222410u
#define TG_GPU_PMIC_QUERY_ADDRESS     0x100u
#define TG_GPU_PMIC_QUERY_PLLP        0x101u
#define TG_GPU_PMIC_QUERY_TEMP        0x102u
#define TG_GPU_PMIC_QUERY_EMC_KHZ     0x103u
#define TG_GPU_PMIC_QUERY_DVFS_POLICY 0x104u
#define TG_GPU_PMIC_REPORT_GPU_KHZ    0x105u
#define TG_GPU_PMIC_REPORT_GPU_UV     0x106u
#define TG_GPU_PMIC_REPORT_GPU_STATUS 0x107u
#define TG_GPU_PMIC_REPORT_ABI        0x108u
#define TG_GPU_PMIC_QUERY_EMC_VOTE    0x109u
#define TG_GPU_PMIC_REPORT_EMC_ACK    0x10au
#define TG_GPU_PMIC_REPORT_EMC_ABI    0x10bu
#define TG_GPU_PMIC_QUERY_EMC_GUARD   0x10cu
#define TG_GPU_PMIC_RELEASE_RAIL      0x10du
#define TG_GPU_PMIC_CLAIM_RAIL        0x10eu
#define TG_GPU_PMIC_REPORT_EMC_FAILURE 0x10fu
#define TG_GPU_LIVE_COUNTER_ABI       0x00020001u
#define TG_GPU_EMC_VOTE_ABI           0x00010001u

/* Cross-instance / cross-reload rail ownership & handoff protocol (V7-01, A05):
 * - QUERY_ADDRESS with address == TG_GPU_RAIL_QUERY_MAGIC returns PmicAddress in
 *   value, TG_GPU_RAIL_STATE_ACK | flags in address, and (claim_seq<<16)|claim_token
 *   in write when serviced by a token-aware SwitchFan provider.
 * - CLAIM_RAIL with non-zero address (1..0xffff) assigns a monotonic 16-bit
 *   claim_token in SwitchFan and returns TG_GPU_RAIL_STATE_ACK | claim_token in address.
 * - RELEASE_RAIL encodes (claim_token<<16) | ((enable<<8)|vout) in value when
 *   talking to a token-aware provider; SwitchFan rejects mismatched tokens.
 * - REPORT_GPU_STATUS with (address & TG_GPU_RAIL_HANDOFF_MASK) == TG_GPU_RAIL_HANDOFF_MAGIC
 *   records/updates unresolved rail handoff ((claim_token<<16)|flags) across KMD unload. */
#define TG_GPU_RAIL_QUERY_MAGIC       0x5241494Cu
#define TG_GPU_RAIL_HANDOFF_MAGIC     0x52484f00u
#define TG_GPU_RAIL_HANDOFF_MASK      0xffffff00u
#define TG_GPU_RAIL_STATE_ACK         0x80000000u
#define TG_GPU_RAIL_STATE_CLAIMED     0x00000001u
#define TG_GPU_RAIL_STATE_HANDOFF     0x00000002u
#define TG_GPU_RAIL_STATE_ISOLATED    0x00000004u
/* Low byte is MAX77812 M1 voltage code (zero = no EMC vote); high 24 bits
 * identify a request. Only the quiescent GPU clock owner acknowledges it. */
#define TG_GPU_EMC_VOTE_CODE(word) ((uint32_t)(word) & 0xffu)
#define TG_GPU_EMC_VOTE_UV(word) (TG_GPU_EMC_VOTE_CODE(word) ? \
    250000u + 5000u * TG_GPU_EMC_VOTE_CODE(word) : 0u)

/* Packed 32-bit DVFS policy word returned by TG_GPU_PMIC_QUERY_DVFS_POLICY:
 *   [3:0]   mode (0 = fixed boot profile, 1 = T210B01 HighOPT NAPLL DVFS)
 *   [15:4]  min_khz / 400 (exact for all GM20B OPPs up to 1638000 kHz)
 *   [27:16] max_khz / 400 (e.g. 1152 -> 460800 kHz, 1920 -> 768000 kHz, 2112 -> 844800 kHz)
 *   [31:28] policy sequence nibble
 */
#define TG_GPU_DVFS_POLICY_PACK(mode, min_khz, max_khz, seq) \
    (((uint32_t)(mode) & 0xfu) | \
     ((((uint32_t)(min_khz) / 400u) & 0xfffu) << 4) | \
     ((((uint32_t)(max_khz) / 400u) & 0xfffu) << 16) | \
     (((uint32_t)(seq) & 0xfu) << 28))
#define TG_GPU_DVFS_POLICY_MODE(v)    ((uint32_t)(v) & 0xfu)
#define TG_GPU_DVFS_POLICY_MIN_KHZ(v) ((((uint32_t)(v) >> 4) & 0xfffu) * 400u)
#define TG_GPU_DVFS_POLICY_MAX_KHZ(v) ((((uint32_t)(v) >> 16) & 0xfffu) * 400u)
#define TG_GPU_DVFS_POLICY_SEQ(v)     (((uint32_t)(v) >> 28) & 0xfu)

/* Packed 32-bit status word written to TG_GPU_PMIC_REPORT_GPU_STATUS:
 *   [0]     dynamic DVFS active
 *   [1]     thermal throttled
 *   [7:2]   current OPP index (0x3f if fixed non-OPP)
 *   [15:8]  last observed temperature in C (0xff if invalid)
 *   [31:16] saturating/wrapping transition counter
 */
#define TG_GPU_DVFS_STATUS_PACK(active, throttled, opp, temp_c, transitions) \
    (((active) ? 1u : 0u) | ((throttled) ? 2u : 0u) | \
     (((uint32_t)(opp) & 0x3fu) << 2) | \
     (((uint32_t)(temp_c) & 0xffu) << 8) | \
     (((uint32_t)(transitions) & 0xffffu) << 16))

struct tg_gpu_pmic_request {
    uint32_t address;
    uint32_t reg;
    uint32_t write;
    uint32_t value;
};
#endif
