/* SPDX-License-Identifier: MIT */
#ifndef TG_NVDEC_ADAPTER_H
#define TG_NVDEC_ADAPTER_H
#include "context.h"
#include "host1x_clock.h"
#include "nvdec_backend.h"

struct _TG_GPU_ADAPTER;
typedef struct _TG_NVDEC_ADAPTER {
    KMUTEX Gate; /* lifecycle; always before Queue.DispatchGate */
    struct tg_nvdec_win *Backend;
    TG_HOST1X_CLOCK_LEASE *Clock;
    TG_GPU_DEVICE *Device; /* actual Engine.Devices reference */
    UINT64 Generation, Session;
    ULONG HostSource, PllpBase;
    const char *Operation; /* static diagnostic label, never a borrowed buffer */
    volatile LONG Closing;
    NTSTATUS IoStatus;
    BOOLEAN Initialized;
} TG_NVDEC_ADAPTER;

void TgNvdecAdapterInitialize(struct _TG_GPU_ADAPTER *Adapter);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecAdapterStop(struct _TG_GPU_ADAPTER *Adapter);
void TgNvdecRegisterEscape(DRIVER_INITIALIZATION_DATA *Ddis);
#endif
