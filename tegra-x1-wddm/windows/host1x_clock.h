/* SPDX-License-Identifier: MIT */
#ifndef TG_HOST1X_CLOCK_H
#define TG_HOST1X_CLOCK_H
#include "platform_io.h"
#include "host1x_clock_abi.h"

typedef struct _TG_HOST1X_CLOCK_LEASE TG_HOST1X_CLOCK_LEASE;

/* Acquire and subsequent hardware starts are serialized against Quiesce by
 * the consumer. Quiesce first prevents new work, then drains only its owned
 * DMA with no calls back into SwitchFan. It can run before Acquire returns.
 * Every non-NULL result, including one returned on timeout, needs Release.
 * Optional Stage points to a static label for the last synchronous boundary;
 * it does not change the returned status or the lifetime of the lease.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgHost1xClockAcquire(TG_GPU_PLATFORM *Platform,
    TG_HOST1X_QUIESCE Quiesce, PVOID Context, TG_HOST1X_CLOCK_LEASE **Lease,
    const char **Stage);
BOOLEAN TgHost1xClockHeld(TG_HOST1X_CLOCK_LEASE *Lease, ULONG *PllpBase,
                        ULONG *CoreBootFloorUv);
/* Stop owned hardware first. Do not hold a lock needed by Quiesce while
 * waiting here. A timeout keeps *Lease and all callback objects alive.
 * A failed Quiesce result keeps the completed request as an ownership/error
 * record. An explicit later Release retries the actual consumer Quiesce
 * callback before discarding that record; it never rewrites the old result.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS TgHost1xClockRelease(TG_HOST1X_CLOCK_LEASE **Lease);
#endif
