/* SPDX-License-Identifier: MIT */
#ifndef TG_NVDEC_WINDOWS_MEMORY_H
#define TG_NVDEC_WINDOWS_MEMORY_H
#include "private_memory.h"
#include "../../src/nouveau/nvdec/nvdec.h"

/* Serialized by the decoder owner at PASSIVE_LEVEL. Dxgk must outlive every
 * allocation and failed free. This pool tracks OS allocations; it does not
 * establish a Tegra DMA route or prove that hardware has stopped using them. */
typedef struct _TG_NVDEC_MEMORY {
    const DXGKRNL_INTERFACE *Dxgk;
    LIST_ENTRY Allocations;
    NTSTATUS LastStatus;
    BOOLEAN Corrupt;
} TG_NVDEC_MEMORY;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecMemoryInit(TG_NVDEC_MEMORY *, const DXGKRNL_INTERFACE *);
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN TgNvdecMemoryAllocate(TG_NVDEC_MEMORY *, SIZE_T, struct tg_nvdec_dma *);
/* Caller has retired every hardware reference. On failure the pool retains
 * the real tracking handle, even if the codec discards its DMA descriptor. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID TgNvdecMemoryRetire(TG_NVDEC_MEMORY *, struct tg_nvdec_dma *);
/* Retry only already-retired records, never active hardware allocations. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecMemoryRetryRetired(TG_NVDEC_MEMORY *);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecMemoryFinish(TG_NVDEC_MEMORY *);
#endif
