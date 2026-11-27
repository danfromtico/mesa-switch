/* SPDX-License-Identifier: MIT */
#ifndef TG_NVDEC_ADAPTER_H
#define TG_NVDEC_ADAPTER_H
#include "context.h"
#include "host1x_clock.h"
#include "nvdec_backend.h"

struct _TG_GPU_ADAPTER;
enum TG_NVDEC_HOST_CHECK {
    TgNvdecHostNoDisplay = 1,
    TgNvdecHostDisplaySyncpoint,
    TgNvdecHostChannelFifo,
    TgNvdecHostChannelPointers,
    TgNvdecHostSyncpointIrq,
    TgNvdecHostSyncpointValue,
    TgNvdecHostSyncpointChanged,
    TgNvdecHostClockChanged,
    TgNvdecHostChannelDmaState
};
typedef struct _TG_NVDEC_ADAPTER {
    KMUTEX Gate; /* lifecycle; always before Queue.DispatchGate */
    struct tg_nvdec_win *Backend;
    TG_HOST1X_CLOCK_LEASE *Clock;
    TG_GPU_DEVICE *Device; /* actual Engine.Devices reference */
    UINT64 Generation, Session;
    ULONG HostSource, PllpBase, CoreBootFloorUv;
    /* Failure-only register evidence, captured before releasing the lease.
     * Internal storage; the existing bounded error string carries the values. */
    /* Slots4/5 capture DMASTART/END only for a channel rejection. */
    ULONG HostCheck, HostIndex, HostValue[6];
    const char *Operation; /* static diagnostic label, never a borrowed buffer */
    volatile LONG Closing;
    NTSTATUS IoStatus;
    BOOLEAN Initialized;
    BOOLEAN ShutdownBounded, DispatchGateHeld;
} TG_NVDEC_ADAPTER;

void TgNvdecAdapterInitialize(struct _TG_GPU_ADAPTER *Adapter);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecAdapterStop(struct _TG_GPU_ADAPTER *Adapter);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgNvdecAdapterShutdown(struct _TG_GPU_ADAPTER *Adapter);
void TgNvdecRegisterEscape(DRIVER_INITIALIZATION_DATA *Ddis);
#endif
