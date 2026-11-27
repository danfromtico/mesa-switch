/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_REPORT_H
#define TEGRA_GPU_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_GPU_REPORT_WORDS 6u

/* Encode into caller-owned command storage using mesa-switch unchanged.
 * This does not submit anything. The caller must bind Maxwell B on subchannel
 * 0, map the destination as GPU-uncached, and make CPU command writes visible
 * before submitting. A report is not a Horizon syncpoint or WDDM fence.
 */
bool tg_gpu_build_report(uint32_t *commands, size_t capacity_words,
                         uint64_t report_va, uint32_t value);

#endif
