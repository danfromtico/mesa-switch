/* SPDX-License-Identifier: MIT */
#ifndef TG_HOST1X_CLOCK_ABI_H
#define TG_HOST1X_CLOCK_ABI_H
#include <ntddk.h>

#define TG_HOST1X_CLOCK_ABI 0x00010002u
#define TG_HOST1X_IOCTL_CLOCK_HOLD 0x00222414u

/* Kernel-only, paired KMD/SwitchFan protocol. This is a pending power-managed
 * request, not a completed IOCTL used as an invented clock handle.
 *
 * Ready publishes the inherited gate/reset/PLLP snapshot and ReadyStatus.
 * The IRP stays pending until cancellation or provider power-down. The
 * provider calls Quiesce at PASSIVE_LEVEL before completing that IRP. The
 * callback must stop new work and prove its owned DMA quiescent, without
 * issuing requests back to the provider. Failure must retain DMA backing.
 *
 * The consumer owns all pointers until IRP completion and uses an unload-safe
 * completion routine. An acquisition timeout requests cancellation; it does
 * not release these objects. IRP cancellation alone is not DMA completion:
 * QuiesceStatus contains the actual callback result.
 */
typedef NTSTATUS (NTAPI *TG_HOST1X_QUIESCE)(PVOID Context);
typedef struct _TG_HOST1X_CLOCK_REQUEST {
    ULONG Version, Size;
    PKEVENT Ready;
    TG_HOST1X_QUIESCE Quiesce;
    PVOID Context;
    NTSTATUS ReadyStatus, QuiesceStatus;
    ULONG GateL, ResetL, PllpBase;
    ULONG Reserved;
    /* Existing core owner's retained boot floor and live readback, obtained
     * with SetVote=0. These are not a voltage vote or invented ownership. */
    ULONG CoreBootFloorUv, CoreAppliedUv;
} TG_HOST1X_CLOCK_REQUEST;

_Static_assert(sizeof(TG_HOST1X_CLOCK_REQUEST) == 64, "kernel clock request ABI");
static __inline BOOLEAN TgHost1xCoreFloorValid(ULONG floor, ULONG applied)
{
    return floor >= 600000u && floor <= 1050000u && applied >= floor &&
           applied <= 1050000u && !(floor % 12500u) && !(applied % 12500u);
}
#endif
