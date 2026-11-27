/* SPDX-License-Identifier: MIT */
#ifndef TEGRA_GPU_QUERY_H
#define TEGRA_GPU_QUERY_H
#include "context.h"

/* Both entry points use the same GM20B memory and execution capabilities. */
DXGKDDI_QUERYADAPTERINFO TgGpuDdiQueryAdapterInfo;
DXGKDDI_QUERYADAPTERINFO TgGpuDdiQueryDisplayAdapterInfo;
void TgGpuRegisterQueryDdi(DRIVER_INITIALIZATION_DATA *);
#endif
