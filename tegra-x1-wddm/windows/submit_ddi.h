/* SPDX-License-Identifier: MIT */
#ifndef TG_GPU_SUBMIT_DDI_H
#define TG_GPU_SUBMIT_DDI_H
#include "context.h"
DXGKDDI_SUBMITCOMMANDVIRTUAL TgGpuDdiSubmitCommandVirtual;
DXGKDDI_PREEMPTCOMMAND TgGpuDdiPreemptCommand;
DXGKDDI_PRESENT TgGpuDdiPresent;
/* Library-only hookup; registration still requires complete IRQ/TDR/paging
 * paths and physical USER/privilege/residency validation. */
void TgGpuRegisterSubmissionDdi(DRIVER_INITIALIZATION_DATA *);
/* WDDM graphics registration, GDI copies and DMA-free panel flips. */
void TgGpuRegisterDisplaySubmissionDdis(DRIVER_INITIALIZATION_DATA *);
#endif
